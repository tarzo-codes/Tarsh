#ifndef FOREIGN_H
#define FOREIGN_H

#include <stdio.h>

// Recognises commands written for another shell (bash, zsh, fish), runs
// them there, and explains in one line what happened.

typedef struct {
  char language[8];   // "bash", "zsh", "fish" or "sh"
  char what[256];     // e.g. "a for-loop: ran the commands ... once for each item"
  char tip[200];      // the tarsh way to do it, or ""
  char kind[24];      // used to show each tip only once
}Foreign;

// Works out which shell `line` was written for and what it does.
void foreign_analyze(const char *line, Foreign *out);

// Same for a whole script file (by #! line, extension, then contents).
// Returns 0 if it's a tarsh script or unreadable.
int foreign_analyze_file(const char *path, Foreign *out);

// Runs `line` in the right shell and prints the note afterwards.
int foreign_run_line(const char *line);

// Prints the "ran in bash: ..." note (only at an interactive prompt).
void foreign_explain(const Foreign *info, const char *subject);

// `explain on/off`, and EXPLAIN=0/1 in config.t.
void foreign_set_enabled(int enabled);
int foreign_enabled(void);
int foreign_configure(const char *key, const char *value);
void foreign_write_defaults(FILE *file);

#endif
