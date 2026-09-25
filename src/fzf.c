#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

#include "fzf.h"
#include "parser.h"
#include "util.h"
#include "builtins.h"

typedef enum {
  PICK_DIRS,
  PICK_FILES,
  PICK_ANY
}PickKind;

// How a command wants its arguments picked. `max_args` is how many
// pickers open in a row (cp/mv pick a source, then a destination).
typedef struct {
  const char *name;
  PickKind first;
  PickKind rest;
  int max_args;
  int multi;
}PickerSpec;

static const PickerSpec known_specs[] = {
  { "cd",     PICK_DIRS,  PICK_DIRS, 1, 0 },
  { "pushd",  PICK_DIRS,  PICK_DIRS, 1, 0 },
  { "rmdir",  PICK_DIRS,  PICK_DIRS, 1, 1 },
  { "ls",     PICK_DIRS,  PICK_DIRS, 1, 0 },
  { "cp",     PICK_ANY,   PICK_DIRS, 2, 0 },
  { "mv",     PICK_ANY,   PICK_DIRS, 2, 0 },
  { "ln",     PICK_ANY,   PICK_DIRS, 2, 0 },
  { "rm",     PICK_ANY,   PICK_ANY,  1, 1 },
  { "nvim",   PICK_FILES, PICK_FILES, 1, 1 },
  { "vim",    PICK_FILES, PICK_FILES, 1, 1 },
  { "vi",     PICK_FILES, PICK_FILES, 1, 1 },
  { "nano",   PICK_FILES, PICK_FILES, 1, 1 },
  { "micro",  PICK_FILES, PICK_FILES, 1, 1 },
  { "hx",     PICK_FILES, PICK_FILES, 1, 1 },
  { "emacs",  PICK_FILES, PICK_FILES, 1, 1 },
  { "code",   PICK_ANY,   PICK_ANY,  1, 1 },
  { "cat",    PICK_FILES, PICK_FILES, 1, 1 },
  { "less",   PICK_FILES, PICK_FILES, 1, 0 },
  { "bat",    PICK_FILES, PICK_FILES, 1, 1 },
  { "source", PICK_FILES, PICK_FILES, 1, 0 },
  { "chmod",  PICK_ANY,   PICK_ANY,  1, 1 },
};

#define KNOWN_SPEC_COUNT (int)(sizeof(known_specs) / sizeof(known_specs[0]))

#define DEFAULT_COMMANDS "cd,cp,mv,rm,nvim,vim,nano,cat,less,code,source"
#define MAX_COMMANDS 64
#define COMMAND_NAME_MAX 32

static int enabled = 1;
static int available = -1;
static int missing_hint_shown = 0;
static char command_names[MAX_COMMANDS][COMMAND_NAME_MAX];
static int command_count = 0;

// Directories nobody wants to wade through in a picker.
#define PRUNE "\\( -name .git -o -name node_modules -o -name __pycache__ -o -name .cache \\) -prune -o"

static void set_commands(const char *list) {
  command_count = 0;
  const char *start = list;
  while (*start != '\0' && command_count < MAX_COMMANDS) {
    while (*start == ',' || *start == ' ') {
      start++;
    }
    size_t length = strcspn(start, ", ");
    if (length > 0 && length < COMMAND_NAME_MAX) {
      memcpy(command_names[command_count], start, length);
      command_names[command_count][length] = '\0';
      command_count++;
    }
    start += length;
  }
}

void fzf_setup(void) {
  enabled = 1;
  set_commands(DEFAULT_COMMANDS);
}

int fzf_configure(const char *key, const char *value) {
  if (strcmp(key, "FZF") == 0) {
    enabled = atoi(value) != 0;
  }
  else if (strcmp(key, "FZF_COMMANDS") == 0) {
    set_commands(value);
  }
  else {
    return 0;
  }
  return 1;
}

void fzf_write_defaults(FILE *file) {
  fprintf(file, "# fzf pickers (needs fzf installed). Typing one of these commands and\n");
  fprintf(file, "# pressing space opens a picker. Tab and Ctrl+R use fzf too.\n");
  fprintf(file, "FZF=1\n");
  fprintf(file, "FZF_COMMANDS=%s\n", DEFAULT_COMMANDS);
}

