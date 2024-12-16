#include <queue>
#include <iostream>
#include <sstream>
#include <pthread.h>
#include "monitor.h"
#include "WriteOutput.h"
#include "helper.h"

// Function for resetting timestamp for timeout.
void resetTimestamp(struct timespec* timestamp, int maxWaitTime) {
    clock_gettime(CLOCK_REALTIME, timestamp);
    timestamp->tv_sec += maxWaitTime / 1000;
    timestamp->tv_nsec += (maxWaitTime % 1000) * 1000000;
    if (timestamp->tv_nsec >= 1000000000) {
        timestamp->tv_sec++;
        timestamp->tv_nsec -= 1000000000;
    }

}

struct PathSegment {
    char type;
    int id;
    int from;
    int to;

    PathSegment(char t, int i, int f, int to_dir) : type(t), id(i), from(f), to(to_dir) {}
};


class NarrowBridge : public Monitor {
private:
    struct timespec *timeout[2];
    int connectorID;
    int travelTime;
    int maxWaitTime;
    int currentDirection; // 0 or 1 indicating the current allowed direction
    int carsOnBridge; // Number of cars currently on the bridge
    std::queue<int> queues[2]; // Two queues for each direction
    Condition *canPass[2]; // Condition variables for each direction

public:
    NarrowBridge(int id, int travelTime, int maxWaitTime)
        : connectorID(id), travelTime(travelTime), maxWaitTime(maxWaitTime), currentDirection(-1), carsOnBridge(0) {
        canPass[0] = new Condition(this);
        canPass[1] = new Condition(this);
        timeout[0] = new timespec();
        timeout[1] = new timespec();
    }

    void Pass(int carID, int direction) {
        __synchronized__;
        // Enqueue the car in the appropriate queue
        WriteOutput(carID, 'N', connectorID, ARRIVE);

        queues[direction].push(carID);

        if(currentDirection == -1){
            currentDirection = direction;
        }

        if(queues[direction].front() == carID){
            // Wait for the timeout (if it expires, switch direction and notify all cars in the new direction
            resetTimestamp(timeout[direction], maxWaitTime);
        }

        while(true){
            // Wait until it's this car's turn to pass and the direction is correct
            while (queues[currentDirection].front() != carID || direction != currentDirection) {
                if(direction == currentDirection){
                    canPass[direction]->wait();
                } else {
                    int wait_result = canPass[direction]->timedwait(timeout[direction]);
                    if (wait_result == ETIMEDOUT && direction != currentDirection && queues[direction].front() == carID) {
                        currentDirection = 1 - currentDirection; // Switch direction
                        while(carsOnBridge > 0){
                            canPass[direction]->wait(); // Wait for the cars on the bridge to finish passing
                        }
                        // Reset the timeout and notify cars in the new direction
                        resetTimestamp(timeout[1 - currentDirection], maxWaitTime);
                        canPass[1 - currentDirection]->notifyAll(); // Wake up cars in the new direction
                        canPass[currentDirection]->notifyAll(); // Wake up cars in the new direction
                    }
                }
            }
            
            // Start passing

            if(carsOnBridge > 0){
                mutex.unlock();
                sleep_milli(PASS_DELAY); // Simulate the time it takes to start passing
                mutex.lock(); // Lock the monitor again
            }

            if(direction != currentDirection){
                continue;
            }
            
            queues[direction].pop(); // Remove car from the queue

            canPass[currentDirection]->notifyAll(); // Notify all other cars

            WriteOutput(carID, 'N', connectorID, START_PASSING);
            carsOnBridge++;
            mutex.unlock(); // Unlock the monitor before passing
            sleep_milli(travelTime);
            mutex.lock(); // Lock the monitor again
            carsOnBridge--;
            WriteOutput(carID, 'N', connectorID, FINISH_PASSING);

            // Check if queue is empty and change direction if necessary
            if (queues[currentDirection].empty() && !queues[1 - currentDirection].empty() && carsOnBridge == 0) {
                
                currentDirection = 1 - currentDirection; // Switch direction

                // Reset the timeout and notify cars in the new direction
                resetTimestamp(timeout[1 - currentDirection], maxWaitTime);
                canPass[1 - currentDirection]->notifyAll(); // Wake up cars in the new direction
                canPass[currentDirection]->notifyAll(); // Wake up cars in the new direction
            }
            else if(queues[currentDirection].empty() && queues[1 - currentDirection].empty()){
                currentDirection = -1;
            }
            else {
                // Notify the next car in the current direction
                if (!queues[currentDirection].empty()) {
                    canPass[currentDirection]->notifyAll();
                }
            }
            break;
        }
        
    }
};


