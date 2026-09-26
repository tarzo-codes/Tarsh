#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#include "builtins.h"
#include "alias.h"
#include "config.h"
#include "exec.h"
#include "expand.h"
#include "jobs.h"
#include "lang.h"
#include "lineedit.h"
#include "util.h"

extern char **environ;

// Commands that run inside the shell rather than as separate programs,
// either because they change the shell itself (cd, export, alias, fg...)
// or because they are too basic to need a program.
static const char *builtin_names[] = {
  "cd", "pwd", "echo", "export", "unset", "alias", "unalias", "source", ".",
  "type", "read", "shift", "umask", "true", "false", ":",
  "jobs", "fg", "bg", "wait", "kill", "exec",
  "lang", "history", "config", "import-rc", "help", "exit", NULL
};

const char *builtin_name_at(int index) {
  int count = (int)(sizeof(builtin_names) / sizeof(builtin_names[0])) - 1;
  if (index < 0 || index >= count) {
    return NULL;
  }
  return builtin_names[index];
}

void builtin_write_names(FILE *file) {
  for (int i = 0;builtin_names[i] != NULL;i++) {
    fprintf(file, "%s\n", builtin_names[i]);
  }
}

int builtin_exists(const char *name) {
  for (int i = 0;builtin_names[i] != NULL;i++) {
    if (strcmp(name, builtin_names[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

// cd must run in the shell itself; a forked child could only change its
// own directory and then exit.
static int builtin_cd(ArgList *args) {
  const char *target = NULL;

  if (args->count > 2) {
    fprintf(stderr, "tarsh: cd: too many arguments\n");
    return 1;
  }

  if (args->count == 1) {
    target = getenv("HOME");
    if (target == NULL) {
      fprintf(stderr, "tarsh: cd: HOME not set\n");
      return 1;
    }
  }
  else if (strcmp(args->items[1], "-") == 0) {
    target = getenv("OLDPWD");
    if (target == NULL) {
      fprintf(stderr, "tarsh: cd: OLDPWD not set\n");
      return 1;
    }
    printf("%s\n", target);
  }
  else {
    target = args->items[1];
  }

  char previous[PATH_MAX];
  if (getcwd(previous, sizeof(previous)) == NULL) {
    previous[0] = '\0';
  }

  if (chdir(target) != 0) {
    fprintf(stderr, "tarsh: cd: %s: %s\n", target, strerror(errno));
    return 1;
  }

  // Keep PWD/OLDPWD in step so child processes and the prompt agree.
  char current[PATH_MAX];
  if (previous[0] != '\0') {
    setenv("OLDPWD", previous, 1);
  }
  if (getcwd(current, sizeof(current)) != NULL) {
    setenv("PWD", current, 1);
  }
  return 0;
}

static int builtin_pwd(void) {
  char current[PATH_MAX];
  if (getcwd(current, sizeof(current)) == NULL) {
    perror("tarsh: pwd");
    return 1;
  }
  printf("%s\n", current);
  return 0;
}

static int builtin_export(ArgList *args) {
  if (args->count == 1) {
    for (char **entry = environ;*entry != NULL;entry++) {
      printf("export %s\n", *entry);
    }
    return 0;
  }

  int status = 0;
  for (int i = 1;i < args->count;i++) {
    char *separator = strchr(args->items[i], '=');
    if (separator == NULL) {
      // `export NAME` alone just marks an existing value for export,
      // which is already true of everything in our environment.
      if (getenv(args->items[i]) == NULL) {
        fprintf(stderr, "tarsh: export: %s: not set\n", args->items[i]);
        status = 1;
      }
      continue;
    }

    *separator = '\0';
    if (setenv(args->items[i], separator + 1, 1) != 0) {
      fprintf(stderr, "tarsh: export: %s: %s\n", args->items[i], strerror(errno));
      status = 1;
    }
    *separator = '=';
  }
  return status;
}

static int builtin_unset(ArgList *args) {
  int status = 0;
  for (int i = 1;i < args->count;i++) {
    if (unsetenv(args->items[i]) != 0) {
      fprintf(stderr, "tarsh: unset: %s: %s\n", args->items[i], strerror(errno));
      status = 1;
    }
  }
  return status;
}

static void print_escaped(const char *text) {
  for (const char *c = text;*c != '\0';c++) {
    if (*c != '\\' || c[1] == '\0') {
      putchar(*c);
      continue;
    }
    c++;
    switch (*c) {
      case 'n': putchar('\n'); break;
      case 't': putchar('\t'); break;
      case 'r': putchar('\r'); break;
      case 'a': putchar('\a'); break;
      case 'e': putchar('\033'); break;
      case '\\': putchar('\\'); break;
      case '0': putchar('\0'); break;
      default: putchar('\\'); putchar(*c); break;
    }
  }
}

static int builtin_echo(ArgList *args) {
  int newline = 1;
  int escapes = 0;
  int first = 1;

  // Options are -n, -e, -E or combinations like -ne.
  while (first < args->count && args->items[first][0] == '-' && args->items[first][1] != '\0' &&
         strspn(args->items[first] + 1, "neE") == strlen(args->items[first] + 1)) {
    for (const char *c = args->items[first] + 1;*c != '\0';c++) {
      if (*c == 'n') newline = 0;
      if (*c == 'e') escapes = 1;
      if (*c == 'E') escapes = 0;
    }
    first++;
  }

  for (int i = first;i < args->count;i++) {
    if (i > first) {
      putchar(' ');
    }
    if (escapes) {
      print_escaped(args->items[i]);
    }
    else {
      fputs(args->items[i], stdout);
    }
  }
  if (newline) {
    putchar('\n');
  }
  return 0;
}

static int builtin_alias(ArgList *args) {
  if (args->count == 1) {
    return alias_print(NULL);
  }
  int status = 0;
  for (int i = 1;i < args->count;i++) {
    char *equals = strchr(args->items[i], '=');
    if (equals == NULL) {
      if (alias_print(args->items[i]) != 0) {
        fprintf(stderr, "tarsh: alias: %s: no such alias\n", args->items[i]);
        status = 1;
      }
      continue;
    }
    *equals = '\0';
    if (alias_set(args->items[i], equals + 1) != 0) {
      fprintf(stderr, "tarsh: alias: '%s' can't be used as an alias name\n", args->items[i]);
      status = 1;
    }
    *equals = '=';
  }
  return status;
}

static int builtin_unalias(ArgList *args) {
  int status = 0;
  for (int i = 1;i < args->count;i++) {
    if (alias_unset(args->items[i]) != 0) {
      fprintf(stderr, "tarsh: unalias: %s: no such alias\n", args->items[i]);
      status = 1;
    }
  }
  return status;
}

static int ends_with(const char *text, const char *suffix) {
  size_t text_length = strlen(text);
  size_t suffix_length = strlen(suffix);
  return text_length >= suffix_length && strcmp(text + text_length - suffix_length, suffix) == 0;
}

// Is `path` written in tarsh's own language? (.tsh files, tarshrc, or a
// #! line that mentions tarsh.)
static int is_tarsh_file(const char *path) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  if (ends_with(path, ".tsh") || strcmp(base, "tarshrc") == 0) {
    return 1;
  }
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return 0;
  }
  char first[256] = "";
  int tarsh = fgets(first, sizeof(first), file) != NULL &&
              strncmp(first, "#!", 2) == 0 && strstr(first, "tarsh") != NULL;
  fclose(file);
  return tarsh;
}

// tarsh files run in tarsh; anything else (a venv's activate, a bash
// script full of functions) runs in the shell language and its variables
// and directory come back.
static int builtin_source(ArgList *args) {
  if (args->count < 2) {
    fprintf(stderr, "usage: source FILE\n");
    return 2;
  }
  const char *path = args->items[1];
  if (access(path, R_OK) != 0) {
    fprintf(stderr, "tarsh: source: %s: %s\n", path, strerror(errno));
    return 1;
  }
  if (is_tarsh_file(path)) {
    return exec_run_file(path, 0);
  }

  char *quoted = shell_quote(path);
  size_t size = strlen(quoted ? quoted : path) + 16;
  char *line = malloc(size);
  if (line == NULL) {
    free(quoted);
    return 1;
  }
  const char *keyword = strcmp(lang_current_name(), "sh") == 0 ? "." : "source";
  snprintf(line, size, "%s %s", keyword, quoted ? quoted : path);
  int status = lang_run(line);
  free(line);
  free(quoted);
  return status;
}

static int builtin_type(ArgList *args) {
  int status = 0;
  for (int i = 1;i < args->count;i++) {
    const char *name = args->items[i];
    char path[PATH_MAX];
    const char *alias = alias_get(name);
    if (alias != NULL) {
      printf("%s is an alias for %s\n", name, alias);
    }
    else if (builtin_exists(name)) {
      printf("%s is a tarsh builtin\n", name);
    }
    else if (path_lookup(name, path, sizeof(path))) {
      printf("%s is %s\n", name, path);
    }
    else {
      fprintf(stderr, "tarsh: type: %s: not found\n", name);
      status = 1;
    }
  }
  return status;
}

// read [-r] [-p prompt] [NAME...]: reads one line from standard input into
// the named variables (the last one gets the rest of the line), or REPLY.
static int builtin_read(ArgList *args) {
  int first = 1;
  const char *prompt = NULL;
  while (first < args->count && args->items[first][0] == '-') {
    if (strcmp(args->items[first], "-p") == 0 && first + 1 < args->count) {
      prompt = args->items[first + 1];
      first += 2;
    }
    else if (strcmp(args->items[first], "-r") == 0) {
      first++;
    }
    else {
      fprintf(stderr, "tarsh: read: unknown option %s\n", args->items[first]);
      return 2;
    }
  }
  if (prompt != NULL) {
    fputs(prompt, stderr);
    fflush(stderr);
  }

  // Byte at a time so nothing past the newline is consumed.
  char *line = NULL;
  size_t length = 0;
  size_t capacity = 0;
  int got_any = 0;
  char c;
  while (1) {
    ssize_t result = read(STDIN_FILENO, &c, 1);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0 || c == '\n') {
      got_any = got_any || result > 0;
      break;
    }
    got_any = 1;
    if (length + 2 > capacity) {
      capacity = capacity ? capacity * 2 : 128;
      char *grown = realloc(line, capacity);
      if (grown == NULL) {
        free(line);
        return 1;
      }
      line = grown;
    }
    line[length++] = c;
  }
  if (!got_any) {
    free(line);
    return 1;
  }
  if (line == NULL) {
    line = strdup("");
  }
  else {
    line[length] = '\0';
  }

  if (first >= args->count) {
    setenv("REPLY", line, 1);
    free(line);
    return 0;
  }

  char *cursor = line;
  for (int i = first;i < args->count;i++) {
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (i == args->count - 1) {
      // The last variable takes the rest, minus trailing spaces.
      size_t rest = strlen(cursor);
      while (rest > 0 && (cursor[rest - 1] == ' ' || cursor[rest - 1] == '\t')) {
        cursor[--rest] = '\0';
      }
      setenv(args->items[i], cursor, 1);
      break;
    }
    size_t word = strcspn(cursor, " \t");
    char saved = cursor[word];
    cursor[word] = '\0';
    setenv(args->items[i], cursor, 1);
    cursor += word;
    if (saved != '\0') {
      cursor++;
    }
  }
  free(line);
  return 0;
}

static int builtin_shift(ArgList *args) {
  int n = args->count > 1 ? atoi(args->items[1]) : 1;
  if (expand_shift(n) != 0) {
    fprintf(stderr, "tarsh: shift: not that many arguments\n");
    return 1;
  }
  return 0;
}

static int builtin_umask(ArgList *args) {
  if (args->count == 1) {
    mode_t current = umask(0);
    umask(current);
    printf("%04o\n", (unsigned)current);
    return 0;
  }
  char *end;
  long mode = strtol(args->items[1], &end, 8);
  if (*end != '\0' || mode < 0 || mode > 0777) {
    fprintf(stderr, "tarsh: umask: %s: use an octal number like 022\n", args->items[1]);
    return 1;
  }
  umask((mode_t)mode);
  return 0;
}

static int builtin_lang(ArgList *args) {
  if (args->count == 1) {
    printf("tarsh runs commands, pipes, redirects, && and wildcards itself.\n");
    printf("loops, if/else, functions and other language features run in:\n\n");
    lang_list();
    printf("\nswitch with: lang bash | lang zsh | lang fish | lang sh\n");
    return 0;
  }
  return lang_set(args->items[1], !exec_is_sourcing());
}

static int builtin_history(ArgList *args) {
  int total = lineedit_history_count();
  int first = 0;
  if (args->count > 1) {
    int wanted = atoi(args->items[1]);
    if (wanted > 0 && wanted < total) {
      first = total - wanted;
    }
  }
  for (int i = first;i < total;i++) {
    printf("%5d  %s\n", i + 1, lineedit_history_get(i));
  }
  return 0;
}

// Opens tarshrc (or config.t with `config settings`) in your editor and
// reloads it afterwards.
static int builtin_config(ArgList *args) {
  int settings = args->count > 1 && strcmp(args->items[1], "settings") == 0;
  if (args->count > 1 && !settings && strcmp(args->items[1], "rc") != 0) {
    fprintf(stderr, "usage: config [rc|settings]\n");
    return 2;
  }
  const char *path = settings ? config_settings_path() : config_rc_path();

  const char *editor = getenv("VISUAL");
  if (editor == NULL || editor[0] == '\0') editor = getenv("EDITOR");
  char found[PATH_MAX];
  const char *fallbacks[] = { "nano", "nvim", "vim", "vi", NULL };
  for (int i = 0;(editor == NULL || editor[0] == '\0') && fallbacks[i] != NULL;i++) {
    if (path_lookup(fallbacks[i], found, sizeof(found))) {
      editor = fallbacks[i];
    }
  }
  if (editor == NULL || editor[0] == '\0') {
    fprintf(stderr, "tarsh: config: no editor found. set one with: export EDITOR=nano\n");
    return 1;
  }

  char *argv[] = { (char *)editor, (char *)path, NULL };
  int status = jobs_run_argv(argv, editor, NULL);
  if (status != 0) {
    return status;
  }
  if (settings) {
    config_load();
  }
  else {
    exec_run_file(path, 0);
  }
  printf("reloaded %s\n", path);
  return 0;
}

// Copies alias and export lines from bash/zsh startup files into tarshrc
// and applies them now.
static int builtin_import_rc(void) {
  const char *home = getenv("HOME");
  if (home == NULL) {
    fprintf(stderr, "tarsh: import-rc: HOME is not set\n");
    return 1;
  }
  const char *sources[] = { ".bashrc", ".bash_aliases", ".zshrc", ".profile", NULL };

  // Read the current rc so nothing is imported twice.
  char *existing = NULL;
  size_t existing_size = 0;
  FILE *rc = fopen(config_rc_path(), "r");
  if (rc != NULL) {
    FILE *memory = open_memstream(&existing, &existing_size);
    char chunk[4096];
    size_t got;
    while (memory != NULL && (got = fread(chunk, 1, sizeof(chunk), rc)) > 0) {
      fwrite(chunk, 1, got, memory);
    }
    if (memory != NULL) fclose(memory);
    fclose(rc);
  }

  FILE *out = fopen(config_rc_path(), "a");
  if (out == NULL) {
    fprintf(stderr, "tarsh: import-rc: %s: %s\n", config_rc_path(), strerror(errno));
    free(existing);
    return 1;
  }

  int imported = 0;
  for (int s = 0;sources[s] != NULL;s++) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", home, sources[s]);
    FILE *file = fopen(path, "r");
    if (file == NULL) {
      continue;
    }
    int header_written = 0;
    char *line = NULL;
    size_t line_size = 0;
    while (getline(&line, &line_size, file) != -1) {
      line[strcspn(line, "\n")] = '\0';
      // Only top-level lines: indented ones sit inside if-blocks.
      if (strncmp(line, "alias ", 6) != 0 && strncmp(line, "export ", 7) != 0) {
        continue;
      }
      if (existing != NULL && strstr(existing, line) != NULL) {
        continue;
      }
      if (!header_written) {
        fprintf(out, "\n# imported from ~/%s\n", sources[s]);
        header_written = 1;
      }
      fprintf(out, "%s\n", line);
      fflush(out);
      exec_run_line(line);
      printf("  + %s\n", line);
      imported++;
    }
    free(line);
    fclose(file);
  }
  fclose(out);
  free(existing);

  if (imported == 0) {
    printf("nothing new to import (looked in ~/.bashrc, ~/.bash_aliases, ~/.zshrc, ~/.profile)\n");
  }
  else {
    printf("imported %d line%s into %s\n", imported, imported == 1 ? "" : "s", config_rc_path());
  }
  return 0;
}

static int builtin_help(void) {
  printf("tarsh - a beginner-friendly shell\n\n");
  printf("finding things (needs fzf):\n");
  printf("  cd<space>        pick a folder to go into\n");
  printf("  nvim<space>      pick files to edit (also vim, nano, cat, less, code)\n");
  printf("  cp<space>        pick what to copy, then where to put it (mv works the same)\n");
  printf("  rm<space>        pick files to delete (Tab selects several)\n");
  printf("  Tab              complete a command or path (an fzf picker if installed)\n");
  printf("  Ctrl+R           search commands you ran before\n\n");
  printf("combining commands:\n");
  printf("  a | b            send a's output into b          a > file   write output to a file\n");
  printf("  a && b           run b only if a worked          a >> file  add output to a file\n");
  printf("  a || b           run b only if a failed          a < file   read input from a file\n");
  printf("  a ; b            run a, then b                   2>&1       errors go where output goes\n");
  printf("  a &              run a in the background         $(a)       use a's output as text\n");
  printf("  *.txt            every .txt file here            ~          your home folder\n\n");
  printf("running programs:\n");
  printf("  Ctrl+C           stop the running program\n");
  printf("  Ctrl+Z           pause it (then: fg to resume, bg to keep it running behind)\n");
  printf("  jobs             list paused and background programs\n");
  printf("  fg / bg [%%n]     bring job n back / continue it in the background\n");
  printf("  kill %%n          stop job n          wait     wait for background jobs\n\n");
  printf("editing the line:\n");
  printf("  Up/Down          previous/next command      Ctrl+A/Ctrl+E  start/end of line\n");
  printf("  Ctrl+W           delete a word              Ctrl+U         delete to start\n");
  printf("  Ctrl+L           clear the screen           Ctrl+D         exit (on an empty line)\n\n");
  printf("builtins:\n");
  printf("  cd [dir|-]  pwd  echo [-n] [-e]  export N=V  unset N  alias n='cmd'  unalias n\n");
  printf("  source file  type name  read [-p prompt] var  shift  umask  exec cmd\n");
  printf("  jobs  fg  bg  wait  kill  history [n]  lang [name]  help  exit [status]\n\n");
  printf("your setup:\n");
  printf("  config           edit ~/.config/tarsh/tarshrc (aliases, variables, startup commands)\n");
  printf("  config settings  edit ~/.config/tarsh/config.t (prompt, fzf, language)\n");
  printf("  import-rc        copy aliases/exports from ~/.bashrc and ~/.zshrc\n\n");
  printf("if/for/while, functions and other script syntax run in %s (change with: lang).\n",
         lang_current_name());
  return 0;
}

static int builtin_exit(ArgList *args, int *should_exit) {
  static int warned = 0;
  if (jobs_has_stopped() && !warned) {
    fprintf(stderr, "there are paused jobs (see: jobs). type exit again to leave anyway.\n");
    warned = 1;
    return 1;
  }
  *should_exit = 1;
  if (args->count > 1) {
    return atoi(args->items[1]) & 0xFF;
  }
  return expand_last_status();
}

int builtin_run(ArgList *args, int *status, int *should_exit) {
  if (args->count == 0) {
    return 0;
  }

  const char *name = args->items[0];

  if (strcmp(name, "cd") == 0) *status = builtin_cd(args);
  else if (strcmp(name, "pwd") == 0) *status = builtin_pwd();
  else if (strcmp(name, "echo") == 0) *status = builtin_echo(args);
  else if (strcmp(name, "export") == 0) *status = builtin_export(args);
  else if (strcmp(name, "unset") == 0) *status = builtin_unset(args);
  else if (strcmp(name, "alias") == 0) *status = builtin_alias(args);
  else if (strcmp(name, "unalias") == 0) *status = builtin_unalias(args);
  else if (strcmp(name, "source") == 0 || strcmp(name, ".") == 0) *status = builtin_source(args);
  else if (strcmp(name, "type") == 0) *status = builtin_type(args);
  else if (strcmp(name, "read") == 0) *status = builtin_read(args);
  else if (strcmp(name, "shift") == 0) *status = builtin_shift(args);
  else if (strcmp(name, "umask") == 0) *status = builtin_umask(args);
  else if (strcmp(name, "true") == 0 || strcmp(name, ":") == 0) *status = 0;
  else if (strcmp(name, "false") == 0) *status = 1;
  else if (strcmp(name, "jobs") == 0) *status = jobs_builtin_jobs(args);
  else if (strcmp(name, "fg") == 0) *status = jobs_builtin_fg(args);
  else if (strcmp(name, "bg") == 0) *status = jobs_builtin_bg(args);
  else if (strcmp(name, "wait") == 0) *status = jobs_builtin_wait(args);
  else if (strcmp(name, "kill") == 0) *status = jobs_builtin_kill(args);
  else if (strcmp(name, "lang") == 0) *status = builtin_lang(args);
  else if (strcmp(name, "history") == 0) *status = builtin_history(args);
  else if (strcmp(name, "config") == 0) *status = builtin_config(args);
  else if (strcmp(name, "import-rc") == 0) *status = builtin_import_rc();
  else if (strcmp(name, "help") == 0) *status = builtin_help();
  else if (strcmp(name, "exit") == 0) *status = builtin_exit(args, should_exit);
  else return 0;

  return 1;
}
