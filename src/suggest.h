#ifndef SUGGEST_H
#define SUGGEST_H

// Explains a command that could not be found: typo corrections from
// $PATH and the builtins, habits from other systems (cls, dir, ipconfig),
// and "that's a directory, did you mean cd?".
void suggest_command_not_found(const char *name);

#endif
