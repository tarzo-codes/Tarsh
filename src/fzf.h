#ifndef FZF_H
#define FZF_H

#include <stdio.h>

#include "lineedit.h"

// fzf pickers for the line editor:
//   space after `cd`, `cp`, `mv`, `nvim`, ...  pick a path for that command
//   Tab                                       pick a command or path for the current word
//   Ctrl+R                                    search history

void fzf_setup(void);
int fzf_configure(const char *key, const char *value);
void fzf_write_defaults(FILE *file);

// True when fzf is installed and enabled.
int fzf_enabled(void);

// Each returns 1 when it changed the line, 0 when the key should do its
// ordinary thing instead.
int fzf_on_space(EditBuffer *line);
int fzf_on_tab(EditBuffer *line);
int fzf_on_history(EditBuffer *line);

#endif
