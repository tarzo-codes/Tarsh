#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include "complete.h"
#include "alias.h"
#include "builtins.h"

#define MAX_CANDIDATES 4096

typedef struct {
  char **items;
  int count;
}Candidates;

static void add_candidate(Candidates *list, const char *text) {
  if (list->count >= MAX_CANDIDATES) {
    return;
  }
  for (int i = 0;i < list->count;i++) {
    if (strcmp(list->items[i], text) == 0) {
      return;
    }
  }
  char *copy = strdup(text);
  if (copy != NULL) {
    list->items[list->count++] = copy;
  }
}

static int compare_strings(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void collect_commands(Candidates *list, const char *prefix) {
  size_t length = strlen(prefix);
  for (int i = 0;builtin_name_at(i) != NULL;i++) {
    if (strncmp(builtin_name_at(i), prefix, length) == 0) {
      add_candidate(list, builtin_name_at(i));
    }
  }

  const char *path = getenv("PATH");
  char *copy = path ? strdup(path) : NULL;
  if (copy == NULL) {
    return;
  }
  for (char *dir = strtok(copy, ":");dir != NULL;dir = strtok(NULL, ":")) {
    DIR *listing = opendir(dir);
    if (listing == NULL) {
      continue;
    }
    struct dirent *entry;
    while ((entry = readdir(listing)) != NULL) {
      if (strncmp(entry->d_name, prefix, length) != 0 || entry->d_name[0] == '.') {
        continue;
      }
      char full[PATH_MAX];
      snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);
      if (access(full, X_OK) == 0) {
        add_candidate(list, entry->d_name);
      }
    }
    closedir(listing);
  }
  free(copy);
}

// Candidates are full replacement words, e.g. "src/ma" -> "src/main.c".
static void collect_paths(Candidates *list, const char *word) {
  const char *slash = strrchr(word, '/');
  char shown_dir[PATH_MAX] = "";
  char real_dir[PATH_MAX] = ".";
  const char *prefix = word;

  if (slash != NULL) {
    snprintf(shown_dir, sizeof(shown_dir), "%.*s", (int)(slash - word + 1), word);
    prefix = slash + 1;
    // ~/ is shown as typed but listed from $HOME.
    if (shown_dir[0] == '~' && getenv("HOME") != NULL) {
      snprintf(real_dir, sizeof(real_dir), "%s%s", getenv("HOME"), shown_dir + 1);
    }
    else {
      snprintf(real_dir, sizeof(real_dir), "%s", shown_dir);
    }
  }

  DIR *listing = opendir(real_dir);
  if (listing == NULL) {
    return;
  }
  size_t length = strlen(prefix);
  struct dirent *entry;
  while ((entry = readdir(listing)) != NULL) {
    const char *name = entry->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    // Hidden files only when asked for with a leading dot.
    if (name[0] == '.' && prefix[0] != '.') {
      continue;
    }
    if (strncmp(name, prefix, length) != 0) {
      continue;
    }
    char full[PATH_MAX * 2];
    snprintf(full, sizeof(full), "%s/%s", real_dir, name);
    struct stat info;
    int is_dir = stat(full, &info) == 0 && S_ISDIR(info.st_mode);

    char candidate[PATH_MAX * 2];
    snprintf(candidate, sizeof(candidate), "%s%s%s", shown_dir, name, is_dir ? "/" : "");
    add_candidate(list, candidate);
  }
  closedir(listing);
}

// Backslash-escapes characters the parser would otherwise split on.
static char *escape_word(const char *word) {
  char *out = malloc(strlen(word) * 2 + 1);
  if (out == NULL) {
    return NULL;
  }
  char *cursor = out;
  for (const char *c = word;*c != '\0';c++) {
    if (strchr(" \t'\"\\$`&|;<>()*?[]!#{}", *c) != NULL && !(c == word && *c == '~')) {
      *cursor++ = '\\';
    }
    *cursor++ = *c;
  }
  *cursor = '\0';
  return out;
}

