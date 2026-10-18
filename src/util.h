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

// Reads one UTF-8 character from `text` (at most `length` bytes) into
// *codepoint and returns how many bytes it used (at least 1; a broken
// byte reads as U+FFFD).
size_t utf8_decode(const char *text, size_t length, unsigned *codepoint);

// Columns a character takes in a terminal: 2 for wide CJK and emoji, 0 for
// combining marks and joiners, 1 otherwise (Nerd Font icons included).
int char_columns(unsigned codepoint);

// Skips an escape sequence (CSI like colors, or OSC like hyperlinks)
// starting at text[0] == ESC. Returns its length in bytes.
size_t escape_length(const char *text, size_t length);

// Columns `text` takes on screen; escape sequences count zero.
size_t text_columns(const char *text, size_t length);

#endif
