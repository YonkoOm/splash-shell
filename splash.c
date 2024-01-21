#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int lexer(char *line, char ***args, int *num_args) {
  *num_args = 0;
  // count number of args
  char *l = strdup(line);
  if (l == NULL) {
    return -1;
  }
  char *token = strtok(l, " \t\n");
  while (token != NULL) {
    (*num_args)++;
    token = strtok(NULL, " \t\n");
  }
  free(l);
  // split line into args
  *args = malloc(sizeof(char **) * *num_args);
  *num_args = 0;
  token = strtok(line, " \t\n");
  while (token != NULL) {
    char *token_copy = strdup(token);
    if (token_copy == NULL) {
      return -1;
    }
    (*args)[(*num_args)++] = token_copy;
    token = strtok(NULL, " \t\n");
  }
  return 0;
}

/* A helper function that runs the passed in command in the child process after
 * forking the main process. If any file descriptors other than the normal input
 * and output file descriptors are passed in, we duplicate the file
 * descriptor(s) onto either 0 (stdin) or 1 (stdout) or both. If the output fd
 * differs, we also change duplicate the stderr to that passed in fd */
int run_cmd(int input, int output, char **cmd) {
  pid_t pid = fork();
  if (pid == -1) { // checks if fork() failed
    return -1;
  } else if (pid == 0) { // child process
    if (input != STDIN_FILENO) {
      if (dup2(input, STDIN_FILENO) == -1) {
        close(input);
        close(output);
        return -1;
      }
      close(input);
    }

    if (output != STDOUT_FILENO) {
      if (dup2(output, STDOUT_FILENO) == -1 ||
          dup2(output, STDERR_FILENO) == -1) {
        close(output);
        return -1;
      }
      close(output);
    }

    execv(cmd[0], cmd);
    exit(3); // exit with code 3 if execv fails
  } else {   // parent process
    int status;
    wait(&status);
    if (WEXITSTATUS(status) == 3)
      return -1;
  }
  return 0;
}

// changes the current directory
int cd(char **cmd) {
  char *path = cmd[1], *cmd_after_path = cmd[2];
  if (!path || cmd_after_path) // only one arg needed (the path)
    return -1;
  return chdir(path);
}

// prints the path of the current directory
int pwd(char **cmd) {
  char *arg = cmd[1];
  if (arg) // pwd should shouldn't be followed by any arguments
    return -1;

  int size = 256;
  char *cwd = malloc(size);

  while (!getcwd(cwd, size)) {
    if (errno == ERANGE) { // errno being set to ERANGE indicates range error
      size *= 2;
      char *temp = cwd;
      if (!(cwd = realloc(cwd, size))) {
        free(temp);
        return -1;
      }
    } else {
      free(cwd);
      return -1;
    }
  }

  printf("%s\n", cwd);
  fflush(stdout); // immediately flush output to console

  free(cwd);
  return 0;
}

// redirects the output to the file specified instead of the console
int redirection(char **cmd, int num_args) {
  char **redir_cmd = malloc((num_args + 1) * sizeof(char *));
  int i = 0;

  while (strcmp(cmd[i], ">") != 0) {
    redir_cmd[i] = cmd[i];
    i++;
  }
  redir_cmd[i] = NULL;

  char *file = cmd[++i];

  int fd = open(file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd == -1) {
    free(redir_cmd);
    return -1;
  }

  int rc = run_cmd(0, fd, redir_cmd);

  close(fd);
  free(redir_cmd);
  return rc;
}

int run_pipe(char **cmd, int num_args, int is_redir) {
  int i = 0, num_pipes = 0;

  while (cmd[i])
    if (strcmp(cmd[i++], "|") == 0)
      num_pipes++;

  i = 0;
  int pipe_fd[2], prev_in = 0, curr_index = 0, pipes_visited = 0;
  char **pipe_cmd = malloc((num_args + 1) * sizeof(char *));
  if (!pipe_cmd)
    return -1;

  while (pipes_visited != num_pipes) { // runs all commands before the last pipe
    if (strcmp(cmd[i], "|") == 0) {
      pipes_visited++;
      pipe_cmd[curr_index] = NULL;
      curr_index = 0;

      pipe(pipe_fd);

      if (run_cmd(prev_in, pipe_fd[1], pipe_cmd) == -1) {
        free(pipe_cmd);
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return -1;
      }
      close(pipe_fd[1]); // close write end of pipe as no longer needed
      if (prev_in != 0)
        close(prev_in);

      prev_in = pipe_fd[0]; // stores read end in pipe for future iterations
    } else {
      pipe_cmd[curr_index++] = cmd[i];
    }
    i++;
  }

  curr_index = 0;

  while (cmd[i]) { // gets command after the last pipe
    if (strcmp(cmd[i], ">") == 0)
      break;
    pipe_cmd[curr_index++] = cmd[i++];
  }
  pipe_cmd[curr_index] = NULL;

  int rc;

  // run the command after the last pipe
  if (is_redir) { // if is_redir is true, redirects pipe command to file
    char *file = cmd[++i];
    int fd = open(file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd == -1) {
      free(pipe_cmd);
      return -1;
    }
    rc = run_cmd(prev_in, fd, pipe_cmd);
    close(fd);
  } else { // otherwise, it just outputs to console
    rc = run_cmd(prev_in, 1, pipe_cmd);
  }
  close(prev_in);

  if (rc == -1) {
    free(pipe_cmd);
    return -1;
  }

  free(pipe_cmd);
  return 0;
}

