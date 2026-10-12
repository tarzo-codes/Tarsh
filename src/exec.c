#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#include "exec.h"
#include "alias.h"
#include "builtins.h"
#include "expand.h"
#include "jobs.h"
#include "lang.h"
#include "syntax.h"
#include "suggest.h"
#include "util.h"
#include "config.h"
#include "script.h"
#include "foreign.h"

static int should_exit = 0;
static int sourcing_depth = 0;

int exec_should_exit(void) {
  return should_exit;
}

void exec_request_exit(void) {
  should_exit = 1;
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

// NAME=value on its own: a shell variable, unless NAME is already
// exported (then the exported value changes, like in other shells).
static void set_shell_assignments(const ArgList *assignments) {
  for (int i = 0;i < assignments->count;i++) {
    char *copy = strdup(assignments->items[i]);
    char *equals = strchr(copy, '=');
    *equals = '\0';
    if (getenv(copy) != NULL) {
      setenv(copy, equals + 1, 1);
    }
    else {
      script_var_set(copy, equals + 1);
    }
    free(copy);
  }
}

// ~/.config/tarsh/functions/NAME.tsh is loaded the first time NAME is run.
static int autoload_function(const char *name) {
  if (strchr(name, '/') != NULL || name[0] == '.') {
    return 0;
  }
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/functions/%s.tsh", config_directory(), name);
  if (access(path, R_OK) != 0) {
    return 0;
  }
  exec_run_file(path, 1);
  return script_function_exists(name);
}

int exec_command_exists(const char *name) {
  char resolved[PATH_MAX];
  if (builtin_exists(name) || script_function_exists(name) || path_lookup(name, resolved, sizeof(resolved))) {
    return 1;
  }
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/functions/%s.tsh", config_directory(), name);
  return strchr(name, '/') == NULL && access(path, R_OK) == 0;
}

// fish-style: typing a folder's path on its own goes there.
static int is_implicit_cd(const ExpandedCommand *command) {
  if (command->argv.count != 1 || command->redir_count != 0) {
    return 0;
  }
  const char *word = command->argv.items[0];
  char resolved[PATH_MAX];
  if (builtin_exists(word) || script_function_exists(word) || path_lookup(word, resolved, sizeof(resolved))) {
    return 0;
  }
  struct stat info;
  int looks_like_path = strchr(word, '/') != NULL || strcmp(word, "..") == 0 || strcmp(word, ".") == 0;
  return looks_like_path && stat(word, &info) == 0 && S_ISDIR(info.st_mode);
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
  if (!builtin_exists(command->argv.items[0]) && script_function_exists(command->argv.items[0])) {
    status = script_call_command(&command->argv);
  }
  else {
    builtin_run(&command->argv, &status, &exit_requested);
  }
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

// ./script with no #! line: the kernel can't tell how to run it (and
// glibc would quietly use /bin/sh). Pick the shell from the file itself.
static int runs_foreign_script(ExpandedCommand *command, int *status) {
  const char *path = command->argv.items[0];
  if (strchr(path, '/') == NULL || access(path, X_OK) != 0) {
    return 0;
  }
  struct stat info;
  if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
    return 0;
  }
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return 0;
  }
  char start[2] = { 0, 0 };
  size_t got = fread(start, 1, 2, file);
  fclose(file);
  if (got == 2 && start[0] == '#' && start[1] == '!') {
    return 0;   // has a #! line: the kernel knows what to do
  }

  Foreign foreign;
  char self[PATH_MAX];
  const char *interpreter = NULL;
  if (foreign_analyze_file(path, &foreign)) {
    if (!lang_installed(foreign.language)) {
      return 0;
    }
    interpreter = foreign.language;
  }
  else {
    // A tarsh script without a #! line: run it with this tarsh.
    ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length <= 0) {
      return 0;
    }
    self[length] = '\0';
    interpreter = self;
    foreign.language[0] = '\0';
  }

  ArgList original = command->argv;
  arglist_init(&command->argv);
  arglist_append(&command->argv, interpreter);
  for (int i = 0;i < original.count;i++) {
    arglist_append(&command->argv, original.items[i]);
  }
  arglist_free(&original);

  char text[PATH_MAX + 32];
  snprintf(text, sizeof(text), "%s", path);
  *status = jobs_launch(command, 1, 0, text);
  if (foreign.language[0] != '\0') {
    // A script runs in its own process: nothing comes back to tarsh.
    lang_clear_changes();
    foreign_explain(&foreign, path);
  }
  return 1;
}

