#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <glob.h>
#include <sys/wait.h>

#include "expand.h"
#include "exec.h"
#include "jobs.h"
#include "lang.h"
#include "syntax.h"

// Every character of a word being built carries flags saying where it
// came from, so splitting and globbing only touch what they should.
#define FROM_QUOTES 1     // quoted or escaped: never split, never a glob
#define FROM_EXPANSION 2  // unquoted $VAR / $(...) result: split on spaces

typedef struct {
  char *text;
  unsigned char *flags;
  size_t length;
  size_t capacity;
}Field;

static int last_status = 0;
static pid_t last_background = 0;
static pid_t shell_pid = 0;
static char **positional = NULL;
static int positional_count = 0;

void expand_init(void) {
  shell_pid = getpid();
}

void expand_set_last_status(int status) {
  last_status = status;
}

int expand_last_status(void) {
  return last_status;
}

void expand_set_last_background(pid_t pid) {
  last_background = pid;
}

void expand_set_positional(int count, char **args) {
  for (int i = 0;i < positional_count;i++) {
    free(positional[i]);
  }
  free(positional);
  positional = calloc(count > 0 ? count : 1, sizeof(char *));
  positional_count = 0;
  if (positional == NULL) {
    return;
  }
  for (int i = 0;i < count;i++) {
    positional[i] = strdup(args[i]);
  }
  positional_count = count;
}

int expand_shift(int n) {
  if (n < 0 || n > positional_count - 1) {
    return 1;
  }
  for (int i = 1;i <= n;i++) {
    free(positional[i]);
  }
  memmove(positional + 1, positional + 1 + n, (positional_count - 1 - n) * sizeof(char *));
  positional_count -= n;
  return 0;
}

static void field_add(Field *field, const char *text, size_t length, unsigned char flag) {
  if (field->length + length + 1 > field->capacity) {
    size_t capacity = field->capacity ? field->capacity : 64;
    while (field->length + length + 1 > capacity) {
      capacity *= 2;
    }
    char *text_grown = realloc(field->text, capacity);
    unsigned char *flags_grown = realloc(field->flags, capacity);
    if (text_grown == NULL || flags_grown == NULL) {
      perror("tarsh: out of memory");
      exit(EXIT_FAILURE);
    }
    field->text = text_grown;
    field->flags = flags_grown;
    field->capacity = capacity;
  }
  memcpy(field->text + field->length, text, length);
  memset(field->flags + field->length, flag, length);
  field->length += length;
  field->text[field->length] = '\0';
}

static void field_add_string(Field *field, const char *text, unsigned char flag) {
  if (text != NULL) {
    field_add(field, text, strlen(text), flag);
  }
}

char *expand_capture(const char *command) {
  int fds[2];
  if (pipe(fds) != 0) {
    perror("tarsh: pipe");
    return strdup("");
  }

  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) {
    perror("tarsh: fork");
    close(fds[0]);
    close(fds[1]);
    return strdup("");
  }

  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);
    jobs_become_subshell();
    int status = exec_run_line(command);
    fflush(stdout);
    _exit(status);
  }

  close(fds[1]);
  char *output = NULL;
  size_t length = 0;
  char chunk[4096];
  ssize_t got;
  while ((got = read(fds[0], chunk, sizeof(chunk))) != 0) {
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    char *grown = realloc(output, length + got + 1);
    if (grown == NULL) {
      break;
    }
    output = grown;
    memcpy(output + length, chunk, got);
    length += got;
    output[length] = '\0';
  }
  close(fds[0]);

  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }

  if (output == NULL) {
    return strdup("");
  }
  while (length > 0 && output[length - 1] == '\n') {
    output[--length] = '\0';
  }
  return output;
}

static int is_name_char(char c, int first) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
         (!first && c >= '0' && c <= '9');
}

static void add_positional_joined(Field *field, unsigned char flag) {
  for (int i = 1;i < positional_count;i++) {
    if (i > 1) {
      field_add(field, " ", 1, flag);
    }
    field_add_string(field, positional[i], flag);
  }
}

static const char *lookup_variable(const char *name) {
  const char *value = getenv(name);
  // fish users expect $status; give them tarsh's last status.
  if (value == NULL && strcmp(name, "status") == 0 && strcmp(lang_current_name(), "fish") == 0) {
    static char digits[16];
    snprintf(digits, sizeof(digits), "%d", last_status);
    return digits;
  }
  return value;
}

