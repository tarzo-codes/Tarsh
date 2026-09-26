#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

#include "util.h"

static int is_executable_file(const char *path) {
  struct stat info;
  return stat(path, &info) == 0 && S_ISREG(info.st_mode) && access(path, X_OK) == 0;
}

int path_lookup(const char *name, char *out, size_t out_size) {
  if (name == NULL || name[0] == '\0') {
    return 0;
  }

  if (strchr(name, '/') != NULL) {
    if (is_executable_file(name)) {
      snprintf(out, out_size, "%s", name);
      return 1;
    }
    return 0;
  }

  const char *path = getenv("PATH");
  if (path == NULL) {
    path = "/usr/local/bin:/usr/bin:/bin";
  }

  const char *start = path;
  while (1) {
    const char *end = strchr(start, ':');
    size_t length = (end == NULL) ? strlen(start) : (size_t)(end - start);

    char candidate[PATH_MAX];
    // An empty PATH entry means the current directory.
    if (length == 0) {
      snprintf(candidate, sizeof(candidate), "./%s", name);
    }
    else {
      snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)length, start, name);
    }

    if (is_executable_file(candidate)) {
      snprintf(out, out_size, "%s", candidate);
      return 1;
    }

    if (end == NULL) {
      break;
    }
    start = end + 1;
  }
  return 0;
}

static int is_plain_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         strchr("._/+-@%:,=", c) != NULL;
}

char *shell_quote(const char *text) {
  int plain = text[0] != '\0';
  for (const char *c = text;*c != '\0';c++) {
    // Bytes >= 0x80 are UTF-8; they are safe unquoted too.
    if (!is_plain_char(*c) && (unsigned char)*c < 0x80) {
      plain = 0;
      break;
    }
  }
  if (plain) {
    return strdup(text);
  }

  // Wrap in single quotes; an embedded ' becomes '\'' (close, escaped
  // quote, reopen).
  size_t length = 2;
  for (const char *c = text;*c != '\0';c++) {
    length += (*c == '\'') ? 4 : 1;
  }

  char *quoted = malloc(length + 1);
  if (quoted == NULL) {
    return NULL;
  }

  char *out = quoted;
  *out++ = '\'';
  for (const char *c = text;*c != '\0';c++) {
    if (*c == '\'') {
      memcpy(out, "'\\''", 4);
      out += 4;
    }
    else {
      *out++ = *c;
    }
  }
  *out++ = '\'';
  *out = '\0';
  return quoted;
}