static int exec_command_exists_on_path(const char *name) {
  char resolved[PATH_MAX];
  return strchr(name, '/') != NULL || path_lookup(name, resolved, sizeof(resolved));
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
    set_shell_assignments(&only->assignments);
  }
  else if (pipeline->count == 1 && !background && strcmp(only->argv.items[0], "exec") == 0) {
    status = run_exec(only);
  }
  else if (pipeline->count == 1 && !background && builtin_exists(only->argv.items[0])) {
    status = run_builtin_here(only);
  }
  else if (pipeline->count == 1 && !background &&
           (script_function_exists(only->argv.items[0]) || (!exec_command_exists_on_path(only->argv.items[0]) &&
                                                             autoload_function(only->argv.items[0])))) {
    status = run_builtin_here(only);
  }
  else if (pipeline->count == 1 && !background && runs_foreign_script(only, &status)) {
    // handled
  }
  else if (pipeline->count == 1 && !background && is_implicit_cd(only)) {
    ArgList cd;
    arglist_init(&cd);
    arglist_append(&cd, "cd");
    arglist_append(&cd, only->argv.items[0]);
    int ignored = 0;
    builtin_run(&cd, &status, &ignored);
    arglist_free(&cd);
  }
  else {
    // Functions used in pipelines and in the background load here too.
    for (int i = 0;i < pipeline->count;i++) {
      if (commands[i].argv.count > 0 && !builtin_exists(commands[i].argv.items[0]) &&
          !script_function_exists(commands[i].argv.items[0]) &&
          !exec_command_exists_on_path(commands[i].argv.items[0])) {
        autoload_function(commands[i].argv.items[0]);
      }
    }
    status = jobs_launch(commands, pipeline->count, background, pipeline->text);
  }

  free_expanded(commands, pipeline->count);
  return status;
}

// Drops the first word of a command (for `and`, `or`, `not`).
static void shift_word(Command *command) {
  free(command->words.items[0]);
  memmove(command->words.items, command->words.items + 1, (command->words.count - 1) * sizeof(char *));
  command->words.count--;
  command->words.items[command->words.count] = NULL;
}

static int is_word(const Command *command, const char *word) {
  return command->words.count > 1 && strcmp(command->words.items[0], word) == 0;
}

static int run_list(CommandList *list) {
  int status = expand_last_status();
  for (int i = 0;i < list->count && !should_exit;i++) {
    if (i > 0) {
      Connector before = list->connectors[i - 1];
      if ((before == CONNECT_AND && status != 0) || (before == CONNECT_OR && status == 0)) {
        continue;
      }
    }
    // fish's `cmd; and other`, `cmd; or other`, `not cmd` (also `! cmd`).
    Command *first = &list->pipelines[i].commands[0];
    if (is_word(first, "and") || is_word(first, "or")) {
      int wants_success = first->words.items[0][0] == 'a';
      if ((wants_success && status != 0) || (!wants_success && status == 0)) {
        continue;
      }
      shift_word(first);
    }
    int negate = 0;
    if (is_word(first, "not") || is_word(first, "!")) {
      negate = 1;
      shift_word(first);
    }
    status = run_pipeline(&list->pipelines[i], list->connectors[i] == CONNECT_BG);
    if (negate) {
      status = status == 0 ? 1 : 0;
    }
    expand_set_last_status(status);
  }
  return status;
}