// Expands the $... or `...` at raw[i]; returns the index after it.
static size_t expand_dollar(const char *raw, size_t i, Field *field, unsigned char flag) {
  char buffer[32];

  if (raw[i] == '`') {
    size_t end = i + 1;
    while (raw[end] != '\0' && raw[end] != '`') {
      if (raw[end] == '\\' && raw[end + 1] != '\0') {
        end++;
      }
      end++;
    }
    char *inner = strndup(raw + i + 1, end - i - 1);
    char *output = expand_capture(inner);
    field_add_string(field, output, flag);
    free(inner);
    free(output);
    return raw[end] == '`' ? end + 1 : end;
  }

  char next = raw[i + 1];

  if (next == '(') {
    int unterminated = 0;
    size_t end = syntax_subst_end(raw, i, &unterminated);
    size_t inner_length = (end >= i + 3) ? end - i - 3 : 0;
    char *inner = strndup(raw + i + 2, inner_length);
    char *output = expand_capture(inner);
    field_add_string(field, output, flag);
    free(inner);
    free(output);
    return end;
  }

  if (next == '{') {
    const char *close = strchr(raw + i + 2, '}');
    if (close == NULL) {
      field_add(field, "$", 1, flag);
      return i + 1;
    }
    char *inside = strndup(raw + i + 2, close - (raw + i + 2));
    char *fallback = strstr(inside, ":-");
    if (fallback != NULL) {
      *fallback = '\0';
      fallback += 2;
    }
    const char *value = lookup_variable(inside);
    if (value == NULL || value[0] == '\0') {
      value = fallback;
    }
    field_add_string(field, value, flag);
    free(inside);
    return (close - raw) + 1;
  }

  if (next == '?') {
    snprintf(buffer, sizeof(buffer), "%d", last_status);
    field_add_string(field, buffer, flag);
    return i + 2;
  }
  if (next == '$') {
    snprintf(buffer, sizeof(buffer), "%d", (int)shell_pid);
    field_add_string(field, buffer, flag);
    return i + 2;
  }
  if (next == '!') {
    if (last_background > 0) {
      snprintf(buffer, sizeof(buffer), "%d", (int)last_background);
      field_add_string(field, buffer, flag);
    }
    return i + 2;
  }
  if (next == '#') {
    snprintf(buffer, sizeof(buffer), "%d", positional_count > 0 ? positional_count - 1 : 0);
    field_add_string(field, buffer, flag);
    return i + 2;
  }
  if (next == '@' || next == '*') {
    add_positional_joined(field, flag);
    return i + 2;
  }
  if (next >= '0' && next <= '9') {
    int index = next - '0';
    if (index < positional_count) {
      field_add_string(field, positional[index], flag);
    }
    else if (index == 0) {
      field_add_string(field, "tarsh", flag);
    }
    return i + 2;
  }

  size_t length = 0;
  while (is_name_char(raw[i + 1 + length], length == 0)) {
    length++;
  }
  if (length == 0) {
    field_add(field, "$", 1, flag);
    return i + 1;
  }
  char *name = strndup(raw + i + 1, length);
  field_add_string(field, lookup_variable(name), flag);
  free(name);
  return i + 1 + length;
}

// Builds the flagged characters for one raw word. Returns 1 if the word
// contained any quotes (so an empty result still counts as an argument).
static int build_field(const char *raw, Field *field) {
  int had_quotes = 0;
  size_t i = 0;

  if (raw[0] == '~' && (raw[1] == '\0' || raw[1] == '/')) {
    const char *home = getenv("HOME");
    if (home != NULL) {
      field_add_string(field, home, FROM_QUOTES);
      i = 1;
    }
  }

  char quote = '\0';
  while (raw[i] != '\0') {
    char c = raw[i];

    if (quote == '\0') {
      if (c == '\'') {
        size_t end = i + 1;
        while (raw[end] != '\0' && raw[end] != '\'') end++;
        field_add(field, raw + i + 1, end - i - 1, FROM_QUOTES);
        had_quotes = 1;
        i = raw[end] ? end + 1 : end;
      }
      else if (c == '"') {
        quote = '"';
        had_quotes = 1;
        i++;
      }
      else if (c == '\\') {
        if (raw[i + 1] != '\0') {
          field_add(field, raw + i + 1, 1, FROM_QUOTES);
          i += 2;
        }
        else {
          field_add(field, "\\", 1, FROM_QUOTES);
          i++;
        }
      }
      else if (c == '$' || c == '`') {
        i = expand_dollar(raw, i, field, FROM_EXPANSION);
      }
      else {
        field_add(field, &raw[i], 1, 0);
        i++;
      }
    }
    else {
      if (c == '"') {
        quote = '\0';
        i++;
      }
      else if (c == '\\' && raw[i + 1] != '\0' && strchr("$`\"\\", raw[i + 1]) != NULL) {
        field_add(field, raw + i + 1, 1, FROM_QUOTES);
        i += 2;
      }
      else if (c == '$' || c == '`') {
        i = expand_dollar(raw, i, field, FROM_QUOTES);
      }
      else {
        field_add(field, &raw[i], 1, FROM_QUOTES);
        i++;
      }
    }
  }
  return had_quotes;
}

