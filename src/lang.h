#ifndef LANG_H
#define LANG_H

#include <stdio.h>

// tarsh runs commands, pipes, redirections, && || ; &, globs and $(...)
// itself. Things only a full shell language can do (if/for/while/case,
// functions, subshells, here-documents, $((math)), {a,b}, fish syntax) are
// handed to the selected language backend: bash, zsh, fish or sh.

void lang_setup(void);
int lang_configure(const char *key, const char *value);
void lang_write_defaults(FILE *file);

// Name of the active language, e.g. "bash".
const char *lang_current_name(void);

// Switches the active language. Returns nonzero (with a friendly
// message) if the name is unknown or the shell is not installed.
int lang_set(const char *name, int verbose);

// Prints the known languages and which are installed.
void lang_list(void);

// True when `line` uses syntax tarsh does not run natively.
int lang_needs_backend(const char *line);

// Runs `line` through the active backend and returns its exit status.
// Directory changes and exported variables made by the line are carried
// back into tarsh afterwards.
int lang_run(const char *line);

// Runs `line` in a specific language (falls back to the active one if
// that isn't installed).
int lang_run_as(const char *name, const char *line);
int lang_installed(const char *name);
const char *lang_install_hint(const char *name);

// What the last backend run brought back into tarsh.
int lang_last_stopped(void);
void lang_clear_changes(void);
const char *lang_last_directory(void);      // "" if the folder didn't change
#include "parser.h"
ArgList *lang_last_variables(void);

// Login shells: runs /etc/profile and ~/.profile through sh and keeps the
// variables they set (PATH and friends).
void lang_source_login_profiles(void);

#endif
