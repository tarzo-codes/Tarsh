#ifndef PARSER_H
#define PARSER_H

// A growable, NULL-terminated argument vector suitable for execvp().
// `items` always has `count` real entries followed by a NULL sentinel.
typedef struct {
  char **items;
  int count;
  int capacity;
}ArgList;

void arglist_init(ArgList *args);
void arglist_reset(ArgList *args);
void arglist_free(ArgList *args);

// Appends a copy of `text`.
void arglist_append(ArgList *args, const char *text);

// Splits `buffer` into words the way the user sees them: whitespace
// separates words, quotes group them, a backslash escapes the next
// character. Nothing is expanded. Used to inspect what the user is still
// typing (e.g. for the fzf pickers). Returns the word count, or -1 inside
// an unterminated quote.
int parser_split_words(const char *buffer, ArgList *args);

#endif
