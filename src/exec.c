#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include "exec.h"
#include "alias.h"
#include "builtins.h"
#include "expand.h"
#include "jobs.h"
#include "lang.h"
#include "syntax.h"
#include "suggest.h"
#include "util.h"

static int should_exit = 0;
static int sourcing_depth = 0;

int exec_should_exit(void) {
  return should_exit;
}

void exec_cancel_exit(void) {
  should_exit = 0;
}

int exec_is_sourcing(void) {
  return sourcing_depth > 0;
}

// NAME=value (NAME must be a valid variable name).
static int is_assignment(const char *word) {
  const char *equals = strchr(word, '=');
  if (equals == NULL || equals == word) {
    return 0;
  }
  for (const char *c = word;c < equals;c++) {
    int letter = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '_';
    int digit = (*c >= '0' && *c <= '9');
    if (!letter && !(digit && c != word)) {
      return 0;
    }
  }
  return 1;
}

static void free_expanded(ExpandedCommand *commands, int count) {
  for (int i = 0;i < count;i++) {
    arglist_free(&commands[i].argv);
    arglist_free(&commands[i].assignments);
    for (int r = 0;r < commands[i].redir_count;r++) {
      free(commands[i].redirs[r].path);
    }
    free(commands[i].redirs);
  }
  free(commands);
}

static int expand_command(const Command *command, ExpandedCommand *out) {
  arglist_init(&out->argv);
  arglist_init(&out->assignments);
  out->redirs = NULL;
  out->redir_count = 0;

  int i = 0;
  // Leading NAME=value words are assignments, not the command.
  while (i < command->words.count && is_assignment(command->words.items[i])) {
    char *value = expand_single(command->words.items[i]);
    arglist_append(&out->assignments, value);
    free(value);
    i++;
  }
  for (;i < command->words.count;i++) {
    expand_word(command->words.items[i], &out->argv);
  }

  if (command->redir_count > 0) {
    out->redirs = calloc(command->redir_count, sizeof(ExpandedRedirect));
    if (out->redirs == NULL) {
      return -1;
    }
  }
  for (int r = 0;r < command->redir_count;r++) {
    const Redirect *source = &command->redirs[r];
    ExpandedRedirect *target = &out->redirs[r];
    target->kind = source->kind;
    target->fd = source->fd;
    target->target_fd = source->target_fd;
    target->path = NULL;
    out->redir_count++;
    if (source->target != NULL) {
      target->path = expand_single(source->target);
      if (target->path[0] == '\0') {
        fprintf(stderr, "tarsh: %s: redirect target is empty\n", source->target);
        return -1;
      }
    }
  }
  return 0;
}

static void set_assignments(const ArgList *assignments) {
  for (int i = 0;i < assignments->count;i++) {
    char *copy = strdup(assignments->items[i]);
    char *equals = strchr(copy, '=');
    *equals = '\0';
    setenv(copy, equals + 1, 1);
    free(copy);
  }
}

// `exec cmd` replaces the shell; `exec >file` redirects the shell itself.
static int run_exec(ExpandedCommand *command) {
  if (jobs_apply_redirects(command->redirs, command->redir_count, NULL, NULL) != 0) {
    return 1;
  }
  if (command->argv.count == 1) {
    return 0;
  }
  set_assignments(&command->assignments);
  signal(SIGINT, SIG_DFL);
  signal(SIGQUIT, SIG_DFL);
  signal(SIGTSTP, SIG_DFL);
  signal(SIGTTIN, SIG_DFL);
  signal(SIGTTOU, SIG_DFL);
  fflush(stdout);
  execvp(command->argv.items[1], command->argv.items + 1);
  int saved_errno = errno;
  fprintf(stderr, "tarsh: exec: %s: %s\n", command->argv.items[1], strerror(saved_errno));
  return saved_errno == ENOENT ? 127 : 126;
}

// Runs a builtin inside the shell, with its redirections and NAME=value
// prefixes applied only for the duration.
static int run_builtin_here(ExpandedCommand *command) {
  SavedFd saved[MAX_SAVED_FDS];
  int saved_count = 0;

  fflush(stdout);
  fflush(stderr);
  if (jobs_apply_redirects(command->redirs, command->redir_count, saved, &saved_count) != 0) {
    jobs_restore_fds(saved, saved_count);
    return 1;
  }

  // Remember old values of temporary assignments.
  int count = command->assignments.count;
  char **old_values = calloc(count > 0 ? count : 1, sizeof(char *));
  for (int i = 0;i < count;i++) {
    char *name = strdup(command->assignments.items[i]);
    *strchr(name, '=') = '\0';
    const char *old = getenv(name);
    old_values[i] = old ? strdup(old) : NULL;
    free(name);
  }
  set_assignments(&command->assignments);

  int status = 0;
  int exit_requested = 0;
  builtin_run(&command->argv, &status, &exit_requested);
  if (exit_requested) {
    should_exit = 1;
  }

  for (int i = 0;i < count;i++) {
    char *name = strdup(command->assignments.items[i]);
    *strchr(name, '=') = '\0';
    if (old_values[i] != NULL) {
      setenv(name, old_values[i], 1);
    }
    else {
      unsetenv(name);
    }
    free(name);
    free(old_values[i]);
  }
  free(old_values);

  fflush(stdout);
  fflush(stderr);
  jobs_restore_fds(saved, saved_count);
  return status;
}

