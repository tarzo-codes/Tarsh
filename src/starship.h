#ifndef STARSHIP_H
#define STARSHIP_H

#include <stdio.h>

// Starship (starship.rs) as tarsh's prompt. STARSHIP in config.t:
//   auto  use starship when it's installed and you have a starship.toml,
//         unless a tarsh theme is in use (the default)
//   on    always use starship when it's installed
//   off   never
// starship is given the last exit status, how long the command took, the
// number of jobs and the terminal width, so its status, cmd_duration and
// jobs modules work as they do in other shells.

void starship_setup(void);
int starship_configure(const char *key, const char *value);
void starship_write_defaults(FILE *file);

// Changes the setting for this session: 0 off, 1 on, 2 auto.
void starship_set_mode(int mode);
int starship_mode(void);

// Where the starship program is, or NULL when it isn't installed.
const char *starship_path(void);

// Should the prompt come from starship right now? `theme_in_use` says
// whether a tarsh theme is active (auto gives way to it).
int starship_wanted(int theme_in_use);

// Runs `starship prompt` and returns the prompt (malloc'd), or NULL when
// starship is missing, fails or takes too long.
char *starship_render(int last_status, double last_duration, int jobs);

// Does starship.toml set right_format (a right-hand prompt)?
int starship_has_right_prompt(void);

// `starship prompt --right`, the same way.
char *starship_render_right(int last_status, double last_duration, int jobs);

#endif
