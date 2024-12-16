#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include "parser.h"

void handle_pipeline(single_input *pipe_input);
void handle_subshell_command(parsed_input *input);
void handle_subshell(single_input *input);
void handle_subshell_pipe(single_input *input);
void handle_subshell_command_pipe(parsed_input *input);
void handle_parallel_subshell(parsed_input *input);

void handle_command(single_input *input){
    if (input == NULL || input->data.cmd.args[0] == NULL) {
        fprintf(stderr, "Invalid command.\n");
        return;
    }

    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
    } else if (pid == 0) {// Child process
        if (execvp(input->data.cmd.args[0], input->data.cmd.args) == -1) {
            perror("execvp");
            exit(EXIT_FAILURE);
        }
    } else {// Parent process.
        int status;
        do {
            waitpid(pid, &status, WUNTRACED);
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
    }
}

void handle_sequential(parsed_input *input){
    if (input == NULL) {
        fprintf(stderr, "Invalid input.\n");
        return;
    }

    for (int i = 0; i < input->num_inputs; i++) {

        if (input->inputs[i].type == INPUT_TYPE_COMMAND) {
            handle_command(&(input->inputs[i]));
        }
        else if (input->inputs[i].type == INPUT_TYPE_PIPELINE) {
            handle_pipeline(&(input->inputs[i]));
        }
        else {
            fprintf(stderr, "Unsupported input type for sequential execution.\n");
        }
    }
}

void execute_command(command *cmd) {
    if (execvp(cmd->args[0], cmd->args) == -1) {
        perror("execvp");
        exit(0);
    }
}

void handle_pipeline(single_input *pipe_input) {
    int i;
    int in_fd = 0;
    int fd[2];

    for (i = 0; i < pipe_input->data.pline.num_commands; i++) {

        if (i < pipe_input->data.pline.num_commands - 1) {
            if (pipe(fd) == -1) {
                perror("pipe");
                exit(EXIT_FAILURE);
            }
        }

        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            exit(EXIT_FAILURE);
        } else if (pid == 0) { // Child process
            if (in_fd != 0) {
                dup2(in_fd, STDIN_FILENO);
                close(in_fd);
            }
            if (i < pipe_input->data.pline.num_commands - 1) {
                close(fd[0]);
                dup2(fd[1], STDOUT_FILENO);
            } else {
                if (pipe_input->data.pline.num_commands > 1) {
                    close(fd[1]);
                }
            }
            execute_command(pipe_input->data.pline.commands + i);
            exit(EXIT_FAILURE);
        } else { // Parent process
            if (in_fd != 0) {
                close(in_fd);
            }
            if (i < pipe_input->data.pline.num_commands - 1) {
                close(fd[1]);
                in_fd = fd[0];
            } else {
                if (pipe_input->data.pline.num_commands > 1) {
                    close(fd[0]);
                }
            }
        }
    }
    while (wait(NULL) > 0);
}

void handle_pipeline_standalone(parsed_input *input){
    int i;
    int in_fd = 0; 
    int fd[2];     
    for (i = 0; i < input->num_inputs; i++) {
       
        if (i < input->num_inputs - 1) {
            if (pipe(fd) == -1) {
                perror("pipe");
                exit(EXIT_FAILURE);
            }
        }

        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            exit(EXIT_FAILURE);
        } else if (pid == 0) { // Child process
            if (in_fd != 0) {
                dup2(in_fd, STDIN_FILENO); 
                close(in_fd);
            }
            if (i < input->num_inputs - 1) {
                close(fd[0]); 
                dup2(fd[1], STDOUT_FILENO); 
            } else {
                if (input->num_inputs > 1) {
                    close(fd[1]);
                }
            }
            
            if(input->inputs[i].type == INPUT_TYPE_SUBSHELL){
                handle_subshell_pipe(&(input->inputs[i]));
            } else {
                execute_command(&(input->inputs[i].data.cmd));
            }

            exit(EXIT_FAILURE); 
        } else { // Parent process
            if (in_fd != 0) {
                close(in_fd); 
            }
            if (i < input->num_inputs - 1) {
                close(fd[1]); 
                in_fd = fd[0];
            } else {
                if (input->num_inputs > 1) {
                    close(fd[0]);
                }
            }
        }
    }

    while (wait(NULL) > 0);
}

void handle_parallel(parsed_input *input) {
    if (input == NULL) {
        fprintf(stderr, "Invalid input.\n");
        return;
    }

    int num_commands = input->num_inputs;
    pid_t pids[num_commands];
    int i;

    for (i = 0; i < num_commands; i++) {
        pids[i] = fork();

        if (pids[i] == -1) {
            perror("fork");
        } else if (pids[i] == 0) { // Child process

            if (input->inputs[i].type == INPUT_TYPE_COMMAND) {
                execute_command(&(input->inputs[i].data.cmd));
            }
            else if (input->inputs[i].type == INPUT_TYPE_PIPELINE) {
                handle_pipeline(&(input->inputs[i]));
            }
            else {
                fprintf(stderr, "Unsupported input type in parallel execution.\n");
            }
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < num_commands; i++) {
        if (pids[i] > 0) {
            waitpid(pids[i], NULL, 0);
        }
    }
}

void handle_subshell(single_input *input) {
    if (input == NULL || input->type != INPUT_TYPE_SUBSHELL) {
        fprintf(stderr, "Invalid subshell input.\n");
        return;
    }

    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
    } else if (pid == 0) { // Child process

        parsed_input subshell_input;
        if (parse_line(input->data.subshell, &subshell_input)) {
            handle_subshell_command(&subshell_input);
        } else {
            fprintf(stderr, "Subshell command parsing failed.\n");
        }
        exit(EXIT_FAILURE);
    } else { // Parent process
        waitpid(pid, NULL, 0);
    }
}

