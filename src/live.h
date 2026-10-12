#ifndef LIVE_H
#define LIVE_H

#include <stdio.h>

// Live preview: while you type a command made only of commands you've
// listed in LIVE_COMMANDS (grep, awk, sed, sort, ...), tarsh runs it in a
// sandbox and shows the first lines of its output under the prompt.
//
// The sandbox can't write, delete or rename files, use the network,
// signal other programs or touch the terminal, and is stopped after a
// moment, so even a mistyped command can't do harm.

void live_setup(void);
int live_configure(const char *key, const char *value);
void live_write_defaults(FILE *file);

int live_enabled(void);
void live_set_enabled(int on);
int live_hints_enabled(void);
void live_set_hints(int on);
int live_lines(void);
int live_delay_ms(void);

// Can `line` be previewed? If not and `why` is given, says why.
int live_eligible(const char *line, char *why, size_t why_size);

// Runs `line` in the sandbox and returns up to `max_lines` lines of its
// output (with grep's match highlighting), cleaned so it can't affect the
// terminal. *total_lines gets how many lines it produced (at least).
// Returns NULL if it couldn't run.
char *live_run(const char *line, int max_lines, int *total_lines, int *failed);

// `live` builtin.
int live_builtin(int argc, char **argv);

#endif
