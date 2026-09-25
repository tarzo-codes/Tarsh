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

// History: loaded from and appended to ~/.config/tarsh/history.
void lineedit_history_load(void);
void lineedit_history_add(const char *line);
int lineedit_history_count(void);
const char *lineedit_history_get(int index);

// Called around anything that hands the terminal to another program.
int lineedit_raw_on(void);
void lineedit_raw_off(void);

#endif