void handle_subshell_command(parsed_input *input) {
    switch (input->separator) {
        case SEPARATOR_PIPE:
             handle_pipeline_standalone(input);
            break;
        case SEPARATOR_SEQ:
            handle_sequential(input);
            break;
        case SEPARATOR_PARA:
            handle_parallel(input);
            break;
        default:
            if (input->num_inputs == 1) {
                if (input->inputs[0].type == INPUT_TYPE_COMMAND) {
                    handle_command(&(input->inputs[0]));
                }
            }
            break;
    }
}

void handle_subshell_pipe(single_input *input){
    if (input == NULL || input->type != INPUT_TYPE_SUBSHELL) {
        fprintf(stderr, "Invalid subshell input.\n");
        return;
    }
    parsed_input subshell_inp;
    if(parse_line(input->data.subshell, &subshell_inp)){
        handle_subshell_command_pipe(&subshell_inp);
    } 
}

void handle_subshell_command_pipe(parsed_input *input){

    switch (input->separator) {
        case SEPARATOR_PIPE:
             handle_pipeline_standalone(input);
            break;
        case SEPARATOR_SEQ:
            handle_sequential(input);
            break;
        case SEPARATOR_PARA:
            handle_parallel_subshell(input);
            break;
        default:
            if (input->num_inputs == 1) {
                if (input->inputs[0].type == INPUT_TYPE_COMMAND) {
                    handle_command(&(input->inputs[0]));
                }
            }
            break;
    }
}

void repeater_logic(int *write_fds, int num_commands) {
    signal(SIGPIPE, SIG_IGN);
    char buffer[256 * 1024];
    ssize_t nbytes;

    while ((nbytes = read(STDIN_FILENO, buffer, sizeof(buffer))) > 0) {
        for (int i = 0; i < num_commands; ++i) {
            write(write_fds[i], buffer, nbytes);
        }
    }
}

void handle_parallel_subshell(parsed_input *input) {
    if (input == NULL || input->num_inputs == 0) return;

    int num_commands = input->num_inputs;
    int write_fds[num_commands];
    pid_t pid;
    for (int i = 0; i < num_commands; ++i) {
        int fds[2];
        if (pipe(fds) < 0) {
            perror("pipe");
            exit(EXIT_FAILURE);
        }

        pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        } else if (pid == 0) { // Child process
            close(fds[1]);
            dup2(fds[0], STDIN_FILENO);
            close(fds[0]);

            if (input->inputs[i].type == INPUT_TYPE_COMMAND) {
                execute_command(&(input->inputs[i].data.cmd));
            } else if(input->inputs[i].type == INPUT_TYPE_PIPELINE) {
                handle_pipeline(&(input->inputs[i]));
            }
            exit(EXIT_FAILURE);
        } else { // Parent process
            close(fds[0]);
            write_fds[i] = fds[1];
        }
    }

    if(pid > 0) {
        repeater_logic(write_fds, num_commands);

        for (int j = 0; j < num_commands; ++j) {
            close(write_fds[j]);
        }

        while (wait(NULL) > 0);
    }
}

int main() {
    char line[INPUT_BUFFER_SIZE];

    while (1) {
        printf("/> ");
        fflush(stdout);

        if (fgets(line, INPUT_BUFFER_SIZE, stdin) == NULL) {
            if (feof(stdin)) {
                printf("\nEOF detected. Exiting eshell.\n");
                break;
            }
            printf("Error reading input. Exiting eshell.\n");
            break;
        }

        line[strcspn(line, "\n")] = 0;

        if (strcmp(line, "quit") == 0) {
            break;
        }

        parsed_input input;

        if (parse_line(line, &input)) {
            
            switch (input.separator) {
                case SEPARATOR_PIPE:
                    handle_pipeline_standalone(&input);
                    break;
                case SEPARATOR_SEQ:
                    handle_sequential(&input);
                    break;
                case SEPARATOR_PARA:
                    handle_parallel(&input);
                    break;
                default:
                    if (input.num_inputs == 1 && input.inputs[0].type == INPUT_TYPE_COMMAND) {
                        handle_command(&input.inputs[0]);
                    } else if (input.num_inputs == 1 && input.inputs[0].type == INPUT_TYPE_SUBSHELL) {
                        handle_subshell(&input.inputs[0]);
                    }
                    break;
            }
        } else {
            continue;
        }
        free_parsed_input(&input);
    }

    return 0;
}