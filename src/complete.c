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
#include "parser.h"
#include "util.h"

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

// ---------------------------------------------------------------------------
// Options with descriptions, from the man page (or --help for system tools)
// ---------------------------------------------------------------------------

#define OPTION_CACHE 16

typedef struct {
  char command[64];
  ArgList names;
  ArgList descriptions;
}OptionSet;

static OptionSet option_cache[OPTION_CACHE];
static int option_cache_next = 0;

static char *trim_copy(const char *text, size_t length) {
  while (length > 0 && (*text == ' ' || *text == '\t')) { text++; length--; }
  while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\n')) length--;
  return strndup(text, length);
}

static void add_option(OptionSet *set, const char *token, size_t length, const char *description) {
  // --color[=WHEN] -> --color ; --width=COLS -> --width=
  size_t cut = 0;
  int wants_value = 0;
  while (cut < length && !strchr("[=<, ", token[cut])) cut++;
  if (cut < length && token[cut] == '=') wants_value = 1;
  if (cut < 2) return;
  char option[128];
  snprintf(option, sizeof(option), "%.*s%s", (int)cut, token, wants_value ? "=" : "");
  for (int i = 0;i < set->names.count;i++) {
    if (strcmp(set->names.items[i], option) == 0) return;
  }
  arglist_append(&set->names, option);
  arglist_append(&set->descriptions, description);
}

static void parse_option_lines(FILE *source, OptionSet *set) {
  char *line = NULL;
  size_t size = 0;
  char *pending[8];
  int pending_count = 0;

  while (getline(&line, &size, source) != -1 && set->names.count < 300) {
    line[strcspn(line, "\n")] = '\0';
    // Strip man's overstrike bold/underline (x\bx).
    size_t used = 0;
    for (size_t i = 0;line[i];i++) {
      if (line[i + 1] == '\b' && line[i + 2]) { continue; }
      if (line[i] == '\b') continue;
      line[used++] = line[i];
    }
    line[used] = '\0';

    const char *text = line;
    while (*text == ' ' || *text == '\t') text++;

    // A description on the line after the option (man page style).
    if (pending_count > 0) {
      if (*text && *text != '-') {
        char *description = trim_copy(text, strlen(text));
        for (int i = 0;i < pending_count;i++) {
          add_option(set, pending[i], strlen(pending[i]), description);
          free(pending[i]);
        }
        free(description);
        pending_count = 0;
        continue;
      }
      if (*text == '-') {
        for (int i = 0;i < pending_count;i++) {
          add_option(set, pending[i], strlen(pending[i]), "");
          free(pending[i]);
        }
        pending_count = 0;
      }
    }
    if (*text != '-' || text[1] == '\0' || text[1] == ' ') continue;

    // Options run until two spaces (or a tab); the rest is the description.
    const char *gap = text;
    while (*gap && !(gap[0] == ' ' && gap[1] == ' ') && *gap != '\t') gap++;
    char *options = strndup(text, gap - text);
    char *description = trim_copy(gap, strlen(gap));

    char *cursor = options;
    while (*cursor) {
      while (*cursor == ' ' || *cursor == ',') cursor++;
      if (*cursor != '-') {
        while (*cursor && *cursor != ' ' && *cursor != ',') cursor++;
        continue;
      }
      size_t length = strcspn(cursor, " ,");
      if (description[0]) {
        add_option(set, cursor, length, description);
      }
      else if (pending_count < 8) {
        pending[pending_count++] = strndup(cursor, length);
      }
      cursor += length;
    }
    free(options);
    free(description);
  }
  for (int i = 0;i < pending_count;i++) {
    add_option(set, pending[i], strlen(pending[i]), "");
    free(pending[i]);
  }
  free(line);
}

int complete_options(const char *command, ArgList **names, ArgList **descriptions) {
  if (strlen(command) >= 64 || strchr(command, '/') != NULL || command[0] == '\0') {
    return 0;
  }
  for (int i = 0;i < OPTION_CACHE;i++) {
    if (strcmp(option_cache[i].command, command) == 0) {
      *names = &option_cache[i].names;
      *descriptions = &option_cache[i].descriptions;
      return option_cache[i].names.count;
    }
  }

  char resolved[PATH_MAX];
  if (!path_lookup(command, resolved, sizeof(resolved))) {
    return 0;
  }

  OptionSet *set = &option_cache[option_cache_next];
  option_cache_next = (option_cache_next + 1) % OPTION_CACHE;
  if (set->command[0]) {
    arglist_free(&set->names);
    arglist_free(&set->descriptions);
  }
  snprintf(set->command, sizeof(set->command), "%s", command);
  arglist_init(&set->names);
  arglist_init(&set->descriptions);

  char *quoted = shell_quote(command);
  char shell_command[512];
  char found[PATH_MAX];
  if (path_lookup("man", found, sizeof(found))) {
    snprintf(shell_command, sizeof(shell_command),
             "MANWIDTH=200 MAN_KEEP_FORMATTING=0 man %s 2>/dev/null", quoted);
    FILE *man = popen(shell_command, "r");
    if (man) {
      parse_option_lines(man, set);
      pclose(man);
    }
  }
  // No man page: GNU-style tools answer --help safely. Only ask programs
  // that came with the system.
  if (set->names.count == 0 &&
      (strncmp(resolved, "/usr/bin/", 9) == 0 || strncmp(resolved, "/bin/", 5) == 0)) {
    snprintf(shell_command, sizeof(shell_command), "%s%s --help 2>&1 </dev/null",
             path_lookup("timeout", found, sizeof(found)) ? "timeout 2 " : "", quoted);
    FILE *help = popen(shell_command, "r");
    if (help) {
      parse_option_lines(help, set);
      pclose(help);
    }
  }
  free(quoted);
  *names = &set->names;
  *descriptions = &set->descriptions;
  return set->names.count;
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
  int already_listed = 0;
  if (list.items == NULL) {
    free(word);
    return;
  }
  if (!command_position && word[0] == '-') {
    // Options of the command, with what each one does.
    ArgList words;
    arglist_init(&words);
    ArgList *names = NULL, *descriptions = NULL;
    if (parser_split_words(line->data, &words) > 0 && complete_options(words.items[0], &names, &descriptions) > 0) {
      int matches = 0, last = -1;
      for (int i = 0;i < names->count;i++) {
        if (strncmp(names->items[i], word, strlen(word)) == 0) { matches++; last = i; add_candidate(&list, names->items[i]); }
      }
      if (matches > 1) {
        printf("\r\n");
        int shown = 0;
        for (int i = 0;i < names->count && shown < 40;i++) {
          if (strncmp(names->items[i], word, strlen(word)) != 0) continue;
          printf("  %-24s %.80s\r\n", names->items[i], descriptions->items[i]);
          shown++;
        }
        if (matches > shown) printf("  ... and %d more\r\n", matches - shown);
        fflush(stdout);
        already_listed = 1;
        // Still fill in the common start below, without listing again.
        for (int i = 0;i < list.count;i++) free(list.items[i]);
        list.count = 0;
        for (int i = 0;i < names->count;i++) {
          if (strncmp(names->items[i], word, strlen(word)) == 0) add_candidate(&list, names->items[i]);
        }
        (void)last;
      }
    }
    arglist_free(&words);
  }
  else if (command_position && strchr(word, '/') == NULL) {
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
    else if (list.count > 1 && !already_listed) {
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