class Ferry : public Monitor {
private:
    int connectorID;
    int travelTime;
    int maxWaitTime;
    int capacity;
    int carsOnFerry[2];
    Condition *readyToDepart[2]; // Condition variables to manage departure for each side
    struct timespec *departureTime[2]; // Time when the ferry should depart if not full for each side

public:
    Ferry(int id, int travelTime, int maxWaitTime, int capacity)
        : connectorID(id), travelTime(travelTime), maxWaitTime(maxWaitTime), capacity(capacity) {
        for (int i = 0; i < 2; ++i) {
            readyToDepart[i] = new Condition(this);
            carsOnFerry[i] = 0;
            departureTime[i] = new timespec();
        }
    }

    void Pass(int carID, int side) {
        __synchronized__;

        WriteOutput(carID, 'F', connectorID, ARRIVE);

        carsOnFerry[side]++;

        if(carsOnFerry[side] == 1){
            resetTimestamp(departureTime[side], maxWaitTime);
        }

        while(true){
            if(carsOnFerry[side] < capacity){
                
                int wait_result = readyToDepart[side]->timedwait(departureTime[side]);
                if(wait_result == ETIMEDOUT){
                    carsOnFerry[side] = 0;
                    readyToDepart[side]->notifyAll();
                    
                }
                WriteOutput(carID, 'F', connectorID, START_PASSING);
                mutex.unlock();
                sleep_milli(travelTime);
                mutex.lock();
                WriteOutput(carID, 'F', connectorID, FINISH_PASSING);
                break;
            }
            else{
                //Ferry wil pass now
                carsOnFerry[side] = 0;
                readyToDepart[side]->notifyAll();
                WriteOutput(carID, 'F', connectorID, START_PASSING);
                mutex.unlock();
                sleep_milli(travelTime);
                mutex.lock();                
                WriteOutput(carID, 'F', connectorID, FINISH_PASSING);
                break;
            }
        }

        
    }
};


class Crossroad : public Monitor {
private:
    int timingOut;
    int connectorID;
    int travelTime;
    int maxWaitTime;
    int currentDirection; // 0 to 3 indicating the current allowed direction
    std::queue<int> queues[4]; // Four queues for each direction (N, S, E, W)
    Condition *canPass[4]; // Condition variables for each direction
    struct timespec *timeout;
    int carsOnCross; // Number of cars currently on the crossroad

public:
    Crossroad(int id, int travelTime, int maxWaitTime)
        : connectorID(id), travelTime(travelTime), maxWaitTime(maxWaitTime), currentDirection(-1), carsOnCross(0), timingOut(-1){
        for (int i = 0; i < 4; i++) {
            canPass[i] = new Condition(this);
        }
        timeout = new timespec();
    }

