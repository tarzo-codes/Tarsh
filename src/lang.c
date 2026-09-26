#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

#include "lang.h"
#include "jobs.h"
#include "syntax.h"
#include "util.h"

extern char **environ;

typedef struct {
  const char *name;
  const char *install_hint;
  // Appended after the user's line: saves the status, reports the final
  // directory and environment back to tarsh, then exits with the status.
  const char *epilogue;
}Language;

#define POSIX_EPILOGUE \
  "\n__tarsh_status=$?\n" \
  "pwd > \"$TARSH_CWD_FILE\"\n" \
  "env -0 > \"$TARSH_ENV_FILE\"\n" \
  "exit $__tarsh_status\n"

static const Language languages[] = {
  { "bash", "sudo apt install bash", POSIX_EPILOGUE },
  { "zsh",  "sudo apt install zsh   (Fedora: sudo dnf install zsh, Arch: sudo pacman -S zsh)", POSIX_EPILOGUE },
  { "fish", "sudo apt install fish  (Fedora: sudo dnf install fish, Arch: sudo pacman -S fish)",
    "\nset __tarsh_status $status\n"
    "pwd > \"$TARSH_CWD_FILE\"\n"
    "env -0 > \"$TARSH_ENV_FILE\"\n"
    "exit $__tarsh_status\n" },
  { "sh",   "sh should always exist; check your system", POSIX_EPILOGUE },
};

#define LANGUAGE_COUNT (int)(sizeof(languages) / sizeof(languages[0]))

static const Language *active_language = &languages[0];

// Variables the backend changes for its own bookkeeping; copying them back
// would be wrong or noisy.
static const char *env_ignore[] = {
  "_", "SHLVL", "PWD", "OLDPWD", "TARSH_CWD_FILE", "TARSH_ENV_FILE",
  "__tarsh_status", NULL
};

static const Language *find_language(const char *name) {
  for (int i = 0;i < LANGUAGE_COUNT;i++) {
    if (strcmp(languages[i].name, name) == 0) {
      return &languages[i];
    }
  }
  return NULL;
}

static int language_installed(const Language *language) {
  char path[PATH_MAX];
  return path_lookup(language->name, path, sizeof(path));
}

void lang_setup(void) {
  // bash is the most common default; fall back to sh on minimal systems.
  active_language = language_installed(&languages[0]) ? &languages[0] : find_language("sh");
}

int lang_configure(const char *key, const char *value) {
  if (strcmp(key, "SHELL_LANG") != 0) {
    return 0;
  }
  const Language *language = find_language(value);
  if (language == NULL) {
    fprintf(stderr, "tarsh: config: SHELL_LANG '%s' is not one of bash, zsh, fish, sh\n", value);
  }
  else if (!language_installed(language)) {
    fprintf(stderr, "tarsh: config: SHELL_LANG=%s but %s is not installed, using %s\n",
            value, value, active_language->name);
  }
  else {
    active_language = language;
  }
  return 1;
}

void lang_write_defaults(FILE *file) {
  fprintf(file, "# Shell language for pipes, redirects, loops and scripts: bash, zsh, fish or sh\n");
  fprintf(file, "# (switch any time with: lang fish)\n");
  fprintf(file, "SHELL_LANG=%s\n", active_language->name);
}

const char *lang_current_name(void) {
  return active_language->name;
}

int lang_set(const char *name, int verbose) {
  const Language *language = find_language(name);
  if (language == NULL) {
    fprintf(stderr, "tarsh: lang: '%s' is not a language tarsh knows. try: bash, zsh, fish, sh\n", name);
    return 1;
  }
  if (!language_installed(language)) {
    fprintf(stderr, "tarsh: lang: %s is not installed on this system.\n", name);
    fprintf(stderr, "  install it with: %s\n", language->install_hint);
    return 1;
  }
  active_language = language;
  if (verbose) {
    printf("now using %s for loops, functions and other %s syntax\n", name, name);
  }
  return 0;
}

void lang_list(void) {
  for (int i = 0;i < LANGUAGE_COUNT;i++) {
    const Language *language = &languages[i];
    printf("  %s %-5s %s\n",
           language == active_language ? "*" : " ",
           language->name,
           language_installed(language) ? "" : "(not installed)");
  }
}