static int fzf_installed(void) {
  if (available < 0) {
    char path[PATH_MAX];
    available = path_lookup("fzf", path, sizeof(path));
  }
  return available;
}

int fzf_enabled(void) {
  return enabled && fzf_installed();
}

// Tells the user once why the picker didn't open.
static void show_missing_hint(void) {
  if (missing_hint_shown || !enabled) {
    return;
  }
  missing_hint_shown = 1;
  const char *hint =
    "\r\n  (tip: install fzf to get a file picker here:"
    " sudo apt install fzf / sudo dnf install fzf / sudo pacman -S fzf)\r\n";
  if (write(STDOUT_FILENO, hint, strlen(hint)) < 0) {}
}

static const PickerSpec *spec_for(const char *command) {
  static PickerSpec fallback;

  int listed = 0;
  for (int i = 0;i < command_count;i++) {
    if (strcmp(command_names[i], command) == 0) {
      listed = 1;
      break;
    }
  }
  if (!listed) {
    return NULL;
  }

  for (int i = 0;i < KNOWN_SPEC_COUNT;i++) {
    if (strcmp(known_specs[i].name, command) == 0) {
      return &known_specs[i];
    }
  }

  // A command from FZF_COMMANDS that tarsh has no special rules for.
  fallback.name = command;
  fallback.first = PICK_ANY;
  fallback.rest = PICK_ANY;
  fallback.max_args = 1;
  fallback.multi = 1;
  return &fallback;
}

static const char *source_for(PickKind kind) {
  switch (kind) {
    case PICK_DIRS:
      // `..` and `~` first so going up or home is one keystroke away.
      return "{ printf '%s\\n' .. '~'; find . -mindepth 1 -maxdepth 8 " PRUNE
             " -type d -print 2>/dev/null | sed 's|^\\./||'; }";
    case PICK_FILES:
      return "find . -mindepth 1 -maxdepth 8 " PRUNE
             " \\( -type f -o -type l \\) -print 2>/dev/null | sed 's|^\\./||'";
    case PICK_ANY:
    default:
      return "find . -mindepth 1 -maxdepth 8 " PRUNE
             " -print 2>/dev/null | sed 's|^\\./||'";
  }
}

// Appends `text` to the malloc'd string *out.
static void append(char **out, size_t *length, const char *text) {
  size_t extra = strlen(text);
  char *grown = realloc(*out, *length + extra + 1);
  if (grown == NULL) {
    return;
  }
  memcpy(grown + *length, text, extra + 1);
  *out = grown;
  *length += extra;
}

static void append_quoted(char **out, size_t *length, const char *text) {
  char *quoted = shell_quote(text);
  if (quoted != NULL) {
    append(out, length, quoted);
    free(quoted);
  }
}

// Runs `source | fzf ...` and returns the picked lines joined by spaces,
// each quoted for the command line, or NULL if nothing was picked.
static char *run_fzf(const char *source, const char *label, const char *query,
                     int multi, int preview, int keep_order) {
  char *command = NULL;
  size_t length = 0;

  append(&command, &length, source);
  // fzf runs --preview through $SHELL; pin it to sh so a fish or zsh
  // SHELL can't break the preview command.
  append(&command, &length, " | SHELL=/bin/sh fzf --height=40% --reverse --border=rounded");
  append(&command, &length, " --prompt=");
  append_quoted(&command, &length, label);
  append(&command, &length, " --header=");
  append_quoted(&command, &length, multi ?
                "Enter: pick   Tab: select several   Esc: type it yourself" :
                "Enter: pick   Esc: type it yourself");
  if (query != NULL && query[0] != '\0') {
    append(&command, &length, " --query=");
    append_quoted(&command, &length, query);
  }
  if (multi) {
    append(&command, &length, " --multi");
  }
  if (keep_order) {
    append(&command, &length, " --no-sort --tiebreak=index");
  }
  if (preview) {
    append(&command, &length,
           " --preview-window=right,50%,border-left"
           " --preview='if [ -d {} ]; then ls -Ap --color=always {}; "
           "else head -n 100 {}; fi 2>/dev/null'");
  }
  if (command == NULL) {
    return NULL;
  }

  // fzf needs the terminal in its normal mode while it runs.
  lineedit_raw_off();
  fflush(stdout);
  FILE *picker = popen(command, "r");
  free(command);

  char *result = NULL;
  size_t result_length = 0;

  if (picker != NULL) {
    char *entry = NULL;
    size_t entry_size = 0;
    while (getline(&entry, &entry_size, picker) != -1) {
      entry[strcspn(entry, "\n")] = '\0';
      if (entry[0] == '\0') {
        continue;
      }
      if (result_length > 0) {
        append(&result, &result_length, " ");
      }
      // `~` must stay bare so it still expands to $HOME.
      if (strcmp(entry, "~") == 0) {
        append(&result, &result_length, "~");
      }
      else {
        append_quoted(&result, &result_length, entry);
      }
    }
    free(entry);
    pclose(picker);
  }

  lineedit_raw_on();
  return result;
}

