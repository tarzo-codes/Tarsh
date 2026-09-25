#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

#include "suggest.h"
#include "builtins.h"

#define NAME_LIMIT 64

// Commands people type out of habit from Windows or other tools.
static const struct {
  const char *typed;
  const char *hint;
}habits[] = {
  { "cls",      "clear" },
  { "dir",      "ls" },
  { "copy",     "cp" },
  { "move",     "mv" },
  { "del",      "rm" },
  { "type",     "cat" },
  { "ipconfig", "ip addr" },
  { "cd..",     "cd .." },
  { "md",       "mkdir" },
  { "rd",       "rmdir" },
  { "where",    "which" },
  { "tasklist", "ps aux" },
  { "python",   "python3" },
  { "pip",      "pip3 (or: python3 -m pip)" },
  { NULL, NULL }
};

// Commands beginners use most; they win ties, so `sl` suggests `ls`
// rather than some obscure tool that is equally close.
static const char *common_commands[] = {
  "ls", "cd", "cat", "cp", "mv", "rm", "mkdir", "touch", "grep", "find",
  "git", "nvim", "vim", "nano", "less", "man", "clear", "echo", "pwd",
  "python3", "sudo", "apt", "top", "ps", "kill", "ssh", "curl", NULL
};

// Edit distance where swapping two neighbouring letters counts as one
// typo (optimal string alignment), with rows sized for NAME_LIMIT.
static int edit_distance(const char *a, const char *b) {
  size_t a_length = strlen(a);
  size_t b_length = strlen(b);
  if (a_length >= NAME_LIMIT || b_length >= NAME_LIMIT) {
    return NAME_LIMIT;
  }

  int before[NAME_LIMIT + 1];
  int previous[NAME_LIMIT + 1];
  int current[NAME_LIMIT + 1];
  for (size_t j = 0;j <= b_length;j++) {
    previous[j] = (int)j;
    before[j] = (int)j;
  }

  for (size_t i = 1;i <= a_length;i++) {
    current[0] = (int)i;
    for (size_t j = 1;j <= b_length;j++) {
      int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      int best = previous[j - 1] + cost;
      if (previous[j] + 1 < best) best = previous[j] + 1;
      if (current[j - 1] + 1 < best) best = current[j - 1] + 1;
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] &&
          before[j - 2] + 1 < best) {
        best = before[j - 2] + 1;
      }
      current[j] = best;
    }
    memcpy(before, previous, (b_length + 1) * sizeof(int));
    memcpy(previous, current, (b_length + 1) * sizeof(int));
  }
  return previous[b_length];
}

typedef struct {
  char name[NAME_LIMIT];
  int distance;
}Suggestion;

static void consider(Suggestion *best, const char *typed, const char *candidate) {
  if (strcmp(typed, candidate) == 0) {
    return;
  }
  int distance = edit_distance(typed, candidate);
  if (distance < best->distance) {
    best->distance = distance;
    snprintf(best->name, sizeof(best->name), "%s", candidate);
  }
}

void suggest_command_not_found(const char *name) {
  fprintf(stderr, "tarsh: command not found: %s\n", name);

  for (int i = 0;habits[i].typed != NULL;i++) {
    if (strcmp(habits[i].typed, name) == 0) {
      fprintf(stderr, "  on Linux that's: %s\n", habits[i].hint);
      return;
    }
  }

  struct stat info;
  if (stat(name, &info) == 0 && S_ISDIR(info.st_mode)) {
    fprintf(stderr, "  %s is a folder. to go into it: cd %s\n", name, name);
    return;
  }
  if (stat(name, &info) == 0 && S_ISREG(info.st_mode)) {
    fprintf(stderr, "  %s is a file here. to run it: ./%s   (to read it: cat %s)\n",
            name, name, name);
    return;
  }

  Suggestion best = { "", NAME_LIMIT };
  for (int i = 0;common_commands[i] != NULL;i++) {
    consider(&best, name, common_commands[i]);
  }
  for (int i = 0;builtin_name_at(i) != NULL;i++) {
    consider(&best, name, builtin_name_at(i));
  }

  const char *path = getenv("PATH");
  char *copy = path ? strdup(path) : NULL;
  if (copy != NULL) {
    for (char *dir = strtok(copy, ":");dir != NULL;dir = strtok(NULL, ":")) {
      DIR *listing = opendir(dir);
      if (listing == NULL) {
        continue;
      }
      struct dirent *entry;
      while ((entry = readdir(listing)) != NULL) {
        if (entry->d_name[0] != '.') {
          consider(&best, name, entry->d_name);
        }
      }
      closedir(listing);
    }
    free(copy);
  }

  // One typo per four letters is about where suggestions stop helping.
  size_t length = strlen(name);
  int allowed = (length <= 4) ? 1 : (int)(length / 4) + 1;
  if (allowed > 2) {
    allowed = 2;
  }
  if (best.name[0] != '\0' && best.distance <= allowed) {
    fprintf(stderr, "  did you mean: %s\n", best.name);
  }
}
