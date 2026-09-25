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

// Splits `buffer` into tokens. Whitespace separates tokens, single quotes
// are literal, double quotes allow \" escapes, and a backslash outside
// quotes escapes the next character. Outside single quotes $NAME, ${NAME}
// and $? are expanded, and a leading ~ becomes $HOME. Returns the number
// of tokens, or -1 on an unterminated quote.
int main_command_parser(const char *buffer, ArgList *args);

// Same tokenizing rules, but no $ or ~ expansion. Used to inspect what
// the user is still typing.
int parser_split_words(const char *buffer, ArgList *args);

// The value $? expands to.
void parser_set_last_status(int status);

#endif