int loop(char **cmd, int num_args, int is_redir, int is_pipe) {
  char *num = cmd[1], *cmd_to_run = cmd[2];
  if (!num || !cmd_to_run)
    return -1;

  for (int i = 0; i < strlen(num); i++)
    if (!isdigit(num[i]))
      return -1;

  int num_loops = atoi(cmd[1]);

  for (int i = 0; i < num_loops; i++) {
    if (is_pipe) {
      if (run_pipe(cmd + 2, num_args, is_redir) == -1)
        return -1;
    } else if (is_redir) {
      if (redirection(cmd + 2, num_args) == -1)
        return -1;
    } else if (strcmp(cmd_to_run, "cd") == 0) {
      if (cd(cmd + 2) == -1)
        return -1;
    } else if (strcmp(cmd_to_run, "pwd") == 0) {
      if (pwd(cmd + 2) == -1)
        return -1;
    } else {
      if (run_cmd(0, 1, cmd + 2) == -1)
        return -1;
    }
  }
  return 0;
}

/* looks through the current command to look for a redirection request (1 if
 * exists). If redir exists, makes look for any syntactical errors
 */
int find_redir(char **cmd) {
  int i = 0, redir_count = 0, is_redir = 0, redir_index = 0;
  while (cmd[i]) {
    for (int j = 0; j < strlen(cmd[i]); j++) {
      if (cmd[i][j] == '>') {
        if (++redir_count > 1) // check only a single redirection in cmd
          return -1;
        is_redir = 1;
        redir_index = i;
      }
    }
    i++;
  }

  if (is_redir) {
    char *file = cmd[redir_index + 1];
    char *cmd_after_file = cmd[redir_index + 2];
    if (redir_index == 0 || !file ||
        (cmd_after_file && strcmp(cmd_after_file, ";") != 0))
      return -1;
    return 1;
  }

  return 0;
}

/* looks through the current command to look for any pipes (returns 1 if
 * exists). If pipe exits, checks for any pipe-related syntax errors (returns -1
 * if any) */
int find_pipe(char **cmd) {
  int is_pipe = 0, pipe_index = 0;

  for (int i = 0; cmd[i]; i++) {
    if (strcmp(cmd[i], "|") == 0) {
      char *cmd_after_pipe = cmd[i + 1];
      if (i == 0 || !cmd_after_pipe)
        return -1;
      is_pipe = 1;
      pipe_index = i;
    } else if (strcmp(cmd[i], ">") == 0) {
      int redir_index = i;
      if (is_pipe && (redir_index < pipe_index))
        return -1;
    }
  }
  return is_pipe;
}

// frees all the allocated commands and arguments
void free_args(char **args, int num_args) {
  for (int i = 0; i < num_args; i++)
    free(args[i]);
  free(args);
}

int main(int argc, char **argv) {
  char error_message[30] = "An error has occurred\n";
  char *line;
  size_t len;

  while (1) {
    printf("splash> ");
    fflush(stdout); // immediately flush "splash> " to stdout

    line = NULL;
    len = 0;

    if (getline(&line, &len, stdin) == -1) {
      free(line);
      write(STDERR_FILENO, error_message, strlen(error_message));
    }

    char **args = NULL;
    int num_args = 0;

    // splits the commands and args
    if (lexer(line, &args, &num_args) == -1) {
      free(line);
      write(STDERR_FILENO, error_message, strlen(error_message));
    }
    free(line);

    if (!num_args)
      continue;

    int rc;          // return code
    char **curr_cmd; // pointer to char pointers (aka pointer to array strings)

    for (int i = 0; i < num_args; i++) {
      rc = 0;
      curr_cmd = malloc((num_args + 1) * sizeof(char *));
      if (!curr_cmd)
        exit(EXIT_FAILURE);

      int curr_index = 0;

      /* insert args into current command buffer until we reach the last arg or
       * hit a semicolon */
      while (i < num_args && strcmp(args[i], ";") != 0)
        curr_cmd[curr_index++] = args[i++];
      curr_cmd[curr_index] = NULL;

      char *is_arg = curr_cmd[0];
      if (!is_arg) { // i.e. we only have a semicolon
        free(curr_cmd);
        continue;
      }

      int is_redir = find_redir(curr_cmd), is_pipe = find_pipe(curr_cmd);

      if (is_redir == -1 || is_pipe == -1) {
        write(STDERR_FILENO, error_message, strlen(error_message));
        break;
      }

      // series of if-statements check which command to run
      if (strcmp(curr_cmd[0], "exit") == 0) {
        char *arg = curr_cmd[1];
        if (!arg) {
          free(curr_cmd);
          free_args(args, num_args);
          exit(EXIT_SUCCESS);
        }
        rc = -1;
      } else if (strcmp(curr_cmd[0], "loop") == 0) {
        rc = loop(curr_cmd, num_args, is_redir, is_pipe);
      } else if (is_pipe) {
        rc = run_pipe(curr_cmd, num_args, is_redir);
      } else if (is_redir) {
        rc = redirection(curr_cmd, num_args);
      } else if (strcmp(curr_cmd[0], "cd") == 0) {
        rc = cd(curr_cmd);
      } else if (strcmp(curr_cmd[0], "pwd") == 0) {
        rc = pwd(curr_cmd);
      } else {
        rc = run_cmd(STDIN_FILENO, STDOUT_FILENO, curr_cmd);
      }
      free(curr_cmd);

      if (rc == -1) { // non-program related errors throw an error message
        write(STDERR_FILENO, error_message, strlen(error_message));
        break;
      }
    }
    free_args(args, num_args);
  }
  return 0;
}