    void Pass(int carID, int direction) {
        __synchronized__;
        // Enqueue the car in the appropriate queue
        WriteOutput(carID, 'C', connectorID, ARRIVE);
        queues[direction].push(carID);

        if (currentDirection == -1) {
            currentDirection = direction;
        }


        if(currentDirection != direction){
            if(queues[direction].front() == carID){
                bool reset_time = true;
                for(int i = 0; i < 4; i++){
                    if(i != currentDirection && i != direction){
                         if(!queues[i].empty()){
                            reset_time = false;
                            break;
                         }
                    }
                }
                if(reset_time){
                    resetTimestamp(timeout, maxWaitTime);
                    timingOut = carID;
                }
            }
        }

        while(true){
            // Wait until it's this car's turn to pass and the direction is correct
            while (queues[currentDirection].front() != carID || direction != currentDirection) {
                
                if(direction == currentDirection){//Not in front of the queue.            
                    canPass[direction]->wait();                    
                } else if(timingOut == carID) {
                    int wait_result = canPass[direction]->timedwait(timeout);
                    if (wait_result == ETIMEDOUT && direction != currentDirection && queues[direction].front() == carID) {
                        printf("Timeout for car %d at crossroad %d\n", carID, connectorID);
                        //Find the next direction with cars
                        if(!queues[(currentDirection + 1) % 4].empty()){
                            currentDirection = (currentDirection + 1) % 4; // Switch direction cyclically
                            printf("Switching direction to %d\n", currentDirection);
                            resetTimestamp(timeout, maxWaitTime);
                        }
                        else if(!queues[(currentDirection + 2) % 4].empty()){
                            currentDirection = (currentDirection + 2) % 4; // Switch direction cyclically
                            printf("Switching direction to %d\n", currentDirection);
                            resetTimestamp(timeout, maxWaitTime);
                        }
                        else if (!queues[(currentDirection + 3) % 4].empty()){
                            currentDirection = (currentDirection + 3) % 4; // Switch direction cyclically
                            printf("Switching direction to %d\n", currentDirection);
                            resetTimestamp(timeout, maxWaitTime);
                        }

                        for(int i = 0; i < 4; i++){
                            if(i != currentDirection && !queues[i].empty()){
                                timingOut = queues[i].front();
                                break;
                            }
                        }
                        for(int i = 0; i < 4; i++){
                            if(i != currentDirection){
                                canPass[i]->notifyAll(); // Wake up cars in the new direction
                            }
                        }
                        
                        while(carsOnCross > 0){
                            canPass[currentDirection]->wait(); // Wait for the cars on the bridge to finish passing
                        }
                        canPass[currentDirection]->notifyAll(); // Wake up cars in the new direction
                    }
                }
                else{
                    canPass[direction]->wait();
                }
            }

            // Start passing

            if (carsOnCross > 0) {
                mutex.unlock();
                sleep_milli(PASS_DELAY); // Simulate the time it takes to start passing
                mutex.lock(); // Lock the monitor again
            }

            if(direction != currentDirection){

                continue;
            }

            queues[direction].pop(); // Remove car from the queue

            canPass[direction]->notifyAll(); // Notify all other cars

            WriteOutput(carID, 'C', connectorID, START_PASSING);
            carsOnCross++;
            mutex.unlock(); // Unlock the monitor before passing
            sleep_milli(travelTime);
            mutex.lock(); // Lock the monitor again
            carsOnCross--;
            WriteOutput(carID, 'C', connectorID, FINISH_PASSING);

            // Check if queue is empty and change direction if necessary
            if (queues[currentDirection].empty() && carsOnCross == 0) {
                    int oldDirection = currentDirection;
                if(!queues[(currentDirection + 1) % 4].empty()){
                    currentDirection = (currentDirection + 1) % 4; // Switch direction cyclically
                    resetTimestamp(timeout, maxWaitTime);
                    for(int i = 0; i < 4; i++){
                        if(i != currentDirection && !queues[i].empty()){
                            timingOut = queues[i].front();
                            break;
                        }
                    }
                    canPass[0]->notifyAll(); // Wake up cars in the new direction
                    canPass[1]->notifyAll(); // Wake up cars in the new direction
                    canPass[2]->notifyAll(); // Wake up cars in the new direction
                    canPass[3]->notifyAll(); // Wake up cars in the new direction
                }
                else if(!queues[(currentDirection + 2) % 4].empty()){
                    currentDirection = (currentDirection + 2) % 4; // Switch direction cyclically
                    resetTimestamp(timeout, maxWaitTime);
                    for(int i = 0; i < 4; i++){
                        if(i != currentDirection && !queues[i].empty()){
                            timingOut = queues[i].front();
                            break;
                        }
                    }
                    canPass[0]->notifyAll(); // Wake up cars in the new direction
                    canPass[1]->notifyAll(); // Wake up cars in the new direction
                    canPass[2]->notifyAll(); // Wake up cars in the new direction
                    canPass[3]->notifyAll(); // Wake up cars in the new direction
                }
                else if (!queues[(currentDirection + 3) % 4].empty()){
                    currentDirection = (currentDirection + 3) % 4; // Switch direction cyclically
                    resetTimestamp(timeout, maxWaitTime);
                    for(int i = 0; i < 4; i++){
                        if(i != currentDirection && !queues[i].empty()){
                            timingOut = queues[i].front();
                            break;
                        }
                    }
                    canPass[0]->notifyAll(); // Wake up cars in the new direction
                    canPass[1]->notifyAll(); // Wake up cars in the new direction
                    canPass[2]->notifyAll(); // Wake up cars in the new direction
                    canPass[3]->notifyAll(); // Wake up cars in the new direction
                }
                else {
                    currentDirection = -1; 
                }
            } else if (!queues[currentDirection].empty()) {
                canPass[currentDirection]->notify(); // Notify the next car in the current direction
            }
            break;
        }
    }
};