// Words that start something only a real shell language can run.
static const char *backend_keywords[] = {
  "if", "then", "elif", "else", "fi", "for", "while", "until", "do", "done",
  "case", "esac", "select", "function", "time", "coproc", "[[", "((", "!", "{", "}",
  "local", "declare", "typeset", "readonly", "let", "eval", "trap", "set", "shopt",
  // fish
  "begin", "switch", "and", "or", "not", "end", "abbr", "funced", "funcsave",
  NULL
};

static int is_keyword(const char *word, size_t length) {
  for (int i = 0;backend_keywords[i] != NULL;i++) {
    if (strlen(backend_keywords[i]) == length && strncmp(word, backend_keywords[i], length) == 0) {
      return 1;
    }
  }
  return 0;
}

// Syntax inside one word that tarsh doesn't do natively: subshells and
// fish command substitution (...), function definitions name(), $((math)),
// brace expansion {a,b}, and ** in languages where it recurses.
static int word_needs_backend(const char *word, size_t length) {
  int recursive_glob = strcmp(active_language->name, "bash") != 0 &&
                       strcmp(active_language->name, "sh") != 0;
  char quote = '\0';
  for (size_t i = 0;i < length;i++) {
    char c = word[i];
    if (quote == '\'') {
      if (c == '\'') quote = '\0';
      continue;
    }
    if (c == '\\') {
      i++;
      continue;
    }
    if (c == '$' && word[i + 1] == '(' && word[i + 2] == '(') {
      return 1;
    }
    if (c == '$' && word[i + 1] == '(') {
      // Native $(...): skip over it; its contents are checked when it runs.
      int unterminated = 0;
      i = syntax_subst_end(word, i, &unterminated) - 1;
      continue;
    }
    if (c == '$' && word[i + 1] == '{') {
      // Native ${NAME}: skip to the closing brace.
      while (i < length && word[i] != '}') i++;
      continue;
    }
    if (quote == '"') {
      if (c == '"') quote = '\0';
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      continue;
    }
    if (c == '(' || c == ')' || c == '{' || c == '}') {
      return 1;
    }
    if (recursive_glob && c == '*' && word[i + 1] == '*') {
      return 1;
    }
  }
  return 0;
}

int lang_needs_backend(const char *line) {
  int command_start = 1;
  size_t i = 0;

  while (line[i] != '\0') {
    char c = line[i];
    if (c == ' ' || c == '\t') {
      i++;
      continue;
    }
    if (c == '\n' || c == ';' || c == '|' || (c == '&' && line[i + 1] != '>')) {
      command_start = 1;
      i += ((c == '&' || c == '|') && line[i + 1] == c) ? 2 : 1;
      continue;
    }
    if (c == '#') {
      while (line[i] != '\0' && line[i] != '\n') i++;
      continue;
    }
    if (c == '<' && line[i + 1] == '<') {
      return 1;   // here-documents and here-strings
    }
    if ((c == '<' || c == '>') && line[i + 1] == '(') {
      return 1;   // process substitution
    }
    if (c == '<' || c == '>' || c == '&') {
      while (line[i] == '<' || line[i] == '>' || line[i] == '&') i++;
      continue;
    }
    if (c == '(' || c == ')') {
      return 1;
    }

    int unterminated = 0;
    size_t end = syntax_word_end(line, i, &unterminated);
    if (end == i) {
      i++;
      continue;
    }
    if (command_start && is_keyword(line + i, end - i)) {
      return 1;
    }
    if (word_needs_backend(line + i, end - i)) {
      return 1;
    }
    // After NAME=value the next word is still the command.
    const char *equals = memchr(line + i, '=', end - i);
    command_start = (command_start && equals != NULL && equals != line + i);
    i = end;
  }
  return 0;
}

