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
#include "theme.h"
#include "theme_ui.h"
#include "util.h"
#include "foreign.h"
#include "fish.h"
#include "script.h"

extern char **environ;

// Commands that run inside the shell rather than as separate programs,
// either because they change the shell itself (cd, export, alias, fg...)
// or because they are too basic to need a program.
static const char *builtin_names[] = {
  "cd", "pwd", "echo", "export", "unset", "alias", "unalias", "source", ".",
  "type", "read", "shift", "umask", "true", "false", ":",
  "jobs", "fg", "bg", "wait", "kill", "exec",
  "lang", "history", "config", "import-rc", "theme", "explain", "doctor", "help", "exit", NULL
};

const char *builtin_name_at(int index) {
  int count = (int)(sizeof(builtin_names) / sizeof(builtin_names[0])) - 1;
  if (index < 0) {
    return NULL;
  }
  if (index < count) {
    return builtin_names[index];
  }
  static ArgList fish;
  static int ready = 0;
  if (!ready) {
    arglist_init(&fish);
    fish_write_builtin_names(&fish);
    ready = 1;
  }
  return index - count < fish.count ? fish.items[index - count] : NULL;
}

void builtin_write_names(FILE *file) {
  for (int i = 0;builtin_names[i] != NULL;i++) {
    fprintf(file, "%s\n", builtin_names[i]);
  }
  ArgList fish;
  arglist_init(&fish);
  fish_write_builtin_names(&fish);
  for (int i = 0;i < fish.count;i++) {
    fprintf(file, "%s\n", fish.items[i]);
  }
  arglist_free(&fish);
}

int builtin_exists(const char *name) {
  if (fish_builtin_exists(name)) {
    return 1;
  }
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
      // `export NAME`: hand a shell variable to programs too (a list is
      // passed as its items separated by spaces).
      char *text = NULL;
      if (script_var_get(args->items[i], &text, NULL) == 1) {
        setenv(args->items[i], text, 1);
        script_var_unset(args->items[i]);
        free(text);
      }
      else if (getenv(args->items[i]) == NULL) {
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
    script_var_unset(args->items[i]);
    *separator = '=';
  }
  return status;
}

static int builtin_unset(ArgList *args) {
  int status = 0;
  for (int i = 1;i < args->count;i++) {
    script_var_unset(args->items[i]);
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
  Foreign foreign;
  if (!foreign_analyze_file(path, &foreign) || !lang_installed(foreign.language)) {
    snprintf(foreign.language, sizeof(foreign.language), "%s", lang_current_name());
  }
  const char *keyword = strcmp(foreign.language, "sh") == 0 ? "." : "source";
  snprintf(line, size, "%s %s", keyword, quoted ? quoted : path);
  int status = lang_run_as(foreign.language, line);
  if (!lang_last_stopped()) {
    snprintf(foreign.what, sizeof(foreign.what), "sourced %.120s: ran the file inside %s and copied back what it changed",
             path, foreign.language);
    foreign.tip[0] = '\0';
    foreign_explain(&foreign, path);
  }
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
    printf("tarsh runs commands and tarsh script itself. Lines written in bash, zsh or\n");
    printf("fish syntax run in that shell: tarsh recognises which one, and uses this\n");
    printf("one when it can't tell (*):\n\n");
    lang_list();
    printf("\nswitch with: lang bash | lang zsh | lang fish | lang sh\n");
    return 0;
  }
  return lang_set(args->items[1], !exec_is_sourcing());
}

