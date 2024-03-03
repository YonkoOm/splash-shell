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

/* A helper function that runs the command in a child process. If file
 * descriptors other than the normal input and output fds are passed in, we
 * duplicate the file descriptor(s) onto 0 (stdin), 1 (stdout) or both. If the
 * output fd differs, we also duplicate output fd onto stderr */
int run_cmd(int input, int output, char **cmd) {
  pid_t pid = fork();
  if (pid == -1) { // fork failure
    return -1;
  } else if (pid == 0) { // child process
    if (input != STDIN_FILENO) {
      if (dup2(input, STDIN_FILENO) == -1) {
        close(input);
        close(output);
        return -1;
      }
      close(input); // close read pipe fd as no longer neeeded
    }

    if (output != STDOUT_FILENO) {
      if (dup2(output, STDOUT_FILENO) == -1 ||
          dup2(output, STDERR_FILENO) == -1) {
        close(output);
        return -1;
      }
      close(output); // close write end fd as no longer needed
    }

    execv(cmd[0], cmd);
    exit(3); // if execv fails, exit with code 3
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
  char *path = cmd[1];
  if (!path)
    return -1;
  char *cmd_after_path = cmd[2];
  if (cmd_after_path)
    return -1;

  return chdir(path);
}

// prints the path of the current directory
int pwd(char **cmd) {
  char *cmd_after_pwd = cmd[1];
  if (cmd_after_pwd)
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
  char **curr_cmd = malloc((num_args + 1) * sizeof(char *));
  if (!curr_cmd)
    return -1;

  int i = 0;
  while (strcmp(cmd[i], ">") != 0) {
    curr_cmd[i] = cmd[i];
    i++;
  }
  curr_cmd[i] = NULL;

  char *file = cmd[++i];
  int fd = open(file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd == -1) {
    free(curr_cmd);
    return -1;
  }
  int rc = run_cmd(0, fd, curr_cmd);

  close(fd);
  free(curr_cmd);
  return rc;
}

int run_pipe(char **cmd, int num_args, int is_redir) {
  int i = 0, num_pipes = 0;
  while (cmd[i])
    if (strcmp(cmd[i++], "|") == 0)
      num_pipes++;

  char **curr_cmd = malloc((num_args + 1) * sizeof(char *));
  if (!curr_cmd)
    return -1;

  int pipefds[2], read_pipe = STDIN_FILENO, curr_index = 0, pipes_visited = 0;
  i = 0;
  while (pipes_visited != num_pipes) { // run commands b/f the last pipe
    if (strcmp(cmd[i], "|") == 0) {
      pipes_visited++;
      curr_cmd[curr_index] = NULL;
      curr_index = 0;

      pipe(pipefds);

      if (run_cmd(read_pipe, pipefds[1], curr_cmd) == -1) {
        free(curr_cmd);
        close(pipefds[0]);
        close(pipefds[1]);
        return -1;
      }
      close(pipefds[1]); // close write end of pipe as no longer needed
      if (read_pipe != 0)
        close(read_pipe);

      read_pipe = pipefds[0]; // stores read end of pipe for future iterations
    } else {
      curr_cmd[curr_index++] = cmd[i];
    }
    i++;
  }

  curr_index = 0;
  while (cmd[i] && strcmp(cmd[i], ">") != 0) // gets command after the last pipe
    curr_cmd[curr_index++] = cmd[i++];
  curr_cmd[curr_index] = NULL;

  int rc;
  // run the command after the last pipe
  if (is_redir) { // if true, output moves to file
    char *file = cmd[++i];
    int fd = open(file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd == -1) {
      free(curr_cmd);
      return -1;
    }
    rc = run_cmd(read_pipe, fd, curr_cmd);
    close(fd);
  } else { // otherwise, it just outputs to console
    rc = run_cmd(read_pipe, STDOUT_FILENO, curr_cmd);
  }
  if (rc == -1) {
    free(curr_cmd);
    return -1;
  }

  close(read_pipe);
  free(curr_cmd);
  return 0;
}

int loop(char **cmd, int num_args, int is_redir, int is_pipe) {
  char *num = cmd[1];
  if (!num) // check for num
    return -1;
  char *cmd_to_run = cmd[2];
  if (!cmd_to_run) // if num exists, check for command
    return -1;

  for (size_t i = 0; i < strlen(num); i++)
    if (!isdigit(num[i]))
      return -1;

  int rc;
  int num_loops = atoi(cmd[1]);
  for (int i = 0; i < num_loops; i++) {
    rc = 0;
    if (is_pipe) {
      rc = run_pipe(cmd + 2, num_args, is_redir);
    } else if (is_redir) {
      rc = redirection(cmd + 2, num_args);
    } else if (strcmp(cmd_to_run, "cd") == 0) {
      rc = cd(cmd + 2);
    } else if (strcmp(cmd_to_run, "pwd") == 0) {
      rc = pwd(cmd + 2);
    } else {
      rc = run_cmd(STDIN_FILENO, STDOUT_FILENO, cmd + 2);
    }
    if (rc == -1)
      return rc;
  }
  return 0;
}

/* looks through the current command to look for a redirection request (1 if
 * exists). If redir exists, makes look for any syntactical errors
 */
int find_redir(char **cmd) {
  int i = 0, index = 0, count = 0;
  while (cmd[i]) {
    for (size_t j = 0; j < strlen(cmd[i]); j++) {
      if (cmd[i][j] == '>') {
        if (++count > 1 || i == 0)
          return -1;
        index = i;
      }
    }
    i++;
  }

  if (index > 0) {
    char *file = cmd[index + 1];
    char *extra_arg = cmd[index + 2];
    if (!file || extra_arg)
      return -1;
    return 1;
  }
  return 0;
}

/* looks through the current command to look for any pipes (returns 1 if
 * exists). If pipe exists, checks for any pipe-related syntax errors (returns
 * -1 if any) */
int find_pipe(char **cmd) {
  int i = 0, index = 0;
  while (cmd[i]) {
    if (strcmp(cmd[i], "|") == 0) {
      char *second_command = cmd[i + 1];
      if (i == 0 || !second_command)
        return -1;
      index = i;
    } else if (strcmp(cmd[i], ">") == 0) {
      if (index > 0 && (i < index)) // return -1 if redir bf pipe
        return -1;
    }
    i++;
  }
  return index > 0;
}

// frees all the allocated commands and arguments
void free_args(char **args, int num_args) {
  for (int i = 0; i < num_args; i++)
    free(args[i]);
  free(args);
}

int main() {
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
      if (feof(stdin))
        exit(EXIT_SUCCESS);
      write(STDERR_FILENO, error_message, strlen(error_message));
    }

    char **args = NULL;
    int num_args = 0;
    if (lexer(line, &args, &num_args) == -1) { // tokenize user command
      free(line);
      write(STDERR_FILENO, error_message, strlen(error_message));
    }
    free(line);

    if (num_args == 0)
      continue;

    int rc;     // return code
    char **cmd; // buffer to hold the command to run
    int index;
    for (int i = 0; i < num_args; i++) {
      cmd = malloc((num_args + 1) * sizeof(char *));
      if (!cmd)
        exit(EXIT_FAILURE);

      index = 0;
      /* insert args into buffer until we hit the last arg or hit a semicolon */
      while (i < num_args && strcmp(args[i], ";") != 0)
        cmd[index++] = args[i++];
      cmd[index] = NULL;

      if (!cmd[0]) { // checks for no args (i.e. we only have a semicolon)
        free(cmd);
        continue;
      }

      int is_redir = find_redir(cmd), is_pipe = find_pipe(cmd);
      if (is_redir == -1 || is_pipe == -1) {
        write(STDERR_FILENO, error_message, strlen(error_message));
        break;
      }

      rc = 0;
      if (strcmp(cmd[0], "exit") == 0) {
        if (!cmd[1]) { // check for no additional args
          free(cmd);
          free_args(args, num_args);
          exit(EXIT_SUCCESS);
        }
        rc = -1;
      } else if (strcmp(cmd[0], "loop") == 0) {
        rc = loop(cmd, num_args, is_redir, is_pipe);
      } else if (is_pipe) {
        rc = run_pipe(cmd, num_args, is_redir);
      } else if (is_redir) {
        rc = redirection(cmd, num_args);
      } else if (strcmp(cmd[0], "cd") == 0) {
        rc = cd(cmd);
      } else if (strcmp(cmd[0], "pwd") == 0) {
        rc = pwd(cmd);
      } else {
        rc = run_cmd(STDIN_FILENO, STDOUT_FILENO, cmd);
      }
      free(cmd);

      if (rc == -1) { // non-program related errors throw an error message
        write(STDERR_FILENO, error_message, strlen(error_message));
        break;
      }
    }
    free_args(args, num_args);
  }
  return 0;
}
