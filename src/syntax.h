#ifndef SYNTAX_H
#define SYNTAX_H

#include <stddef.h>

#include "parser.h"

// tarsh's native command language, parsed into:
//   CommandList  pipelines joined by ;  &&  ||  &  or newlines
//   Pipeline     commands joined by |
//   Command      words (still unexpanded) plus redirections

typedef enum {
  REDIR_IN,           // <  file
  REDIR_OUT,          // >  file
  REDIR_APPEND,       // >> file
  REDIR_BOTH,         // &> file    (stdout and stderr)
  REDIR_BOTH_APPEND,  // &>> file
  REDIR_DUP,          // 2>&1
  REDIR_CLOSE         // 2>&-
}RedirKind;

typedef struct {
  RedirKind kind;
  int fd;
  int target_fd;
  char *target;       // raw word for file targets, NULL otherwise
}Redirect;

typedef struct {
  ArgList words;
  Redirect *redirs;
  int redir_count;
}Command;

typedef struct {
  Command *commands;
  int count;
  char *text;         // the source text, shown by `jobs`
}Pipeline;

typedef enum {
  CONNECT_SEQ,        // ;  or newline
  CONNECT_AND,        // &&
  CONNECT_OR,         // ||
  CONNECT_BG          // &
}Connector;

typedef struct {
  Pipeline *pipelines;
  Connector *connectors;  // connectors[i] follows pipelines[i]
  int count;
}CommandList;

// Returns the index just past the word starting at `start`. Quotes,
// backslashes, $(...), ${...} and `...` are kept inside the word. Sets
// *unterminated when a quote or substitution never closes.
size_t syntax_word_end(const char *text, size_t start, int *unterminated);

// Given text[start] == '$' and text[start + 1] == '(', returns the index
// just past the matching ')'.
size_t syntax_subst_end(const char *text, size_t start, int *unterminated);

// Parses `text`. Returns 0 on success; on failure returns -1 and writes a
// readable message to `error`.
int syntax_parse(const char *text, CommandList *list, char *error, size_t error_size);
void syntax_free(CommandList *list);

// True when `text` can't run yet because more input is needed: an open
// quote, a trailing | && || or backslash, or an unclosed if/for/while/
// case/{ block (or fish's function/begin/switch ... end).
int syntax_incomplete(const char *text, int fish);

#endif
