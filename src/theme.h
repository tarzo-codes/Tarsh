#ifndef THEME_H
#define THEME_H

#include <stdio.h>

#include "parser.h"

// The theming engine. A theme is a list of prompt segments (cwd, git,
// languages, status, ...) with colors, icons and a separator style. It
// lives in ~/.config/tarsh/theme.t, a plain text file:
//
//   [theme]
//   name=Ocean
//   separator=powerline     powerline | round | slant | plain | none | custom
//   glyphs=nerd             nerd | unicode | ascii
//   colors=auto             auto | truecolor | 256 | 16
//
//   [segment]
//   type=cwd
//   fg=#ffffff
//   bg=blue
//   icon=auto
//
// When the file is missing (or has enabled=0) the classic FORMAT prompt
// from config.t is used.

typedef struct Theme Theme;

// What the prompt shows about the shell's state.
typedef struct {
  int last_status;
  double last_duration;
  int jobs;
  const char *shell_language;
  const char *user;
  const char *host;
}ThemeContext;

// Loads ~/.config/tarsh/theme.t (if any). Problems are printed once.
void theme_init(void);
int theme_active(void);
const char *theme_path(void);
const char *theme_current_name(void);

// Renders the active theme. Returns a malloc'd string with ANSI colors.
char *theme_render_active(const ThemeContext *context);

// Parses theme text. Problems (unknown keys, bad colors) are appended to
// `warnings` when it isn't NULL; parsing always produces a usable theme.
Theme *theme_parse(const char *text, ArgList *warnings);
void theme_free(Theme *theme);
char *theme_render(const Theme *theme, const ThemeContext *context);

// The saved theme file's text, or the default preset if there is none.
char *theme_saved_text(void);

// Writes `text` to theme.t and makes it the active theme.
int theme_save_text(const char *text);

// Turns the saved theme on or off (enabled=1/0) without losing it.
int theme_set_enabled(int enabled);

// Built-in starting points.
int theme_preset_count(void);
const char *theme_preset_name(int index);
const char *theme_preset_description(int index);
const char *theme_preset_text(int index);
int theme_preset_find(const char *name);

// Facts about the current folder, for the editor's "detected here" panel.
// Writes a JSON object.
void theme_write_context_json(FILE *out);

// Writes `text` as a JSON string literal.
void theme_json_string(FILE *out, const char *text);

#endif
