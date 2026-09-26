#ifndef CONFIG_H
#define CONFIG_H

// Returns "$HOME/.config/tarsh", creating it when absent. The string is
// owned by the config module.
const char *config_directory(void);

// Loads ~/.config/tarsh/config.t, writing a default one first if it is
// missing, and hands every KEY=VALUE line to the module that owns it.
// Returns 1 when the file was newly created (i.e. this is a first run).
int config_load(void);

// ~/.config/tarsh/config.t: settings (prompt, fzf, language).
const char *config_settings_path(void);

// ~/.config/tarsh/tarshrc: commands run when an interactive tarsh starts
// (aliases, exports, lang ...). tarsh's answer to ~/.bashrc.
const char *config_rc_path(void);

// Writes a starter tarshrc if there isn't one. Returns 1 if it did.
int config_ensure_rc(void);

#endif
