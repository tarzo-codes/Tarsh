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

int main_command_parser(const char *buffer, ArgList *args) {
  arglist_reset(args);

  // The unquoted result of a token is never longer than its source text,
  // so one scratch buffer of that size is always enough.
  size_t length = strlen(buffer);
  char *scratch = malloc(length + 1);
  if (scratch == NULL) {
    fatal_oom();
  }

  size_t i = 0;
  while (i < length) {
    while (i < length && is_token_space(buffer[i])) {
      i++;
    }
    if (i >= length) {
      break;
    }

    size_t out = 0;
    char quote = '\0';

    while (i < length) {
      char c = buffer[i];

      if (quote == '\0' && is_token_space(c)) {
        break;
      }
      else if (quote == '\0' && (c == '\'' || c == '"')) {
        quote = c;
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
          scratch[out++] = c;
          i++;
        }
        else {
          scratch[out++] = next;
          i += 2;
        }
      }
      else {
        scratch[out++] = c;
        i++;
      }
    }

    if (quote != '\0') {
      free(scratch);
      arglist_reset(args);
      return -1;
    }

    scratch[out] = '\0';
    char *token = strdup(scratch);
    if (token == NULL) {
      free(scratch);
      fatal_oom();
    }
    arglist_push(args, token);
  }

  free(scratch);
  return args->count;
}
