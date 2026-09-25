#ifndef LANG_H
#define LANG_H

#include <stdio.h>

// tarsh runs simple commands itself. Anything that needs real shell syntax
// (pipes, redirection, &&, loops, globs, $(...), fish's `set`...) is handed
// to the selected language backend: bash, zsh, fish or sh.

void lang_setup(void);
int lang_configure(const char *key, const char *value);
void lang_write_defaults(FILE *file);

// Name of the active language, e.g. "bash".
const char *lang_current_name(void);

// Switches the active language. Prints a friendly message and returns
// nonzero if the name is unknown or the shell is not installed.
int lang_set(const char *name);

// Prints the known languages and which are installed.
void lang_list(void);

// True when `line` uses syntax the native tarsh parser does not handle.
int lang_needs_backend(const char *line);

// Runs `line` through the active backend and returns its exit status.
// Directory changes and exported variables made by the line are carried
// back into tarsh afterwards.
int lang_run(const char *line);

#endif
