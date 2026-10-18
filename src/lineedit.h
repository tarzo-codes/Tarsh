#ifndef LINEEDIT_H
#define LINEEDIT_H

#include <stddef.h>

// The line being edited. `cursor` is a byte offset into `data`.
typedef struct {
  char *data;
  size_t length;
  size_t capacity;
  size_t cursor;
}EditBuffer;

// Inserts `text` at the cursor and moves the cursor past it.
void editbuf_insert(EditBuffer *line, const char *text);

// Deletes `count` bytes ending at the cursor.
void editbuf_delete_before(EditBuffer *line, size_t count);

// Reads one line from the terminal with editing, history and the fzf
// pickers. Returns a malloc'd line (without the newline), or NULL on
// Ctrl+D at an empty prompt.
char *lineedit_read(const char *prompt);

// Text drawn at the right edge of the input row (a right prompt) while the
// line is short enough; "" or NULL for none. Applies to the next read.
void lineedit_set_right_prompt(const char *text);

// Same, with `initial` already typed (used to pre-indent script blocks).
char *lineedit_read_initial(const char *prompt, const char *initial);

// History: loaded from and appended to ~/.config/tarsh/history.
void lineedit_history_load(void);
void lineedit_history_add(const char *line);
int lineedit_history_count(void);
const char *lineedit_history_get(int index);

// HIGHLIGHT / AUTOSUGGEST settings in config.t.
#include <stdio.h>
void lineedit_configure_defaults(FILE *file);
int lineedit_configure(const char *key, const char *value);

// Private mode: nothing is read from or saved to the history file.
void lineedit_set_private(int on);

// `history delete TEXT` / `history clear`.
int lineedit_history_delete(const char *text);
void lineedit_history_clear(void);

// Called around anything that hands the terminal to another program.
int lineedit_raw_on(void);
void lineedit_raw_off(void);

#endif
