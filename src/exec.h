#ifndef EXEC_H
#define EXEC_H

#include <stdio.h>

// Runs one (possibly multi-line) command line and returns its status.
int exec_run_line(const char *text);

// Runs a tarsh script or rc file line by line in this shell. Multi-line
// blocks (if/for/while ... ) are gathered before running. Returns the
// last status. With `quiet_missing`, a missing file is not an error.
int exec_run_file(const char *path, int quiet_missing);
int exec_run_stream(FILE *input);

// Set by `exit`; the main loop and script runners stop when it is.
int exec_should_exit(void);
void exec_cancel_exit(void);

// True while an rc file or `source`d file is running.
int exec_is_sourcing(void);

#endif
