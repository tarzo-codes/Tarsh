#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include "builtins.h"
#include "lang.h"
#include "lineedit.h"

extern char **environ;

static const char *builtin_names[] = {
  "cd", "pwd", "echo", "export", "unset", "lang", "history", "help", "exit", NULL
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

static int builtin_echo(ArgList *args) {
  int first = 1;
  int newline = 1;

  if (args->count > 1 && strcmp(args->items[1], "-n") == 0) {
    newline = 0;
    first = 2;
  }

  for (int i = first;i < args->count;i++) {
    if (i > first) {
      printf(" ");
    }
    printf("%s", args->items[i]);
  }
  if (newline) {
    printf("\n");
  }
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

static int builtin_lang(ArgList *args) {
  if (args->count == 1) {
    printf("tarsh runs simple commands itself. pipes (|), redirects (>), &&, loops,\n");
    printf("wildcards (*) and scripts are run with your chosen shell language:\n\n");
    lang_list();
    printf("\nswitch with: lang bash | lang zsh | lang fish | lang sh\n");
    return 0;
  }
  return lang_set(args->items[1]);
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

static int builtin_help(void) {
  printf("tarsh - a beginner-friendly shell\n\n");
  printf("finding things (needs fzf):\n");
  printf("  cd<space>        pick a folder to go into\n");
  printf("  nvim<space>      pick files to edit (also vim, nano, cat, less, code)\n");
  printf("  cp<space>        pick what to copy, then where to put it (mv works the same)\n");
  printf("  rm<space>        pick files to delete (Tab selects several)\n");
  printf("  Tab              pick a command, or a path for the word you're typing\n");
  printf("  Ctrl+R           search commands you ran before\n");
  printf("  in a picker: type to filter, Enter to choose, Esc to type it yourself\n\n");
  printf("editing the line:\n");
  printf("  Up/Down          previous/next command      Ctrl+A/Ctrl+E  start/end of line\n");
  printf("  Ctrl+W           delete a word              Ctrl+U         delete to start\n");
  printf("  Ctrl+C           cancel the line or stop the running command\n");
  printf("  Ctrl+L           clear the screen           Ctrl+D         exit (on an empty line)\n\n");
  printf("builtins:\n");
  printf("  cd [dir|-]       change folder (- goes back to the previous one)\n");
  printf("  pwd              show which folder you're in\n");
  printf("  echo [-n] ...    print text\n");
  printf("  export N=V       set a variable (N=V on its own works too)\n");
  printf("  unset NAME       remove a variable\n");
  printf("  lang [name]      show or change the shell language (bash, zsh, fish, sh)\n");
  printf("  history [n]      list previous commands\n");
  printf("  help             show this message\n");
  printf("  exit [status]    leave the shell\n\n");
  printf("anything with pipes, redirects, &&, loops or wildcards runs in your shell\n");
  printf("language (currently %s). settings live in ~/.config/tarsh/config.t\n",
         lang_current_name());
  return 0;
}

static int builtin_exit(ArgList *args, int *should_exit) {
  *should_exit = 1;
  if (args->count > 1) {
    return atoi(args->items[1]) & 0xFF;
  }
  return 0;
}

static int is_assignment(const char *word) {
  const char *separator = strchr(word, '=');
  if (separator == NULL || separator == word) {
    return 0;
  }
  for (const char *c = word;c < separator;c++) {
    int letter = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '_';
    int digit = (*c >= '0' && *c <= '9');
    if (!letter && !(digit && c != word)) {
      return 0;
    }
  }
  return 1;
}

int builtin_run(ArgList *args, int *status, int *should_exit) {
  if (args->count == 0) {
    return 0;
  }

  const char *name = args->items[0];

  // A bare NAME=value sets a variable for this session.
  if (args->count == 1 && is_assignment(name)) {
    char *separator = strchr(args->items[0], '=');
    *separator = '\0';
    setenv(args->items[0], separator + 1, 1);
    *separator = '=';
    *status = 0;
    return 1;
  }

  if (strcmp(name, "cd") == 0) {
    *status = builtin_cd(args);
  }
  else if (strcmp(name, "pwd") == 0) {
    *status = builtin_pwd();
  }
  else if (strcmp(name, "echo") == 0) {
    *status = builtin_echo(args);
  }
  else if (strcmp(name, "export") == 0) {
    *status = builtin_export(args);
  }
  else if (strcmp(name, "unset") == 0) {
    *status = builtin_unset(args);
  }
  else if (strcmp(name, "lang") == 0) {
    *status = builtin_lang(args);
  }
  else if (strcmp(name, "history") == 0) {
    *status = builtin_history(args);
  }
  else if (strcmp(name, "help") == 0) {
    *status = builtin_help();
  }
  else if (strcmp(name, "exit") == 0) {
    *status = builtin_exit(args, should_exit);
  }
  else {
    return 0;
  }

  return 1;
}
