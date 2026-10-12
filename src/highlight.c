#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "highlight.h"
#include "alias.h"
#include "exec.h"
#include "fish.h"
#include "script.h"
#include "syntax.h"

#define C_RESET    "\x1b[0m"
#define C_COMMAND  "\x1b[1;32m"
#define C_UNKNOWN  "\x1b[1;31m"
#define C_KEYWORD  "\x1b[35m"
#define C_OPTION   "\x1b[36m"
#define C_STRING   "\x1b[33m"
#define C_VARIABLE "\x1b[96m"
#define C_OPERATOR "\x1b[1;34m"
#define C_COMMENT  "\x1b[90m"
#define C_NUMBER   "\x1b[36m"
#define C_PATH     "\x1b[4m"

typedef struct {
  char *text;
  size_t length, capacity;
}Out;

static void add(Out *out, const char *text, size_t length) {
  if (out->length + length + 1 > out->capacity) {
    size_t capacity = out->capacity ? out->capacity : 256;
    while (out->length + length + 1 > capacity) capacity *= 2;
    char *grown = realloc(out->text, capacity);
    if (grown == NULL) return;
    out->text = grown;
    out->capacity = capacity;
  }
  memcpy(out->text + out->length, text, length);
  out->length += length;
  out->text[out->length] = '\0';
}

static void adds(Out *out, const char *text) { add(out, text, strlen(text)); }

static void colored(Out *out, const char *color, const char *text, size_t length) {
  adds(out, color);
  add(out, text, length);
  adds(out, C_RESET);
}

// Caches the last command looked up: highlighting runs on every key.
static int command_known(const char *name) {
  static char last[256];
  static int last_result;
  if (strcmp(last, name) == 0) return last_result;
  snprintf(last, sizeof(last), "%s", name);
  struct stat info;
  last_result = exec_command_exists(name) || alias_get(name) != NULL || abbr_get(name) != NULL ||
                script_is_keyword(name) ||
                ((strchr(name, '/') || !strcmp(name, "..")) && stat(name, &info) == 0 && S_ISDIR(info.st_mode));
  return last_result;
}

static int is_plain(const char *word, size_t length) {
  for (size_t i = 0;i < length;i++) {
    if (strchr("'\"$`\\", word[i])) return 0;
  }
  return 1;
}

// Colors an argument: quotes, $variables, and existing paths.
static void color_argument(Out *out, const char *word, size_t length) {
  if (word[0] == '-' && length > 1) {
    colored(out, C_OPTION, word, length);
    return;
  }
  if (is_plain(word, length) && length < 4096) {
    char path[4096];
    memcpy(path, word, length);
    path[length] = '\0';
    struct stat info;
    if (stat(path, &info) == 0) {
      colored(out, C_PATH, word, length);
      return;
    }
  }
  size_t i = 0;
  while (i < length) {
    char c = word[i];
    if (c == '\'' || c == '"') {
      size_t end = i + 1;
      while (end < length && word[end] != c) {
        if (c == '"' && word[end] == '\\') end++;
        end++;
      }
      if (end < length) end++;
      colored(out, C_STRING, word + i, end - i);
      i = end;
    }
    else if (c == '$' && i + 1 < length) {
      size_t end = i + 1;
      if (word[end] == '(' ) {
        int unterminated = 0;
        end = syntax_subst_end(word, i, &unterminated);
        if (end > length) end = length;
      }
      else if (word[end] == '{') {
        while (end < length && word[end] != '}') end++;
        if (end < length) end++;
      }
      else {
        while (end < length && (word[end] == '_' || word[end] == '?' ||
               (word[end] >= 'a' && word[end] <= 'z') || (word[end] >= 'A' && word[end] <= 'Z') ||
               (word[end] >= '0' && word[end] <= '9'))) end++;
      }
      colored(out, C_VARIABLE, word + i, end - i);
      i = end;
    }
    else {
      size_t end = i + 1;   // always move forward (a lone trailing $ lands here)
      while (end < length && word[end] != '\'' && word[end] != '"' && word[end] != '$') end++;
      add(out, word + i, end - i);
      i = end;
    }
  }
}

static int word_equals(const char *word, size_t length, const char *text) {
  return strlen(text) == length && strncmp(word, text, length) == 0;
}

// After these, the next word is still a command.
static int leads_to_command(const char *word, size_t length) {
  static const char *words[] = {
    "sudo", "env", "exec", "nohup", "time", "command", "builtin", "then", "do", "else",
    "if", "while", "until", "!", "{", "and", "or", "not", "begin", NULL
  };
  for (int i = 0;words[i];i++) if (word_equals(word, length, words[i])) return 1;
  return 0;
}

