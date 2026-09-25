#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>    // For fork() and execvp()
#include <sys/wait.h>  // For waitpid()
#include <signal.h>
#include <errno.h>
#include <time.h>

#include "parser.h"
#include "prompts.h"
#include "builtins.h"

static double seconds_between(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) +
         (double)(end.tv_nsec - start.tv_nsec) / 1e9;
}

// Runs an external command and returns the status the shell should report.
static int run_external(ArgList *args) {
  pid_t pid = fork();

  if (pid < 0) {
    perror("tarsh: fork failed");
    return 1;
  }

  if (pid == 0) {
    // Child: the shell ignores these signals, but the command should not.
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);

    execvp(args->items[0], args->items);

    // execvp() only returns on failure. Use the conventional statuses so
    // scripts can tell "not found" from "found but not runnable".
    int code = (errno == ENOENT) ? 127 : 126;
    fprintf(stderr, "tarsh: %s: %s\n", args->items[0], strerror(errno));
    _exit(code);
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      perror("tarsh: waitpid failed");
      return 1;
    }
  }

  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    int signal_number = WTERMSIG(status);
    // Commands killed by Ctrl+C leave the cursor mid-line.
    if (signal_number == SIGINT) {
      printf("\n");
    }
    return 128 + signal_number;
  }
  return status;
}

int main(void) {
  char *command_buffer = NULL;
  size_t buffer_size = 0;
  int last_status = 0;
  int should_exit = 0;
  int interactive = isatty(STDIN_FILENO);

  // NOTE: Initial terminal prompt setup
  setup_prompt();

  // The shell itself must survive Ctrl+C; children restore the default.
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);

  ArgList args;
  arglist_init(&args);

  while (!should_exit) {
    if (interactive) {
      printf("%s", render_prompt());
      fflush(stdout);
    }

    ssize_t characters_read = getline(&command_buffer, &buffer_size, stdin);

    // NOTE: Error handling — Ctrl+D (EOF) leaves the shell
    if (characters_read == -1) {
      if (interactive) {
        printf("\nexit\n");
      }
      break;
    }

    command_buffer[strcspn(command_buffer, "\n")] = '\0';

    // NOTE: Parsing the command line into arguments
    int token_count = main_command_parser(command_buffer, &args);

    if (token_count < 0) {
      fprintf(stderr, "tarsh: syntax error: unterminated quote\n");
      last_status = 2;
      update_prompt(0.0, last_status);
      continue;
    }

    // Blank lines and comments are not commands.
    if (token_count == 0 || args.items[0][0] == '#') {
      continue;
    }

    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Builtins run in this process; everything else is forked.
    if (!builtin_run(&args, &last_status, &should_exit)) {
      last_status = run_external(&args);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    update_prompt(seconds_between(start, end), last_status);
  }

  arglist_free(&args);
  free(command_buffer);
  return last_status;
}
