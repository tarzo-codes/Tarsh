#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

// Looks `name` up on $PATH. Writes the full path to `out` and returns 1 if
// an executable was found. Names containing '/' are checked as-is.
int path_lookup(const char *name, char *out, size_t out_size);

// Returns a malloc'd copy of `text` quoted so the tarsh parser (and
// bash/zsh/fish) read it back as exactly one word. Plain words are
// returned unquoted.
char *shell_quote(const char *text);

#endif