int exec_run_line(const char *text) {
  // x = 1, if x:, def f():, print(...) ... belong to tarsh script.
  if (script_claims(text)) {
    return script_run_source(text, "input", 1, jobs_interactive() && !sourcing_depth);
  }

  char *line = alias_expand_line(text);
  int status;

  if (lang_needs_backend(line)) {
    // bash / zsh / fish syntax: run it in the right shell and say so.
    status = foreign_run_line(line);
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

static int leading_indent(const char *line) {
  int indent = 0;
  while (line[indent] == ' ' || line[indent] == '\t') indent++;
  return indent;
}

static int is_blank_or_comment(const char *line) {
  const char *c = line + leading_indent(line);
  return *c == '\0' || *c == '#';
}

// Runs one chunk read from a file: a script block, a script statement or
// a (possibly multi-line) command.
static int chunk_failed = 0;

static int run_chunk(const char *text, const char *origin, int first_line) {
  if (script_claims(text)) {
    int status = script_run_source(text, origin, first_line, 0);
    // A script error stops the file, like an exception in Python. A
    // command that fails doesn't, like in any shell.
    chunk_failed = script_last_failed();
    return status;
  }
  if (script_missing_colon(text, origin, first_line)) {
    chunk_failed = 1;
    return 2;
  }
  return exec_run_line(text);
}

int exec_run_stream_named(FILE *input, const char *origin) {
  char *line = NULL;
  size_t line_size = 0;
  char *pending = NULL;
  size_t pending_length = 0;
  int pending_line = 0;
  int pending_block = 0;   // a script block: ends at the next unindented line
  int line_number = 0;
  int status = 0;
  int have_line = 0;

  chunk_failed = 0;
  while (!should_exit && !chunk_failed) {
    if (!have_line) {
      if (getline(&line, &line_size, input) == -1) break;
      line[strcspn(line, "\n")] = '\0';
      line_number++;
    }
    have_line = 0;
    int fish = strcmp(lang_current_name(), "fish") == 0;

    if (pending_length == 0) {
      if (is_blank_or_comment(line)) continue;
      free(pending);
      pending = strdup(line);
      pending_length = strlen(pending);
      pending_line = line_number;
      pending_block = script_opens_block(line);
      if (!pending_block && !syntax_incomplete(pending, fish)) {
        status = run_chunk(pending, origin, pending_line);
        pending_length = 0;
      }
      continue;
    }

    if (pending_block) {
      const char *body = line + leading_indent(line);
      int continues = is_blank_or_comment(line) || leading_indent(line) > 0 ||
                      strncmp(body, "elif", 4) == 0 || strncmp(body, "else", 4) == 0;
      if (!continues) {
        status = run_chunk(pending, origin, pending_line);
        pending_length = 0;
        have_line = 1;   // handle this line as the start of the next chunk
        continue;
      }
    }

    size_t length = strlen(line);
    pending = realloc(pending, pending_length + length + 2);
    if (pending == NULL) break;
    if (!pending_block && pending[pending_length - 1] == '\\') {
      pending[--pending_length] = '\0';
    }
    else {
      pending[pending_length++] = '\n';
    }
    memcpy(pending + pending_length, line, length + 1);
    pending_length += length;

    if (!pending_block && !syntax_incomplete(pending, fish)) {
      status = run_chunk(pending, origin, pending_line);
      pending_length = 0;
    }
  }

  if (pending_length > 0 && !should_exit && !chunk_failed) {
    status = run_chunk(pending, origin, pending_line);
  }
  chunk_failed = 0;
  free(line);
  free(pending);
  return status;
}

int exec_run_stream(FILE *input) {
  return exec_run_stream_named(input, "stdin");
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
  const char *base = strrchr(path, '/');
  int status = exec_run_stream_named(file, base ? base + 1 : path);
  sourcing_depth--;
  fclose(file);
  return status;
}