static void highlight_shell(const char *text, Out *out) {
  int command_position = 1;
  size_t i = 0;
  while (text[i]) {
    char c = text[i];
    if (c == ' ' || c == '\t') { add(out, &text[i], 1); i++; continue; }
    if (c == '\n') { add(out, &text[i], 1); i++; command_position = 1; continue; }
    if (c == '#' && (i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t')) {
      size_t end = i + strcspn(text + i, "\n");
      colored(out, C_COMMENT, text + i, end - i);
      i = end;
      continue;
    }
    if (c == '|' || c == ';' || (c == '&' && text[i + 1] != '>')) {
      size_t end = i;
      while (text[end] == '|' || text[end] == ';' || text[end] == '&') end++;
      colored(out, C_OPERATOR, text + i, end - i);
      i = end;
      command_position = 1;
      continue;
    }
    if (c == '<' || c == '>' || c == '&') {
      size_t end = i;
      while (text[end] == '<' || text[end] == '>' || text[end] == '&') end++;
      colored(out, C_OPERATOR, text + i, end - i);
      i = end;
      continue;
    }
    if (c == '(' || c == ')') {
      colored(out, C_OPERATOR, text + i, 1);
      command_position = (c == '(');
      i++;
      continue;
    }

    int unterminated = 0;
    size_t end = syntax_word_end(text, i, &unterminated);
    if (end == i) { add(out, &text[i], 1); i++; continue; }
    const char *word = text + i;
    size_t length = end - i;

    if (command_position) {
      const char *equals = memchr(word, '=', length);
      if (equals != NULL && equals != word && is_plain(word, equals - word)) {
        // NAME=value before a command
        colored(out, C_VARIABLE, word, equals - word + 1);
        color_argument(out, equals + 1, length - (equals - word) - 1);
      }
      else if (is_plain(word, length) && length < 256) {
        char name[256];
        memcpy(name, word, length);
        name[length] = '\0';
        const char *color = script_is_keyword(name) || leads_to_command(word, length) ? C_KEYWORD :
                            command_known(name) ? C_COMMAND : C_UNKNOWN;
        colored(out, color, word, length);
        command_position = leads_to_command(word, length);
      }
      else {
        color_argument(out, word, length);
        command_position = 0;
      }
    }
    else {
      color_argument(out, word, length);
    }
    i = end;
  }
}

// tarsh script: keywords, strings, numbers, known functions.
static void highlight_script(const char *text, Out *out) {
  size_t i = 0;
  while (text[i]) {
    char c = text[i];
    if (c == '#') {
      size_t end = i + strcspn(text + i, "\n");
      colored(out, C_COMMENT, text + i, end - i);
      i = end;
      continue;
    }
    if (c == '"' || c == '\'' || ((c == 'f' || c == 'F') && (text[i + 1] == '"' || text[i + 1] == '\''))) {
      size_t start = i;
      if (c == 'f' || c == 'F') i++;
      char quote = text[i];
      size_t end = i + 1;
      while (text[end] && text[end] != quote) { if (text[end] == '\\' && text[end + 1]) end++; end++; }
      if (text[end]) end++;
      colored(out, C_STRING, text + start, end - start);
      i = end;
      continue;
    }
    if ((c == '$' || c == '?') && text[i + 1] == '(') {
      int unterminated = 0;
      size_t end = syntax_subst_end(text, i, &unterminated);
      colored(out, C_VARIABLE, text + i, end - i);
      i = end;
      continue;
    }
    if (c >= '0' && c <= '9') {
      size_t end = i;
      while ((text[end] >= '0' && text[end] <= '9') || text[end] == '.' || text[end] == '_') end++;
      colored(out, C_NUMBER, text + i, end - i);
      i = end;
      continue;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$') {
      size_t end = i + (c == '$');
      while ((text[end] >= 'a' && text[end] <= 'z') || (text[end] >= 'A' && text[end] <= 'Z') ||
             (text[end] >= '0' && text[end] <= '9') || text[end] == '_') end++;
      char name[128];
      size_t length = end - i;
      snprintf(name, sizeof(name), "%.*s", (int)(length < 127 ? length : 127), text + i);
      size_t after = end;
      while (text[after] == ' ') after++;
      if (c == '$') colored(out, C_VARIABLE, text + i, length);
      else if (script_is_keyword(name) || !strcmp(name, "True") || !strcmp(name, "False") || !strcmp(name, "None") ||
               !strcmp(name, "true") || !strcmp(name, "false") || !strcmp(name, "none")) colored(out, C_KEYWORD, text + i, length);
      else if (text[after] == '(' && (script_builtin_exists(name) || script_function_exists(name))) colored(out, C_COMMAND, text + i, length);
      else if (text[after] == '(') colored(out, C_UNKNOWN, text + i, length);
      else add(out, text + i, length);
      i = end;
      continue;
    }
    add(out, &text[i], 1);
    i++;
  }
}

char *highlight_line(const char *text) {
  Out out = { NULL, 0, 0 };
  adds(&out, "");
  if (script_claims(text)) {
    highlight_script(text, &out);
  }
  else {
    highlight_shell(text, &out);
  }
  return out.text;
}
