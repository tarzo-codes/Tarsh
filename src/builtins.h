#ifndef BUILTINS_H
#define BUILTINS_H

#include <stdio.h>

#include "parser.h"

// Runs `args` as a builtin if the command name is one. Returns 1 when the
// command was handled (and writes its exit status to *status), 0 when it
// is not a builtin and should be executed as an external program.
// *should_exit is set when the shell itself must terminate.
int builtin_run(ArgList *args, int *status, int *should_exit);

// True when `name` is the name of a builtin command.
int builtin_exists(const char *name);

// The index'th builtin name, or NULL past the end.
const char *builtin_name_at(int index);

// Writes every builtin name to `file`, one per line.
void builtin_write_names(FILE *file);

#endif
