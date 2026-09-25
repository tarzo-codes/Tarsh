#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include "builtins.h"

extern char **environ;

static const char *builtin_names[] = {
  "cd", "pwd", "echo", "export", "unset", "help", "exit", NULL
};

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

static int builtin_help(void) {
  printf("tarsh - a no-frills Unix shell\n\n");
  printf("builtins:\n");
  printf("  cd [dir|-]      change the working directory\n");
  printf("  pwd             print the working directory\n");
  printf("  echo [-n] ...   print arguments\n");
  printf("  export N=V      set an environment variable\n");
  printf("  unset NAME      remove an environment variable\n");
  printf("  help            show this message\n");
  printf("  exit [status]   leave the shell\n\n");
  printf("anything else is looked up on PATH and run in a child process.\n");
  printf("prompt settings live in ~/.config/tarsh/config.t\n");
  return 0;
}

static int builtin_exit(ArgList *args, int *should_exit) {
  *should_exit = 1;
  if (args->count > 1) {
    return atoi(args->items[1]) & 0xFF;
  }
  return 0;
}

int builtin_run(ArgList *args, int *status, int *should_exit) {
  if (args->count == 0) {
    return 0;
  }

  const char *name = args->items[0];

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
