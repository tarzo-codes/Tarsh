#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>

#include "config.h"
#include "exec.h"
#include "expand.h"
#include "fzf.h"
#include "jobs.h"
#include "lang.h"
#include "lineedit.h"
#include "prompts.h"
#include "syntax.h"
#include "theme.h"

#ifndef TARSH_VERSION
#define TARSH_VERSION "0.4.0"
#endif
#include "script.h"
#include "foreign.h"
#include "fish.h"
#include "live.h"

static double seconds_between(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) +
         (double)(end.tv_nsec - start.tv_nsec) / 1e9;
}

static int is_blank_or_comment(const char *line) {
  while (*line == ' ' || *line == '\t' || *line == '\n') {
    line++;
  }
  return *line == '\0' || *line == '#';
}

static void print_usage(void) {
  printf("usage: tarsh                 start an interactive shell\n");
  printf("       tarsh script [args]   run a tarsh script\n");
  printf("       tarsh -c 'command'    run one command line\n");
  printf("       tarsh -l              start as a login shell (reads /etc/profile, ~/.profile)\n");
  printf("       tarsh --private       don't read or save history this time\n");
  printf("       tarsh --version       show the version\n");
}

static void print_welcome(void) {
  printf("\n  welcome to tarsh!\n\n");
  printf("  - type cd, nvim or cp and press space to pick a path; Tab completes\n");
  printf("  - grey text is a suggestion from your history: press the Right arrow to take it\n");
  printf("  - paste commands written for bash, zsh or fish: they just work, with a short note\n");
  printf("  - tarsh script reads like Python:  for f in glob(\"*.txt\"):  (see: help)\n");
  printf("  - make it yours: config (aliases, startup), theme edit (your prompt)\n");
  if (!fzf_enabled()) {
    printf("  - the pickers need fzf: tarsh doctor shows how to install it\n");
  }
  printf("  - help shows everything; tarsh doctor checks your setup\n\n");
}

// Appends one more line of input to a multi-line command.
static char *join_lines(char *pending, const char *next) {
  size_t length = strlen(pending);
  int backslash = length > 0 && pending[length - 1] == '\\';
  if (backslash) {
    pending[--length] = '\0';
  }
  char *joined = malloc(length + strlen(next) + 2);
  if (joined == NULL) {
    return pending;
  }
  sprintf(joined, "%s%s%s", pending, backslash ? "" : "\n", next);
  free(pending);
  return joined;
}

// History keeps multi-line commands on one line: newlines become "; "
// except where the shell doesn't want a separator (after do, then, |...).
static char *history_form(const char *text) {
  size_t length = strlen(text);
  char *out = malloc(length * 2 + 1);
  if (out == NULL) {
    return NULL;
  }
  size_t used = 0;
  for (size_t i = 0;i < length;i++) {
    if (text[i] != '\n') {
      out[used++] = text[i];
      continue;
    }
    size_t end = used;
    while (end > 0 && out[end - 1] == ' ') end--;
    out[end] = '\0';
    const char *no_separator[] = { "do", "then", "else", "{", "|", "&&", "||", ";", "in", NULL };
    int joined = 0;
    for (int k = 0;no_separator[k] != NULL;k++) {
      size_t kl = strlen(no_separator[k]);
      if (end >= kl && strcmp(out + end - kl, no_separator[k]) == 0 &&
          (end == kl || out[end - kl - 1] == ' ' || !isalpha((unsigned char)no_separator[k][0]))) {
        joined = 1;
        break;
      }
    }
    used = end;
    if (!joined && used > 0) {
      out[used++] = ';';
    }
    out[used++] = ' ';
  }
  out[used] = '\0';
  return out;
}

