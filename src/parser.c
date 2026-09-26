#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"

#define INITIAL_SLOTS 16

static void fatal_oom(void) {
  perror("Fatal: Shell ran out of heap memory during parsing");
  exit(EXIT_FAILURE);
}

void arglist_init(ArgList *args) {
  args->capacity = INITIAL_SLOTS;
  args->count = 0;
  args->items = malloc(args->capacity * sizeof(char *));
  if (args->items == NULL) {
    fatal_oom();
  }
  args->items[0] = NULL;
}

// Frees the tokens but keeps the vector itself, so one ArgList can be
// reused for every line the shell reads.
void arglist_reset(ArgList *args) {
  for (int i = 0;i < args->count;i++) {
    free(args->items[i]);
  }
  args->count = 0;
  if (args->items != NULL) {
    args->items[0] = NULL;
  }
}

void arglist_free(ArgList *args) {
  arglist_reset(args);
  free(args->items);
  args->items = NULL;
  args->capacity = 0;
}

// Appends one token, taking ownership of `token`.
static void arglist_push(ArgList *args, char *token) {
  // Keep one slot spare for the NULL sentinel execvp() expects.
  if (args->count + 1 >= args->capacity) {
    int new_capacity = args->capacity * 2;
    char **grown = realloc(args->items, new_capacity * sizeof(char *));
    if (grown == NULL) {
      free(token);
      fatal_oom();
    }
    args->items = grown;
    args->capacity = new_capacity;
  }
  args->items[args->count] = token;
  args->count++;
  args->items[args->count] = NULL;
}

static int is_token_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// A small growable string for building one token at a time.
typedef struct {
  char *data;
  size_t length;
  size_t capacity;
}Builder;

static void builder_push(Builder *builder, const char *text, size_t length) {
  if (builder->length + length + 1 > builder->capacity) {
    size_t new_capacity = builder->capacity * 2;
    while (builder->length + length + 1 > new_capacity) {
      new_capacity *= 2;
    }
    char *grown = realloc(builder->data, new_capacity);
    if (grown == NULL) {
      fatal_oom();
    }
    builder->data = grown;
    builder->capacity = new_capacity;
  }
  memcpy(builder->data + builder->length, text, length);
  builder->length += length;
  builder->data[builder->length] = '\0';
}

static void builder_char(Builder *builder, char c) {
  builder_push(builder, &c, 1);
}

static int tokenize(const char *buffer, ArgList *args) {
  arglist_reset(args);

  Builder token = { NULL, 0, 64 };
  token.data = malloc(token.capacity);
  if (token.data == NULL) {
    fatal_oom();
  }

  size_t length = strlen(buffer);
  size_t i = 0;
  while (i < length) {
    while (i < length && is_token_space(buffer[i])) {
      i++;
    }
    if (i >= length) {
      break;
    }

    token.length = 0;
    token.data[0] = '\0';
    char quote = '\0';
    int saw_quote = 0;

    while (i < length) {
      char c = buffer[i];

      if (quote == '\0' && is_token_space(c)) {
        break;
      }
      else if (quote == '\0' && (c == '\'' || c == '"')) {
        quote = c;
        saw_quote = 1;
        i++;
      }
      else if (quote != '\0' && c == quote) {
        quote = '\0';
        i++;
      }
      else if (c == '\\' && quote != '\'' && i + 1 < length) {
        // Inside double quotes only a few characters are escapable;
        // outside quotes a backslash escapes anything.
        char next = buffer[i + 1];
        if (quote == '"' && next != '"' && next != '\\' && next != '$') {
          builder_char(&token, c);
          i++;
        }
        else {
          builder_char(&token, next);
          i += 2;
        }
      }
      else {
        builder_char(&token, c);
        i++;
      }
    }

    if (quote != '\0') {
      free(token.data);
      arglist_reset(args);
      return -1;
    }

    if (token.length == 0 && !saw_quote) {
      continue;
    }

    char *copy = strdup(token.data);
    if (copy == NULL) {
      free(token.data);
      fatal_oom();
    }
    arglist_push(args, copy);
  }

  free(token.data);
  return args->count;
}

int parser_split_words(const char *buffer, ArgList *args) {
  return tokenize(buffer, args);
}

void arglist_append(ArgList *args, const char *text) {
  char *copy = strdup(text);
  if (copy == NULL) {
    fatal_oom();
  }
  arglist_push(args, copy);
}