static int builtin_history(ArgList *args) {
  if (args->count > 1 && strcmp(args->items[1], "clear") == 0) {
    lineedit_history_clear();
    printf("history cleared\n");
    return 0;
  }
  if (args->count > 2 && strcmp(args->items[1], "delete") == 0) {
    int removed = lineedit_history_delete(args->items[2]);
    printf("removed %d entr%s containing \"%s\"\n", removed, removed == 1 ? "y" : "ies", args->items[2]);
    return removed ? 0 : 1;
  }
  if (args->count > 2 && strcmp(args->items[1], "search") == 0) {
    int found = 0;
    for (int i = 0;i < lineedit_history_count();i++) {
      if (strstr(lineedit_history_get(i), args->items[2]) != NULL) {
        printf("%5d  %s\n", i + 1, lineedit_history_get(i));
        found++;
      }
    }
    return found ? 0 : 1;
  }
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
  if (args->count > 1 && !settings && strcmp(args->items[1], "rc") != 0 && strcmp(args->items[1], "theme") != 0) {
    fprintf(stderr, "usage: config [rc|settings|theme]\n");
    return 2;
  }
  int theme_file = args->count > 1 && strcmp(args->items[1], "theme") == 0;
  const char *path = theme_file ? theme_path() : settings ? config_settings_path() : config_rc_path();

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
  if (theme_file) {
    theme_init();
  }
  else if (settings) {
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

// The command that installs `package` with this system's package manager.
static void install_hint(char *out, size_t size, const char *package) {
  char found[PATH_MAX];
  static const struct { const char *tool; const char *command; } managers[] = {
    { "apt-get", "sudo apt install %s" }, { "dnf", "sudo dnf install %s" }, { "pacman", "sudo pacman -S %s" },
    { "zypper", "sudo zypper install %s" }, { "apk", "sudo apk add %s" }, { "xbps-install", "sudo xbps-install %s" },
    { "brew", "brew install %s" }, { NULL, NULL }
  };
  for (int i = 0;managers[i].tool;i++) {
    if (path_lookup(managers[i].tool, found, sizeof(found))) {
      snprintf(out, size, managers[i].command, package);
      return;
    }
  }
  snprintf(out, size, "install %s with your package manager", package);
}

static int doctor_color = 1;

static void doctor_line(int good, const char *what, const char *detail, const char *fix) {
  const char *mark = good ? (doctor_color ? "\x1b[32m\u2713\x1b[0m" : "\u2713") : (doctor_color ? "\x1b[33m!\x1b[0m" : "!");
  printf("  %s %-16s %s\n", mark, what, detail);
  if (!good && fix && fix[0]) {
    printf(doctor_color ? "    \x1b[2m\u2192 %s\x1b[0m\n" : "    \u2192 %s\n", fix);
  }
}

// tarsh doctor: is everything set up? Says how to fix what isn't.
static int builtin_doctor(void) {
  char found[PATH_MAX];
  char fix[512];
  char detail[PATH_MAX + 64];
  int problems = 0;

  char self[PATH_MAX];
  ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
  self[length > 0 ? length : 0] = '\0';
  doctor_color = isatty(STDOUT_FILENO);
  printf("\ntarsh doctor\n\n");

  snprintf(detail, sizeof(detail), "%s", self[0] ? self : "(unknown)");
  doctor_line(1, "tarsh", detail, NULL);

  int on_path = path_lookup("tarsh", found, sizeof(found));
  snprintf(detail, sizeof(detail), on_path ? "typing tarsh runs %s" : "tarsh isn't on your PATH", found);
  snprintf(fix, sizeof(fix), "add its folder to PATH in ~/.profile, or reinstall with ./install.sh");
  doctor_line(on_path, "PATH", detail, fix);
  problems += !on_path;

  snprintf(detail, sizeof(detail), "%s (config.t, tarshrc%s)", config_directory(),
           access(theme_path(), F_OK) == 0 ? ", theme.t" : "");
  doctor_line(access(config_rc_path(), F_OK) == 0, "settings", detail, "start tarsh once interactively to create them");

  struct { const char *tool; const char *package; const char *why; } tools[] = {
    { "fzf", "fzf", "pickers: cd + space, Tab, Ctrl+R" },
    { "git", "git", "branch and changes in the prompt" },
    { "bash", "bash", "runs bash syntax and scripts" },
    { "zsh", "zsh", "runs zsh syntax" },
    { "fish", "fish", "runs fish syntax" },
    { "man", "man-db", "option descriptions on Tab" },
  };
  for (size_t i = 0;i < sizeof(tools) / sizeof(tools[0]);i++) {
    int have = path_lookup(tools[i].tool, found, sizeof(found));
    snprintf(detail, sizeof(detail), "%s%s", have ? "" : "missing: ", tools[i].why);
    install_hint(fix, sizeof(fix), tools[i].package);
    doctor_line(have, tools[i].tool, detail, fix);
    problems += !have && i < 3;
  }

  const char *colorterm = getenv("COLORTERM");
  const char *term = getenv("TERM");
  int truecolor = colorterm && (strcmp(colorterm, "truecolor") == 0 || strcmp(colorterm, "24bit") == 0);
  snprintf(detail, sizeof(detail), "TERM=%s%s", term ? term : "(unset)", truecolor ? ", true color" : ", 256 colors");
  doctor_line(1, "colors", detail, NULL);

  printf("  ? %-16s %s\n", "icons", "if these look like icons, your font is ready:  \uE0A0 \uF07C \uE73C \uF17C \uE0B0");
  printf("    \u2192 boxes instead? install a Nerd Font (nerdfonts.com) and pick it in your terminal\n");

  const char *login = getenv("SHELL");
  int is_login = login && strstr(login, "tarsh") != NULL;
  int listed = 0;
  FILE *shells = fopen("/etc/shells", "r");
  if (shells) {
    char line[PATH_MAX];
    while (fgets(line, sizeof(line), shells)) {
      line[strcspn(line, "\n")] = '\0';
      if (self[0] && strcmp(line, self) == 0) listed = 1;
    }
    fclose(shells);
  }
  snprintf(detail, sizeof(detail), is_login ? "tarsh is your login shell" : "your login shell is %s (that's fine)",
           login ? login : "unknown");
  doctor_line(1, "login shell", detail, NULL);
  if (!is_login) {
    printf("    \u2192 to use tarsh by default: %s\n",
           listed ? "chsh -s $(command -v tarsh)" : "./install.sh --default-shell");
  }

  printf("\n%s\n\n", problems ? "a few things to fix above; everything else is optional."
                                 : "all good. type help for a tour, or theme edit to style your prompt.");
  return problems ? 1 : 0;
}

static void theme_usage(void) {
  printf("usage: theme [edit | list | use NAME | on | off | reload | path]\n\n");
  printf("  theme edit         open the theme editor in your browser\n");
  printf("                     (--no-open: just print the link, --port N: pick the port)\n");
  printf("  theme list         show the built-in themes\n");
  printf("  theme use NAME     switch to a built-in theme (replaces theme.t)\n");
  printf("  theme off / on     go back to the classic prompt / use theme.t again\n");
  printf("  theme reload       re-read theme.t after editing it by hand\n");
  printf("  theme path         where theme.t lives\n");
}

// The prompt theming engine: see theme.h.
static int builtin_theme(ArgList *args) {
  const char *action = args->count > 1 ? args->items[1] : "";

  if (action[0] == '\0') {
    if (theme_active()) {
      printf("theme: %s (%s)\n\n", theme_current_name(), theme_path());
    }
    else {
      printf("theme: classic prompt (no theme in use)\n\n");
    }
    theme_usage();
    return 0;
  }
  if (strcmp(action, "edit") == 0) {
    int open_browser = 1;
    int port = 0;
    for (int i = 2;i < args->count;i++) {
      if (strcmp(args->items[i], "--no-open") == 0) {
        open_browser = 0;
      }
      else if (strcmp(args->items[i], "--port") == 0 && i + 1 < args->count) {
        port = atoi(args->items[++i]);
      }
      else {
        fprintf(stderr, "tarsh: theme edit: unknown option %s\n", args->items[i]);
        return 2;
      }
    }
    return theme_editor_run(open_browser, port);
  }
  if (strcmp(action, "list") == 0) {
    for (int i = 0;i < theme_preset_count();i++) {
      printf("  %-10s %s\n", theme_preset_name(i), theme_preset_description(i));
    }
    printf("\nswitch with: theme use NAME   (or design your own: theme edit)\n");
    return 0;
  }
  if (strcmp(action, "use") == 0) {
    if (args->count < 3) {
      fprintf(stderr, "usage: theme use NAME   (see: theme list)\n");
      return 2;
    }
    int index = theme_preset_find(args->items[2]);
    if (index < 0) {
      fprintf(stderr, "tarsh: theme: no theme called '%s' (see: theme list)\n", args->items[2]);
      return 1;
    }
    if (access(theme_path(), F_OK) == 0) {
      // Keep the previous theme around, just in case.
      char backup[PATH_MAX + 16];
      snprintf(backup, sizeof(backup), "%s.bak", theme_path());
      rename(theme_path(), backup);
      printf("your previous theme is saved as %s\n", backup);
    }
    if (theme_save_text(theme_preset_text(index)) != 0) {
      fprintf(stderr, "tarsh: theme: can't write %s: %s\n", theme_path(), strerror(errno));
      return 1;
    }
    printf("now using the %s theme\n", args->items[2]);
    return 0;
  }
  if (strcmp(action, "on") == 0 || strcmp(action, "off") == 0) {
    int enable = strcmp(action, "on") == 0;
    if (access(theme_path(), F_OK) != 0) {
      if (!enable) {
        printf("no theme in use already\n");
        return 0;
      }
      fprintf(stderr, "tarsh: theme: there's no theme.t yet. try: theme list, or theme edit\n");
      return 1;
    }
    if (theme_set_enabled(enable) != 0) {
      fprintf(stderr, "tarsh: theme: can't update %s: %s\n", theme_path(), strerror(errno));
      return 1;
    }
    printf(enable ? "theme on\n" : "theme off: back to the classic prompt (theme on brings it back)\n");
    return 0;
  }
  if (strcmp(action, "reload") == 0) {
    theme_init();
    printf(theme_active() ? "reloaded %s\n" : "no theme in use (%s)\n", theme_path());
    return 0;
  }
  if (strcmp(action, "path") == 0) {
    printf("%s\n", theme_path());
    return 0;
  }
  theme_usage();
  return 2;
}

static int builtin_help(void) {
  printf("tarsh - a beginner-friendly shell        full guide: man tarsh, or docs/GUIDE.md\n\n");
  printf("finding things:\n");
  printf("  cd + space       pick a folder (nvim, cat, less, code: files; cp/mv: what, then where)\n");
  printf("  Tab              complete commands, paths, and options with their descriptions\n");
  printf("  Ctrl+R           search your history          Up arrow  search for what you've typed\n");
  printf("  Right arrow      accept the grey suggestion   Alt+.     last word of the last command\n\n");
  printf("combining commands:\n");
  printf("  a | b      a's output into b        a > f   output to a file   a >> f  add to a file\n");
  printf("  a && b     b if a worked            a || b  b if a failed      a ; b   one after the other\n");
  printf("  a &        in the background        $(a)    a's output as text *.txt every .txt file\n");
  printf("  a; and b / a; or b / not a           fish style works too\n\n");
  printf("programs:\n");
  printf("  Ctrl+C stop   Ctrl+Z pause   fg bring back   bg keep running behind   jobs   kill %%1\n\n");
  printf("folders:\n");
  printf("  cd -   prevd / nextd (Alt+Left/Right)   cdh recent folders   dirh history   ../ goes there\n\n");
  printf("variables:\n");
  printf("  x=1   export X=1   set list a b c   set -U NAME value (saved, shared)   set -e NAME\n\n");
  printf("tarsh script (a Python-like language, docs/SCRIPTING.md):\n");
  printf("  x = 6 * 7          for f in glob(\"*.txt\"):     def greet(name):     if ?(grep -q x f):\n");
  printf("  print(f\"{x}\")       out = $(date)              funcsave greet       functions\n\n");
  printf("other shells:\n");
  printf("  paste bash, zsh or fish syntax: it runs there and a note explains what happened\n");
  printf("  lang [name]      which shell to use when tarsh can't tell (now: %s)\n", lang_current_name());
  printf("  explain off      hide those notes\n\n");
  printf("builtins:\n");
  printf("  cd pwd echo export unset set alias unalias abbr source type read shift umask exec\n");
  printf("  string math count contains random history jobs fg bg wait kill functions funcsave funced\n\n");
  printf("your setup:\n");
  printf("  config           your startup file: aliases, abbreviations, variables (tarshrc)\n");
  printf("  config settings  prompt, pickers, highlighting, suggestions (config.t)\n");
  printf("  theme edit       design your prompt in the browser     theme list  ready-made ones\n");
  printf("  import-rc        bring over aliases from bash/zsh      tarsh doctor  check your setup\n");
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

  if (fish_builtin_run(args, status)) return 1;
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
  else if (strcmp(name, "theme") == 0) *status = builtin_theme(args);
  else if (strcmp(name, "doctor") == 0) *status = builtin_doctor();
  else if (strcmp(name, "explain") == 0) {
    if (args->count > 1 && (strcmp(args->items[1], "on") == 0 || strcmp(args->items[1], "off") == 0)) {
      foreign_set_enabled(strcmp(args->items[1], "on") == 0);
      printf("notes after bash/zsh/fish commands are %s (set EXPLAIN in config.t to keep it)\n",
             foreign_enabled() ? "on" : "off");
      *status = 0;
    }
    else {
      printf("explain is %s. usage: explain on|off\n", foreign_enabled() ? "on" : "off");
      *status = args->count > 1 ? 2 : 0;
    }
  }
  else if (strcmp(name, "help") == 0) *status = builtin_help();
  else if (strcmp(name, "exit") == 0) *status = builtin_exit(args, should_exit);
  else return 0;

  return 1;
}