static int is_option(const char *word) {
  return word[0] == '-' && word[1] != '\0';
}

int fzf_on_space(EditBuffer *line) {
  if (line->length == 0 || line->data[line->length - 1] == ' ') {
    return 0;
  }

  ArgList words;
  arglist_init(&words);
  if (parser_split_words(line->data, &words) <= 0) {
    // Still inside a quote, or nothing typed yet.
    arglist_free(&words);
    return 0;
  }

  // `sudo nvim ` should behave like `nvim `.
  int command_index = 0;
  if (strcmp(words.items[0], "sudo") == 0 && words.count > 1) {
    command_index = 1;
  }

  const PickerSpec *spec = spec_for(words.items[command_index]);
  int picked_args = 0;
  for (int i = command_index + 1;i < words.count;i++) {
    if (!is_option(words.items[i])) {
      picked_args++;
    }
  }
  char label[COMMAND_NAME_MAX + 4];
  snprintf(label, sizeof(label), "%s > ", words.items[command_index]);
  arglist_free(&words);

  if (spec == NULL || picked_args >= spec->max_args) {
    return 0;
  }
  if (!fzf_enabled()) {
    show_missing_hint();
    return 0;
  }

  // The space the user typed.
  editbuf_insert(line, " ");

  // cp/mv chain: pick the source, then straight on to the destination.
  while (picked_args < spec->max_args) {
    PickKind kind = (picked_args == 0) ? spec->first : spec->rest;
    int multi = spec->multi && picked_args == 0;
    char *choice = run_fzf(source_for(kind), label, NULL, multi, 1, kind == PICK_DIRS);
    if (choice == NULL) {
      break;
    }
    editbuf_insert(line, choice);
    free(choice);
    picked_args++;
    if (picked_args < spec->max_args) {
      editbuf_insert(line, " ");
    }
  }
  return 1;
}

// Writes every command name (builtins + $PATH) to `file`, one per line.
static void write_command_names(FILE *file) {
  builtin_write_names(file);

  const char *path = getenv("PATH");
  if (path == NULL) {
    return;
  }
  char *copy = strdup(path);
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
      if (entry->d_name[0] == '.') {
        continue;
      }
      char full[PATH_MAX];
      snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);
      if (access(full, X_OK) == 0) {
        fprintf(file, "%s\n", entry->d_name);
      }
    }
    closedir(listing);
  }
  free(copy);
}

// Creates a temp file for picker input; returns its path (malloc'd).
static char *temp_list(FILE **file_out) {
  const char *tmp = getenv("TMPDIR");
  if (tmp == NULL || tmp[0] == '\0') {
    tmp = "/tmp";
  }
  size_t size = strlen(tmp) + 32;
  char *path = malloc(size);
  if (path == NULL) {
    return NULL;
  }
  snprintf(path, size, "%s/tarsh-list-XXXXXX", tmp);
  int fd = mkstemp(path);
  if (fd < 0) {
    free(path);
    return NULL;
  }
  *file_out = fdopen(fd, "w");
  if (*file_out == NULL) {
    close(fd);
    unlink(path);
    free(path);
    return NULL;
  }
  return path;
}

