#ifndef SCRIPT_H
#define SCRIPT_H

#include "parser.h"

// tarsh script: a small Python-like language that keeps the shell's
// habits. Statements look like Python:
//
//   files = glob("*.log")
//   for f in files:
//       if size(f) > 1000:
//           gzip $f                 # any other line is a shell command
//           print(f"packed {f}")
//
//   def greet(name="world"):
//       echo "hello $name"          # $name works in commands
//
// and commands work as in the shell: `out = $(git branch)` captures
// output, `if ?(make):` tests whether a command worked, and functions can
// be run like commands (`greet bob`). See docs/SCRIPTING.md.

// Does the first line of `text` use script syntax (x = 1, if x:, def,
// print(...), ...)? If not, it's an ordinary command line.
int script_claims(const char *text);

// True when `line` opens a block (if/for/while/def ... ending in ':') so
// more lines are needed at the prompt.
int script_opens_block(const char *line);

// Runs script source. `origin` names it in error messages; `first_line`
// is the line number of its first line. In `repl` mode values of bare
// expressions are printed, like Python's prompt.
int script_run_source(const char *source, const char *origin, int first_line, int repl);

// `for x in items` + an indented block in a .tsh file: reports the
// forgotten ':' and returns 1.
int script_missing_colon(const char *text, const char *origin, int first_line);

// True if the last script_run_source() stopped on an error (a bad
// expression, a missing variable...), as opposed to a command failing.
int script_last_failed(void);

// Functions defined with `def`, usable as commands.
int script_function_exists(const char *name);
int script_call_command(ArgList *argv);
char *script_function_source(const char *name);
void script_function_names(ArgList *out);
int script_function_erase(const char *name);

// For highlighting and completion.
int script_builtin_exists(const char *name);
int script_is_keyword(const char *word);

// Shell variables (not exported to programs unless `export`ed).
// Returns 0 when unset, 1 for a string, 2 for a list (in `list`).
int script_var_get(const char *name, char **string, ArgList *list);
void script_var_set(const char *name, const char *value);
void script_var_set_list(const char *name, ArgList *values);
int script_var_unset(const char *name);
int script_var_exists(const char *name);
void script_var_print_all(void);

// Evaluates an expression for the `math` builtin. Returns 0 and a
// malloc'd result, or nonzero after printing the problem.
int script_math(const char *expression, char **result);

#endif