static int run_pipeline(const Pipeline *pipeline, int background) {
  ExpandedCommand *commands = calloc(pipeline->count, sizeof(ExpandedCommand));
  if (commands == NULL) {
    return 1;
  }
  for (int i = 0;i < pipeline->count;i++) {
    if (expand_command(&pipeline->commands[i], &commands[i]) != 0) {
      free_expanded(commands, i + 1);
      return 1;
    }
  }

  int status;
  ExpandedCommand *only = &commands[0];

  if (pipeline->count == 1 && !background && only->argv.count == 0) {
    // Just assignments (FOO=bar) and/or redirections (> empty.txt).
    SavedFd saved[MAX_SAVED_FDS];
    int saved_count = 0;
    status = jobs_apply_redirects(only->redirs, only->redir_count, saved, &saved_count) == 0 ? 0 : 1;
    jobs_restore_fds(saved, saved_count);
    set_assignments(&only->assignments);
  }
  else if (pipeline->count == 1 && !background && strcmp(only->argv.items[0], "exec") == 0) {
    status = run_exec(only);
  }
  else if (pipeline->count == 1 && !background && builtin_exists(only->argv.items[0])) {
    status = run_builtin_here(only);
  }
  else {
    status = jobs_launch(commands, pipeline->count, background, pipeline->text);
  }

  free_expanded(commands, pipeline->count);
  return status;
}

static int run_list(const CommandList *list) {
  int status = expand_last_status();
  for (int i = 0;i < list->count && !should_exit;i++) {
    if (i > 0) {
      Connector before = list->connectors[i - 1];
      if ((before == CONNECT_AND && status != 0) || (before == CONNECT_OR && status == 0)) {
        continue;
      }
    }
    status = run_pipeline(&list->pipelines[i], list->connectors[i] == CONNECT_BG);
    expand_set_last_status(status);
  }
  return status;
}

int exec_run_line(const char *text) {
  char *line = alias_expand_line(text);
  int status;

  if (lang_needs_backend(line)) {
    status = lang_run(line);
  }
  else {
    CommandList list;
    char error[256];
    if (syntax_parse(line, &list, error, sizeof(error)) != 0) {
      fprintf(stderr, "tarsh: %s\n", error);
      status = 2;
    }
    else {
      status = run_list(&list);
      syntax_free(&list);
    }
  }

  free(line);
  expand_set_last_status(status);
  return status;
}

int exec_run_stream(FILE *input) {
  char *line = NULL;
  size_t line_size = 0;
  char *pending = NULL;
  size_t pending_length = 0;
  int status = 0;
  int fish = strcmp(lang_current_name(), "fish") == 0;

  while (!should_exit && getline(&line, &line_size, input) != -1) {
    line[strcspn(line, "\n")] = '\0';

    size_t length = strlen(line);
    size_t needed = pending_length + length + 2;
    char *grown = realloc(pending, needed);
    if (grown == NULL) {
      break;
    }
    pending = grown;
    if (pending_length == 0) {
      pending[0] = '\0';
    }
    else if (pending[pending_length - 1] == '\\') {
      // A trailing backslash joins lines.
      pending[--pending_length] = '\0';
    }
    else {
      pending[pending_length++] = '\n';
      pending[pending_length] = '\0';
    }
    memcpy(pending + pending_length, line, length + 1);
    pending_length += length;

    if (syntax_incomplete(pending, fish)) {
      continue;
    }

    const char *start = pending;
    while (*start == ' ' || *start == '\t' || *start == '\n') start++;
    if (*start != '\0' && *start != '#') {
      status = exec_run_line(pending);
    }
    pending_length = 0;
    pending[0] = '\0';
  }

  if (pending_length > 0 && !should_exit) {
    status = exec_run_line(pending);
  }
  free(line);
  free(pending);
  return status;
}

int exec_run_file(const char *path, int quiet_missing) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    if (!quiet_missing) {
      fprintf(stderr, "tarsh: %s: %s\n", path, strerror(errno));
      return 1;
    }
    return 0;
  }
  sourcing_depth++;
  int status = exec_run_stream(file);
  sourcing_depth--;
  fclose(file);
  return status;
}
