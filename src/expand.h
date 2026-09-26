#ifndef EXPAND_H
#define EXPAND_H

#include <sys/types.h>

#include "parser.h"

// Turns one raw word into its final argument(s):
//   ~  $NAME  ${NAME}  ${NAME:-default}  $? $$ $! $# $@ $0..$9
//   $(command)  `command`
// then splits unquoted expansion results on whitespace and expands
// unquoted * ? [...] against the filesystem (a pattern with no matches is
// left as typed). Appends the results to `out`.
void expand_word(const char *raw, ArgList *out);

// Like expand_word but always produces exactly one string (no splitting;
// a glob is used only if it matches exactly one path). For redirection
// targets and assignment values. Returns a malloc'd string.
char *expand_single(const char *raw);

// Runs `command` in a forked copy of the shell and returns its standard
// output with trailing newlines removed (malloc'd).
char *expand_capture(const char *command);

void expand_init(void);
void expand_set_last_status(int status);
int expand_last_status(void);
void expand_set_last_background(pid_t pid);

// $0 $1 ... for scripts and `tarsh -c`. args[0] is $0.
void expand_set_positional(int count, char **args);
// Drops $1..$n (the `shift` builtin). Returns nonzero if there weren't n.
int expand_shift(int n);

#endif