std::vector<Crossroad> crossroads;
std::vector<Ferry> ferries;
std::vector<NarrowBridge> narrowBridges;

class Car {
public:
    int carID;
    int travelTime;
    int pathLength;
    std::vector<PathSegment> path; // Vector of path segments

    Car(int id, int tTime, int pLength, const std::vector<PathSegment>& p) : carID(id), travelTime(tTime), pathLength(pLength), path(p) {}

    void operate() {
        for (const auto& segment : path) {

            // Simulate travel time to the connector
            WriteOutput(carID, segment.type, segment.id, TRAVEL);
            sleep_milli(travelTime);

            // Depending on the type of connector, call the appropriate Pass function
            switch (segment.type) {
                case 'C': // Crossroad
                    crossroads[segment.id].Pass(carID, segment.from);
                    break;
                case 'F': // Ferry
                    ferries[segment.id].Pass(carID, segment.from);
                    break;
                case 'N': // NarrowBridge
                    narrowBridges[segment.id].Pass(carID, segment.to);
                    break;
                default:
                    std::cerr << "Unknown connector type: " << segment.type << std::endl;
            }
        }
    }
};

void carThreadFunction(Car car) {
    car.operate();
}

struct ThreadData {
    Car* car;
};

void* carThreadFunction(void* arg) {
    ThreadData* data = (ThreadData*)arg;
    Car* car = data->car;
    car->operate();
    delete data;  // Clean up the allocated memory
    return nullptr;
}

int main() {
    int NC, NF, NN;  // Number of crossroads, ferries, and narrow bridges

    std::cin >> NN;

    // Initialize Narrow Bridges
    for (int i = 0; i < NN; ++i) {
        int travelTime, maxWaitTime;
        std::cin >> travelTime >> maxWaitTime;
        narrowBridges.emplace_back(i, travelTime, maxWaitTime);
    }

    std::cin >> NF;

    // Initialize Ferries
    for (int i = 0; i < NF; ++i) {
        int travelTime, maxWaitTime, capacity;
        std::cin >> travelTime >> maxWaitTime >> capacity;
        ferries.emplace_back(i, travelTime, maxWaitTime, capacity);
    }

    std::cin >> NC;

    // Initialize Crossroads
    for (int i = 0; i < NC; ++i) {
        int travelTime, maxWaitTime;
        std::cin >> travelTime >> maxWaitTime;
        crossroads.emplace_back(i, travelTime, maxWaitTime);
    }


    int N;  // Number of cars
    std::cin >> N;
    std::vector<pthread_t> carThreads(N);

    InitWriteOutput();  // Initialize the output writer

    // Initialize Cars
    for (int i = 0; i < N; ++i) {
        int travelTime;
        int pathLength;
        std::cin >> travelTime >> pathLength;

        std::vector<PathSegment> path;

        std::string typeID;
        int from, to;
        std::string pathStream;
        for(int i = 0; i < pathLength; i++) {
            std::cin >> typeID >> from >> to;
            char type = typeID[0];
            int id = std::stoi(typeID.substr(1));
            path.emplace_back(type, id, from, to);
        }

     
        Car* car = new Car(i, travelTime, pathLength, path);  // Create a car dynamically
        ThreadData* data = new ThreadData;
        data->car = car;

        pthread_create(&carThreads[i], nullptr, carThreadFunction, data);
    }

    // Join all threads
    for (auto& thread : carThreads) {
        pthread_join(thread, nullptr);
    }

    return 0;
}