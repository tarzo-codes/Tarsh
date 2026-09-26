#ifndef ALIAS_H
#define ALIAS_H

// Returns 0 on success, nonzero if `name` can't be an alias name.
int alias_set(const char *name, const char *value);
int alias_unset(const char *name);
const char *alias_get(const char *name);

// Prints `alias name='value'` lines; all of them when name is NULL.
// Returns nonzero if a named alias doesn't exist.
int alias_print(const char *name);

// Returns a malloc'd copy of `line` with aliases replaced wherever a
// command name is expected (line start, after | && || ; & and after
// keywords like do/then).
char *alias_expand_line(const char *line);

#endif
