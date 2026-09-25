#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include "config.h"
#include "prompts.h"
#include "fzf.h"
#include "lang.h"

static char config_dir[PATH_MAX];

// Trims trailing characters from `chars` off `text` in place.
static void trim_trailing(char *text, const char *chars) {
  size_t length = strlen(text);
  while (length > 0 && strchr(chars, text[length - 1]) != NULL) {
    text[length - 1] = '\0';
    length--;
  }
}

const char *config_directory(void) {
  if (config_dir[0] != '\0') {
    return config_dir;
  }

  char *home = getenv("HOME");
  if (home == NULL) {
    home = ".";
  }

  // mkdir() does not build parents, so create $HOME/.config first.
  char parent[PATH_MAX];
  snprintf(parent, sizeof(parent), "%s/.config", home);
  mkdir(parent, 0700);

  snprintf(config_dir, sizeof(config_dir), "%s/.config/tarsh", home);
  mkdir(config_dir, 0700);
  return config_dir;
}

static void write_default_config(const char *config_path) {
  FILE *file = fopen(config_path, "w");
  if (file == NULL) {
    return;
  }
  fprintf(file, "# Tarsh Configuration File\n");
  fprintf(file, "# Modify these values to customize, then restart tarsh.\n\n");
  prompt_write_defaults(file);
  fprintf(file, "\n");
  fzf_write_defaults(file);
  fprintf(file, "\n");
  lang_write_defaults(file);
  fclose(file);
}

static void apply_config_line(char *line) {
  // Only the newline goes: a trailing space in FORMAT is deliberate.
  trim_trailing(line, "\r\n");

  // Skip blank lines and comments.
  char *cursor = line;
  while (*cursor == ' ' || *cursor == '\t') {
    cursor++;
  }
  if (*cursor == '\0' || *cursor == '#') {
    return;
  }

  char *separator = strchr(cursor, '=');
  if (separator == NULL) {
    return;
  }
  *separator = '\0';
  char *key = cursor;
  char *value = separator + 1;
  trim_trailing(key, " \t");

  if (prompt_configure(key, value)) {
    return;
  }
  if (fzf_configure(key, value)) {
    return;
  }
  if (lang_configure(key, value)) {
    return;
  }
  fprintf(stderr, "tarsh: config: unknown setting '%s' (ignored)\n", key);
}

int config_load(void) {
  char config_path[PATH_MAX + 16];
  snprintf(config_path, sizeof(config_path), "%s/config.t", config_directory());

  FILE *file = fopen(config_path, "r");
  if (file == NULL) {
    write_default_config(config_path);
    return 1;
  }

  char line[512];
  while (fgets(line, sizeof(line), file) != NULL) {
    apply_config_line(line);
  }
  fclose(file);
  return 0;
}