int fzf_on_tab(EditBuffer *line) {
  if (!fzf_enabled()) {
    show_missing_hint();
    return 0;
  }

  // The word being completed runs from the last space to the cursor.
  size_t word_start = line->cursor;
  while (word_start > 0 && line->data[word_start - 1] != ' ') {
    word_start--;
  }
  size_t word_length = line->cursor - word_start;

  char query[PATH_MAX];
  snprintf(query, sizeof(query), "%.*s", (int)word_length, line->data + word_start);

  int is_command_word = 1;
  for (size_t i = 0;i < word_start;i++) {
    if (line->data[i] != ' ') {
      is_command_word = 0;
      break;
    }
  }

  char *choice = NULL;
  if (is_command_word) {
    FILE *list = NULL;
    char *list_path = temp_list(&list);
    if (list_path == NULL) {
      return 0;
    }
    write_command_names(list);
    fclose(list);

    char *quoted_path = shell_quote(list_path);
    char source[PATH_MAX + 32];
    snprintf(source, sizeof(source), "sort -u %s", quoted_path ? quoted_path : list_path);
    free(quoted_path);

    choice = run_fzf(source, "command > ", query, 0, 0, 0);
    unlink(list_path);
    free(list_path);
  }
  else {
    // Use the command's own picker kind when it has one.
    ArgList words;
    arglist_init(&words);
    PickKind kind = PICK_ANY;
    if (parser_split_words(line->data, &words) > 0) {
      const PickerSpec *spec = spec_for(words.items[0]);
      if (spec != NULL) {
        kind = spec->first;
      }
    }
    arglist_free(&words);
    choice = run_fzf(source_for(kind), "path > ", query, 1, 1, kind == PICK_DIRS);
  }

  if (choice == NULL) {
    return 1;
  }

  // Replace the partial word with the pick.
  editbuf_delete_before(line, word_length);
  editbuf_insert(line, choice);
  free(choice);
  return 1;
}

int fzf_on_history(EditBuffer *line) {
  if (!fzf_enabled()) {
    show_missing_hint();
    return 0;
  }

  FILE *list = NULL;
  char *list_path = temp_list(&list);
  if (list_path == NULL) {
    return 0;
  }
  // Newest first, each command once.
  int total = lineedit_history_count();
  for (int i = total - 1;i >= 0;i--) {
    const char *entry = lineedit_history_get(i);
    int seen = 0;
    for (int j = total - 1;j > i;j--) {
      if (strcmp(lineedit_history_get(j), entry) == 0) {
        seen = 1;
        break;
      }
    }
    if (!seen) {
      fprintf(list, "%s\n", entry);
    }
  }
  fclose(list);

  char *quoted_path = shell_quote(list_path);
  char source[PATH_MAX + 32];
  snprintf(source, sizeof(source), "cat %s", quoted_path ? quoted_path : list_path);
  free(quoted_path);

  // Read the raw line back ourselves: history entries must not be quoted.
  char *command = NULL;
  size_t length = 0;
  append(&command, &length, source);
  append(&command, &length,
         " | fzf --height=40% --reverse --border=rounded --no-sort --tiebreak=index"
         " --prompt='history > ' --header='Enter: use this command   Esc: cancel'");
  if (line->length > 0) {
    append(&command, &length, " --query=");
    append_quoted(&command, &length, line->data);
  }

  lineedit_raw_off();
  fflush(stdout);
  FILE *picker = command ? popen(command, "r") : NULL;
  free(command);

  char *entry = NULL;
  size_t entry_size = 0;
  if (picker != NULL) {
    if (getline(&entry, &entry_size, picker) != -1) {
      entry[strcspn(entry, "\n")] = '\0';
      line->length = 0;
      line->cursor = 0;
      line->data[0] = '\0';
      editbuf_insert(line, entry);
    }
    pclose(picker);
  }
  free(entry);
  lineedit_raw_on();

  unlink(list_path);
  free(list_path);
  return 1;
}
