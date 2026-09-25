#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <time.h>

#include "parser.h"
#include "prompts.h"
#include "builtins.h"
#include "config.h"
#include "fzf.h"
#include "lang.h"
#include "lineedit.h"
#include "suggest.h"
#include "util.h"

static double seconds_between(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) +
         (double)(end.tv_nsec - start.tv_nsec) / 1e9;
}

static int is_blank_or_comment(const char *line) {
  while (*line == ' ' || *line == '\t') {
    line++;
  }
  return *line == '\0' || *line == '#';
}

static void print_welcome(void) {
  printf("\n  welcome to tarsh!\n\n");
  printf("  - type a command like cd, nvim or cp and press space to pick a path\n");
  printf("  - press Tab to find a command, Ctrl+R to search your history\n");
  printf("  - pipes, loops and scripts run as %s (change with: lang fish)\n", lang_current_name());
  if (!fzf_enabled()) {
    printf("  - the pickers need fzf: sudo apt install fzf (or dnf / pacman)\n");
  }
  printf("  - type help any time to see everything\n\n");
}

// Runs one command line and returns its exit status.
static int execute_line(const char *line, ArgList *args, int *should_exit) {
  // Pipes, redirects, loops, globs, $(...): hand it to bash/zsh/fish.
  if (lang_needs_backend(line)) {
    return lang_run(line);
  }

  // NOTE: Parsing the command line into arguments
  int token_count = main_command_parser(line, args);

  if (token_count < 0) {
    fprintf(stderr, "tarsh: syntax error: a quote (' or \") was opened but never closed\n");
    return 2;
  }
  if (token_count == 0) {
    return 0;
  }

  // Builtins run in this process; everything else is forked.
  int status = 0;
  if (builtin_run(args, &status, should_exit)) {
    return status;
  }

  char resolved[PATH_MAX];
  if (!path_lookup(args->items[0], resolved, sizeof(resolved)) &&
      strchr(args->items[0], '/') == NULL) {
    suggest_command_not_found(args->items[0]);
    return 127;
  }

  return spawn_and_wait(args->items);
}

int main(int argc, char *argv[]) {
  int last_status = 0;
  int should_exit = 0;

  // NOTE: Initial terminal prompt setup
  setup_prompt();
  fzf_setup();
  lang_setup();

  ArgList args;
  arglist_init(&args);

  // `tarsh -c "command"` runs one line and exits. Editors and other tools
  // use this form when tarsh is someone's $SHELL.
  if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
    config_load();
    last_status = execute_line(argv[2], &args, &should_exit);
    arglist_free(&args);
    return last_status;
  }

  int first_run = config_load();
  int interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

  // The shell itself must survive Ctrl+C; children restore the default.
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);

  if (interactive) {
    lineedit_history_load();
    if (first_run) {
      print_welcome();
    }
  }

  char *command_buffer = NULL;
  size_t buffer_size = 0;

  while (!should_exit) {
    char *line = NULL;

    if (interactive) {
      line = lineedit_read(render_prompt());
      // NOTE: Ctrl+D (EOF) leaves the shell
      if (line == NULL) {
        printf("exit\n");
        break;
      }
    }
    else {
      if (getline(&command_buffer, &buffer_size, stdin) == -1) {
        break;
      }
      command_buffer[strcspn(command_buffer, "\n")] = '\0';
      line = strdup(command_buffer);
      if (line == NULL) {
        break;
      }
    }

    if (is_blank_or_comment(line)) {
      free(line);
      continue;
    }

    if (interactive) {
      lineedit_history_add(line);
    }

    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    last_status = execute_line(line, &args, &should_exit);

    clock_gettime(CLOCK_MONOTONIC, &end);
    parser_set_last_status(last_status);
    update_prompt(seconds_between(start, end), last_status);
    free(line);
    fflush(stdout);
  }

  arglist_free(&args);
  free(command_buffer);
  return last_status;
}