// Undoes backslash escapes in what the user typed so far.
static char *unescape_word(const char *word, size_t length) {
  char *out = malloc(length + 1);
  if (out == NULL) {
    return NULL;
  }
  size_t used = 0;
  for (size_t i = 0;i < length;i++) {
    if (word[i] == '\\' && i + 1 < length) {
      i++;
    }
    out[used++] = word[i];
  }
  out[used] = '\0';
  return out;
}

static void show_candidates(const Candidates *list) {
  struct winsize size;
  int width = 80;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
    width = size.ws_col;
  }

  size_t longest = 0;
  for (int i = 0;i < list->count;i++) {
    const char *shown = strrchr(list->items[i], '/');
    // Show just the last part of paths (keep the trailing / of folders).
    const char *name = list->items[i];
    if (shown != NULL && shown[1] != '\0') {
      name = shown + 1;
    }
    size_t length = strlen(name);
    if (length > longest) longest = length;
  }
  int columns = width / (int)(longest + 2);
  if (columns < 1) columns = 1;

  printf("\r\n");
  int shown_count = list->count > 200 ? 200 : list->count;
  for (int i = 0;i < shown_count;i++) {
    const char *slash = strrchr(list->items[i], '/');
    const char *name = (slash != NULL && slash[1] != '\0') ? slash + 1 : list->items[i];
    printf("%-*s", (int)(longest + 2), name);
    if ((i + 1) % columns == 0 || i == shown_count - 1) {
      printf("\r\n");
    }
  }
  if (list->count > shown_count) {
    printf("... and %d more\r\n", list->count - shown_count);
  }
  fflush(stdout);
}

void complete_basic(EditBuffer *line) {
  // Find the start of the word before the cursor (spaces can be escaped).
  size_t start = line->cursor;
  while (start > 0) {
    if (line->data[start - 1] == ' ' && !(start >= 2 && line->data[start - 2] == '\\')) {
      break;
    }
    start--;
  }
  size_t typed_length = line->cursor - start;
  char *word = unescape_word(line->data + start, typed_length);
  if (word == NULL) {
    return;
  }

  int command_position = 1;
  for (size_t i = 0;i < start;i++) {
    if (line->data[i] != ' ') {
      command_position = 0;
      break;
    }
  }

  Candidates list = { calloc(MAX_CANDIDATES, sizeof(char *)), 0 };
  if (list.items == NULL) {
    free(word);
    return;
  }
  if (command_position && strchr(word, '/') == NULL) {
    collect_commands(&list, word);
  }
  else {
    collect_paths(&list, word);
  }

  if (list.count == 0) {
    if (write(STDOUT_FILENO, "\a", 1) < 0) {}
  }
  else {
    qsort(list.items, list.count, sizeof(char *), compare_strings);

    // Longest common prefix of all candidates.
    size_t common = strlen(list.items[0]);
    for (int i = 1;i < list.count;i++) {
      size_t j = 0;
      while (j < common && list.items[i][j] == list.items[0][j]) j++;
      common = j;
    }

    char *completion = strndup(list.items[0], common);
    if (completion != NULL && strlen(completion) > strlen(word)) {
      char *escaped = escape_word(completion);
      if (escaped != NULL) {
        editbuf_delete_before(line, typed_length);
        editbuf_insert(line, escaped);
        // A finished word gets a space; a folder keeps going.
        if (list.count == 1 && completion[common - 1] != '/') {
          editbuf_insert(line, " ");
        }
        free(escaped);
      }
    }
    else if (list.count > 1) {
      show_candidates(&list);
    }
    else if (list.count == 1 && completion != NULL && completion[0] != '\0' &&
             completion[strlen(completion) - 1] != '/') {
      editbuf_insert(line, " ");
    }
    free(completion);
  }

  for (int i = 0;i < list.count;i++) {
    free(list.items[i]);
  }
  free(list.items);
  free(word);
}