static char *make_temp_file(void) {
  const char *tmp = getenv("TMPDIR");
  if (tmp == NULL || tmp[0] == '\0') {
    tmp = "/tmp";
  }
  size_t size = strlen(tmp) + 32;
  char *path = malloc(size);
  if (path == NULL) {
    return NULL;
  }
  snprintf(path, size, "%s/tarsh-XXXXXX", tmp);
  int fd = mkstemp(path);
  if (fd < 0) {
    free(path);
    return NULL;
  }
  close(fd);
  return path;
}

static void sync_directory(const char *cwd_file) {
  FILE *file = fopen(cwd_file, "r");
  if (file == NULL) {
    return;
  }
  char target[PATH_MAX];
  if (fgets(target, sizeof(target), file) != NULL) {
    target[strcspn(target, "\n")] = '\0';
    char current[PATH_MAX];
    if (target[0] != '\0' && (getcwd(current, sizeof(current)) == NULL ||
                              strcmp(current, target) != 0)) {
      if (chdir(target) == 0) {
        setenv("OLDPWD", current, 1);
        setenv("PWD", target, 1);
      }
    }
  }
  fclose(file);
}

static int env_ignored(const char *name) {
  for (int i = 0;env_ignore[i] != NULL;i++) {
    if (strcmp(env_ignore[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

// Reads the backend's `env -0` dump and copies over anything new or
// changed, so `export`, `source venv/bin/activate` and friends stick.
static void sync_environment(const char *env_file) {
  FILE *file = fopen(env_file, "r");
  if (file == NULL) {
    return;
  }

  char *entry = NULL;
  size_t entry_size = 0;
  ssize_t length;
  while ((length = getdelim(&entry, &entry_size, '\0', file)) > 0) {
    char *equals = strchr(entry, '=');
    if (equals == NULL || equals == entry) {
      continue;
    }
    *equals = '\0';
    const char *name = entry;
    const char *value = equals + 1;
    if (env_ignored(name)) {
      continue;
    }
    const char *current = getenv(name);
    if (current == NULL || strcmp(current, value) != 0) {
      setenv(name, value, 1);
    }
  }
  free(entry);
  fclose(file);
}

static int run_in(const Language *language, const char *line) {
  char *cwd_file = make_temp_file();
  char *env_file = make_temp_file();

  if (cwd_file == NULL || env_file == NULL) {
    // Still run the command, just without syncing state back.
    char *argv[] = { (char *)language->name, "-c", (char *)line, NULL };
    free(cwd_file);
    free(env_file);
    return jobs_run_argv(argv, line, NULL);
  }

  size_t script_size = strlen(line) + strlen(language->epilogue) + 1;
  char *script = malloc(script_size);
  if (script == NULL) {
    free(cwd_file);
    free(env_file);
    return 1;
  }
  snprintf(script, script_size, "%s%s", line, language->epilogue);

  setenv("TARSH_CWD_FILE", cwd_file, 1);
  setenv("TARSH_ENV_FILE", env_file, 1);

  char *argv[] = { (char *)language->name, "-c", script, NULL };
  int stopped = 0;
  int status = jobs_run_argv(argv, line, &stopped);

  unsetenv("TARSH_CWD_FILE");
  unsetenv("TARSH_ENV_FILE");

  // A job suspended with Ctrl+Z will still write these files when it
  // finishes, so they stay; otherwise bring its cd/exports back.
  if (!stopped) {
    sync_directory(cwd_file);
    sync_environment(env_file);
    unlink(cwd_file);
    unlink(env_file);
  }
  free(cwd_file);
  free(env_file);
  free(script);
  return status;
}

int lang_run(const char *line) {
  return run_in(active_language, line);
}

void lang_source_login_profiles(void) {
  const Language *sh = find_language("sh");
  const char *home = getenv("HOME");
  char profile[PATH_MAX];

  if (access("/etc/profile", R_OK) == 0) {
    run_in(sh, ". /etc/profile");
  }
  if (home != NULL) {
    snprintf(profile, sizeof(profile), "%s/.profile", home);
    if (access(profile, R_OK) == 0) {
      char *quoted = shell_quote(profile);
      char line[PATH_MAX + 16];
      snprintf(line, sizeof(line), ". %s", quoted ? quoted : profile);
      free(quoted);
      run_in(sh, line);
    }
  }
}
