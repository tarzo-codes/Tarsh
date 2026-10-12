#ifndef FISH_H
#define FISH_H

#include "parser.h"

// fish-style features: abbreviations, `set` with universal variables,
// directory history, and the string/math/count/contains/random,
// funcsave/funced/functions builtins.

int fish_builtin_exists(const char *name);
void fish_write_builtin_names(ArgList *out);
// Returns 1 if `args` was a fish builtin (status in *status).
int fish_builtin_run(ArgList *args, int *status);

// Abbreviations: `abbr gco git checkout` turns gco into `git checkout`
// as you type it.
const char *abbr_get(const char *name);

// Universal variables (set -U): saved in ~/.config/tarsh/universal and
// shared live between every open tarsh.
void universal_load(void);
void universal_sync(void);   // call before each prompt

// Directory history for prevd / nextd / dirh / cdh.
void dirs_note_cwd(void);    // call before each prompt

#endif