static int has_glob(const Field *field, size_t start, size_t end) {
  for (size_t i = start;i < end;i++) {
    if (!(field->flags[i] & FROM_QUOTES) && strchr("*?[", field->text[i]) != NULL) {
      return 1;
    }
  }
  return 0;
}

// Expands the glob in field[start, end). Appends matches to `out` and
// returns how many there were.
static int glob_into(const Field *field, size_t start, size_t end, ArgList *out, int limit_one) {
  // Quoted special characters must reach glob() escaped.
  char *pattern = malloc((end - start) * 2 + 1);
  if (pattern == NULL) {
    return 0;
  }
  size_t used = 0;
  for (size_t i = start;i < end;i++) {
    char c = field->text[i];
    if ((field->flags[i] & FROM_QUOTES) && strchr("*?[]\\", c) != NULL) {
      pattern[used++] = '\\';
    }
    pattern[used++] = c;
  }
  pattern[used] = '\0';

  glob_t matches;
  int found = 0;
  if (glob(pattern, 0, NULL, &matches) == 0) {
    if (!limit_one || matches.gl_pathc == 1) {
      for (size_t i = 0;i < matches.gl_pathc;i++) {
        arglist_append(out, matches.gl_pathv[i]);
      }
      found = (int)matches.gl_pathc;
    }
    globfree(&matches);
  }
  free(pattern);
  return found;
}

static void push_piece(const Field *field, size_t start, size_t end, ArgList *out) {
  if (has_glob(field, start, end) && glob_into(field, start, end, out, 0) > 0) {
    return;
  }
  char *piece = strndup(field->text + start, end - start);
  arglist_append(out, piece);
  free(piece);
}

void expand_word(const char *raw, ArgList *out) {
  // "$@" keeps every positional argument as its own word.
  if (strcmp(raw, "\"$@\"") == 0) {
    for (int i = 1;i < positional_count;i++) {
      arglist_append(out, positional[i]);
    }
    return;
  }

  Field field = { NULL, NULL, 0, 0 };
  field_add(&field, "", 0, 0);
  int had_quotes = build_field(raw, &field);

  if (field.length == 0) {
    if (had_quotes) {
      arglist_append(out, "");
    }
    free(field.text);
    free(field.flags);
    return;
  }

  // Split at whitespace that came from an unquoted expansion.
  size_t start = 0;
  int pieces = 0;
  for (size_t i = 0;i <= field.length;i++) {
    int boundary = (i == field.length) ||
                   ((field.flags[i] & FROM_EXPANSION) &&
                    (field.text[i] == ' ' || field.text[i] == '\t' || field.text[i] == '\n'));
    if (!boundary) {
      continue;
    }
    if (i > start) {
      push_piece(&field, start, i, out);
      pieces++;
    }
    start = i + 1;
  }
  if (pieces == 0 && had_quotes) {
    arglist_append(out, "");
  }

  free(field.text);
  free(field.flags);
}

char *expand_single(const char *raw) {
  Field field = { NULL, NULL, 0, 0 };
  field_add(&field, "", 0, 0);
  build_field(raw, &field);

  char *result = NULL;
  if (has_glob(&field, 0, field.length)) {
    ArgList matches;
    arglist_init(&matches);
    if (glob_into(&field, 0, field.length, &matches, 1) == 1) {
      result = strdup(matches.items[0]);
    }
    arglist_free(&matches);
  }
  if (result == NULL) {
    result = strdup(field.text);
  }
  free(field.text);
  free(field.flags);
  return result;
}
