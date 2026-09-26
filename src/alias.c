#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alias.h"
#include "syntax.h"
#include "util.h"

#define MAX_ALIAS_DEPTH 16

typedef struct {
  char *name;
  char *value;
}Alias;

static Alias *aliases = NULL;
static int alias_count = 0;
static int alias_capacity = 0;

static int find_alias(const char *name, size_t length) {
  for (int i = 0;i < alias_count;i++) {
    if (strlen(aliases[i].name) == length && strncmp(aliases[i].name, name, length) == 0) {
      return i;
    }
  }
  return -1;
}

static int valid_name(const char *name) {
  if (name[0] == '\0') {
    return 0;
  }
  for (const char *c = name;*c != '\0';c++) {
    if (strchr(" \t\n'\"\\$`=/|&;<>(){}", *c) != NULL) {
      return 0;
    }
  }
  return 1;
}

int alias_set(const char *name, const char *value) {
  if (!valid_name(name)) {
    return 1;
  }
  int index = find_alias(name, strlen(name));
  if (index >= 0) {
    char *copy = strdup(value);
    if (copy == NULL) {
      return 1;
    }
    free(aliases[index].value);
    aliases[index].value = copy;
    return 0;
  }

  if (alias_count == alias_capacity) {
    int capacity = alias_capacity ? alias_capacity * 2 : 16;
    Alias *grown = realloc(aliases, capacity * sizeof(Alias));
    if (grown == NULL) {
      return 1;
    }
    aliases = grown;
    alias_capacity = capacity;
  }
  aliases[alias_count].name = strdup(name);
  aliases[alias_count].value = strdup(value);
  alias_count++;
  return 0;
}

int alias_unset(const char *name) {
  int index = find_alias(name, strlen(name));
  if (index < 0) {
    return 1;
  }
  free(aliases[index].name);
  free(aliases[index].value);
  aliases[index] = aliases[alias_count - 1];
  alias_count--;
  return 0;
}

const char *alias_get(const char *name) {
  int index = find_alias(name, strlen(name));
  return index >= 0 ? aliases[index].value : NULL;
}

static void print_one(const Alias *alias) {
  char *quoted = shell_quote(alias->value);
  printf("alias %s=%s\n", alias->name, quoted ? quoted : alias->value);
  free(quoted);
}

int alias_print(const char *name) {
  if (name == NULL) {
    for (int i = 0;i < alias_count;i++) {
      print_one(&aliases[i]);
    }
    return 0;
  }
  int index = find_alias(name, strlen(name));
  if (index < 0) {
    return 1;
  }
  print_one(&aliases[index]);
  return 0;
}

typedef struct {
  char *text;
  size_t length;
  size_t capacity;
}Out;

static void out_add(Out *out, const char *text, size_t length) {
  if (out->length + length + 1 > out->capacity) {
    size_t capacity = out->capacity ? out->capacity : 128;
    while (out->length + length + 1 > capacity) {
      capacity *= 2;
    }
    char *grown = realloc(out->text, capacity);
    if (grown == NULL) {
      perror("tarsh: out of memory");
      exit(EXIT_FAILURE);
    }
    out->text = grown;
    out->capacity = capacity;
  }
  memcpy(out->text + out->length, text, length);
  out->length += length;
  out->text[out->length] = '\0';
}

static int plain_word(const char *word, size_t length) {
  for (size_t i = 0;i < length;i++) {
    if (strchr("'\"\\$`", word[i]) != NULL) {
      return 0;
    }
  }
  return length > 0;
}

static int leads_command(const char *word, size_t length) {
  static const char *const words[] = {
    "do", "then", "else", "elif", "if", "while", "until", "!", "{", "time", NULL
  };
  for (int i = 0;words[i] != NULL;i++) {
    if (strlen(words[i]) == length && strncmp(words[i], word, length) == 0) {
      return 1;
    }
  }
  return 0;
}

// Resolves `name` through chained aliases (ll -> ls -lh -> ls --color -lh)
// without expanding any alias twice.
static char *resolve(const char *name, size_t length) {
  int index = find_alias(name, length);
  if (index < 0) {
    return NULL;
  }
  char *value = strdup(aliases[index].value);
  int used[MAX_ALIAS_DEPTH];
  int used_count = 0;
  used[used_count++] = index;

  while (value != NULL && used_count < MAX_ALIAS_DEPTH) {
    size_t start = 0;
    while (value[start] == ' ' || value[start] == '\t') start++;
    int unterminated = 0;
    size_t end = syntax_word_end(value, start, &unterminated);
    int next = find_alias(value + start, end - start);
    int seen = 0;
    for (int i = 0;i < used_count;i++) {
      if (used[i] == next) seen = 1;
    }
    if (next < 0 || seen) {
      break;
    }
    used[used_count++] = next;
    size_t replacement = strlen(aliases[next].value);
    size_t rest = strlen(value + end);
    char *combined = malloc(start + replacement + rest + 1);
    if (combined == NULL) {
      break;
    }
    memcpy(combined, value, start);
    memcpy(combined + start, aliases[next].value, replacement);
    memcpy(combined + start + replacement, value + end, rest + 1);
    free(value);
    value = combined;
  }
  return value;
}

char *alias_expand_line(const char *line) {
  Out out = { NULL, 0, 0 };
  out_add(&out, "", 0);

  if (alias_count == 0) {
    out_add(&out, line, strlen(line));
    return out.text;
  }

  int command_start = 1;
  size_t i = 0;
  while (line[i] != '\0') {
    char c = line[i];
    if (c == ' ' || c == '\t') {
      out_add(&out, &line[i], 1);
      i++;
      continue;
    }
    if (c == '\n' || c == ';' || c == '|' || c == '&' || c == '(') {
      // && and || are two characters; both lead to a new command.
      size_t length = ((c == '&' || c == '|') && line[i + 1] == c) ? 2 : 1;
      // &> is a redirection, not a separator.
      if (c == '&' && line[i + 1] == '>') {
        out_add(&out, &line[i], 2);
        i += 2;
        continue;
      }
      out_add(&out, &line[i], length);
      i += length;
      command_start = 1;
      continue;
    }
    if (c == '<' || c == '>' || c == '#') {
      if (c == '#') {
        // Comment: copy the rest of the line untouched.
        size_t end = strcspn(line + i, "\n");
        out_add(&out, line + i, end);
        i += end;
        continue;
      }
      out_add(&out, &line[i], 1);
      i++;
      continue;
    }

    int unterminated = 0;
    size_t end = syntax_word_end(line, i, &unterminated);
    if (end == i) {
      out_add(&out, &line[i], 1);
      i++;
      continue;
    }
    const char *word = line + i;
    size_t length = end - i;

    char *replacement = NULL;
    if (command_start && plain_word(word, length)) {
      replacement = resolve(word, length);
    }
    if (replacement != NULL) {
      out_add(&out, replacement, strlen(replacement));
      free(replacement);
      command_start = 0;
    }
    else {
      out_add(&out, word, length);
      command_start = leads_command(word, length);
    }
    i = end;
  }
  return out.text;
}
