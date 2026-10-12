#ifndef PIPEHINT_H
#define PIPEHINT_H

// Explains a command line in plain words without running anything:
// what each command does, what |, >, >>, <, <<, &&, ... do between them,
// what output to expect, and what a regex means.

// One line for under the prompt, or NULL when there's nothing to say
// (no operators, or not a command). malloc'd.
char *pipehint_summary(const char *line);

// The full step-by-step breakdown for `explain LINE`. malloc'd.
char *pipehint_detail(const char *line);

// "line starts, then one or more digits, then the end of the line".
// `extended` means ERE syntax (grep -E, awk, sed -E); otherwise BRE.
char *pipehint_regex(const char *pattern, int extended);

#endif
