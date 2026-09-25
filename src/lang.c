#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

#include "lang.h"
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

int lang_set(const char *name) {
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
  printf("now using %s syntax for pipes, redirects, loops and scripts\n", name);
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

// Words that only make sense to a real shell interpreter when they start
// a line: control flow, function definitions, and fish's builtins.
static const char *backend_keywords[] = {
  "if", "for", "while", "until", "case", "function", "select", "time",
  "source", ".", "alias", "unalias", "eval", "local", "declare",
  "typeset", "readonly", "let", "[[", "!", "{",
  "set", "begin", "switch", "and", "or", "not", "abbr", "funced",
  NULL
};

int lang_needs_backend(const char *line) {
  // Check the first word against the keyword list.
  const char *word = line;
  while (*word == ' ' || *word == '\t') {
    word++;
  }
  size_t word_length = strcspn(word, " \t");
  for (int i = 0;backend_keywords[i] != NULL;i++) {
    if (strlen(backend_keywords[i]) == word_length &&
        strncmp(word, backend_keywords[i], word_length) == 0) {
      return 1;
    }
  }

  // `FOO=bar some-command` is a per-command assignment; bare `FOO=bar`
  // is handled natively as an export.
  const char *equals = memchr(word, '=', word_length);
  if (equals != NULL && equals != word && word[word_length] != '\0') {
    return 1;
  }

  char quote = '\0';
  int at_word_start = 1;
  for (const char *c = line;*c != '\0';c++) {
    if (quote == '\'') {
      if (*c == '\'') {
        quote = '\0';
      }
      continue;
    }

    if (*c == '\\') {
      if (c[1] != '\0') {
        c++;
      }
      at_word_start = 0;
      continue;
    }

    if (quote == '"') {
      if (*c == '"') {
        quote = '\0';
      }
      else if (*c == '`' || (*c == '$' && c[1] == '(')) {
        return 1;
      }
      continue;
    }

    if (*c == '\'' || *c == '"') {
      quote = *c;
      at_word_start = 0;
      continue;
    }

    // $? is expanded natively with tarsh's own last status.
    if (*c == '$' && c[1] == '?') {
      c++;
      at_word_start = 0;
      continue;
    }
    if (strchr("|&;<>()`*?[{", *c) != NULL) {
      return 1;
    }
    if (*c == '$' && c[1] == '(') {
      return 1;
    }
    // A '#' starting a later word is a comment in every backend.
    if (*c == '#' && at_word_start) {
      return 1;
    }

    at_word_start = (*c == ' ' || *c == '\t');
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

int lang_run(const char *line) {
  char *cwd_file = make_temp_file();
  char *env_file = make_temp_file();

  if (cwd_file == NULL || env_file == NULL) {
    // Still run the command, just without syncing state back.
    char *argv[] = { (char *)active_language->name, "-c", (char *)line, NULL };
    free(cwd_file);
    free(env_file);
    return spawn_and_wait(argv);
  }

  size_t script_size = strlen(line) + strlen(active_language->epilogue) + 1;
  char *script = malloc(script_size);
  if (script == NULL) {
    free(cwd_file);
    free(env_file);
    return 1;
  }
  snprintf(script, script_size, "%s%s", line, active_language->epilogue);

  setenv("TARSH_CWD_FILE", cwd_file, 1);
  setenv("TARSH_ENV_FILE", env_file, 1);

  char *argv[] = { (char *)active_language->name, "-c", script, NULL };
  int status = spawn_and_wait(argv);

  unsetenv("TARSH_CWD_FILE");
  unsetenv("TARSH_ENV_FILE");

  sync_directory(cwd_file);
  sync_environment(env_file);

  unlink(cwd_file);
  unlink(env_file);
  free(cwd_file);
  free(env_file);
  free(script);
  return status;
}