static int interactive_loop(void) {
  while (!exec_should_exit()) {
    jobs_notify();
    universal_sync();
    dirs_note_cwd();

    char *line = lineedit_read(render_prompt());
    // NOTE: Ctrl+D (EOF) leaves the shell
    if (line == NULL) {
      if (jobs_has_stopped()) {
        // Same as typing exit: warn once about paused jobs.
        printf("\n");
        exec_run_line("exit");
        if (!exec_should_exit()) {
          continue;
        }
        break;
      }
      printf("exit\n");
      break;
    }

    int script_block = script_opens_block(line);
    if (script_block) {
      // A tarsh script block (if x:, for f in files:, def f():) goes on
      // until an empty line, like Python's prompt. Each new line starts
      // indented like the one before, one level deeper after a ':'.
      lineedit_history_add(line);
      char indent[256];
      const char *last = line;
      while (1) {
        size_t width = strspn(last, " \t");
        if (width > sizeof(indent) - 8) width = sizeof(indent) - 8;
        memcpy(indent, last, width);
        indent[width] = '\0';
        size_t end = strlen(last);
        while (end > 0 && last[end - 1] == ' ') end--;
        if (end > 0 && last[end - 1] == ':') strcat(indent, "    ");

        char *more = lineedit_read_initial("... ", indent);
        if (more == NULL || more[strspn(more, " \t")] == '\0') {
          free(more);
          break;
        }
        lineedit_history_add(more);
        size_t old_length = strlen(line);
        char *joined = realloc(line, old_length + strlen(more) + 2);
        if (joined == NULL) {
          free(more);
          break;
        }
        line = joined;
        sprintf(line + old_length, "\n%s", more);
        free(more);
        last = strrchr(line, '\n') + 1;
      }
    }
    else {
      // Keep reading while the command is unfinished (open quote, trailing
      // |, an if/for/while without its end...).
      int fish = strcmp(lang_current_name(), "fish") == 0;
      while (syntax_incomplete(line, fish)) {
        char *more = lineedit_read("> ");
        if (more == NULL) {
          break;
        }
        line = join_lines(line, more);
        free(more);
      }
    }

    if (is_blank_or_comment(line)) {
      free(line);
      continue;
    }

    if (!script_block) {
      char *remembered = history_form(line);
      if (remembered != NULL) {
        lineedit_history_add(remembered);
        free(remembered);
      }
    }

    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int status = exec_run_line(line);

    clock_gettime(CLOCK_MONOTONIC, &end);
    update_prompt(seconds_between(start, end), status);
    free(line);
    fflush(stdout);
  }
  return expand_last_status();
}

int main(int argc, char *argv[]) {
  // NOTE: Initial setup
  setup_prompt();
  fzf_setup();
  lang_setup();
  live_setup();
  expand_init();

  int login = argv[0][0] == '-';
  int private_mode = 0;
  int first_arg = 1;
  while (first_arg < argc && argv[first_arg][0] == '-') {
    const char *option = argv[first_arg];
    if (strcmp(option, "-l") == 0 || strcmp(option, "--login") == 0) {
      login = 1;
      first_arg++;
    }
    else if (strcmp(option, "-c") == 0) {
      break;
    }
    else if (strcmp(option, "-P") == 0 || strcmp(option, "--private") == 0) {
      private_mode = 1;
      first_arg++;
    }
    else if (strcmp(option, "-v") == 0 || strcmp(option, "--version") == 0) {
      printf("tarsh %s\n", TARSH_VERSION);
      return 0;
    }
    else if (strcmp(option, "-h") == 0 || strcmp(option, "--help") == 0) {
      print_usage();
      return 0;
    }
    else if (strcmp(option, "--") == 0) {
      first_arg++;
      break;
    }
    else {
      fprintf(stderr, "tarsh: unknown option %s\n", option);
      print_usage();
      return 2;
    }
  }

  int first_run = config_load();
  theme_init();
  universal_load();

  // `tarsh -c "command" [name args...]` runs one line and exits. Editors
  // and other tools use this form when tarsh is someone's $SHELL.
  if (first_arg < argc && strcmp(argv[first_arg], "-c") == 0) {
    if (first_arg + 1 >= argc) {
      fprintf(stderr, "tarsh: -c needs a command\n");
      return 2;
    }
    const char *command = argv[first_arg + 1];
    if (first_arg + 2 < argc) {
      expand_set_positional(argc - first_arg - 2, argv + first_arg + 2);
    }
    jobs_init(0);
    return exec_run_line(command);
  }

  // `tarsh script.tsh args...`
  if (first_arg < argc) {
    Foreign foreign;
    jobs_init(0);
    // `tarsh deploy.sh`: a bash/zsh/fish script runs in its own shell.
    if (foreign_analyze_file(argv[first_arg], &foreign) && lang_installed(foreign.language)) {
      int count = argc - first_arg;
      char **command = calloc(count + 2, sizeof(char *));
      if (command == NULL) {
        return 1;
      }
      command[0] = foreign.language;
      for (int i = 0;i < count;i++) command[i + 1] = argv[first_arg + i];
      int status = jobs_run_argv(command, argv[first_arg], NULL);
      free(command);
      if (isatty(STDERR_FILENO) && foreign_enabled()) {
        fprintf(stderr, "\x1b[2m\u21B3 tarsh %s.\x1b[0m\n", foreign.what);
      }
      return status;
    }
    expand_set_positional(argc - first_arg, argv + first_arg);
    int status = exec_run_file(argv[first_arg], 0);
    return status;
  }

  int interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
  jobs_init(interactive);

  if (login) {
    lang_source_login_profiles();
  }

  if (!interactive) {
    // Commands piped in: run them like a script.
    return exec_run_stream(stdin);
  }

  lineedit_set_private(private_mode);
  lineedit_history_load();
  int new_rc = config_ensure_rc();
  exec_run_file(config_rc_path(), 1);
  if (first_run || new_rc) {
    print_welcome();
  }
  if (config_greeting()[0]) {
    printf("%s\n", config_greeting());
  }
  if (private_mode) {
    printf("private mode: nothing you type is saved to history.\n");
  }

  int status = interactive_loop();
  jobs_shutdown();
  return status;
}
