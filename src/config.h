#ifndef CONFIG_H
#define CONFIG_H

// Returns "$HOME/.config/tarsh", creating it when absent. The string is
// owned by the config module.
const char *config_directory(void);

// Loads ~/.config/tarsh/config.t, writing a default one first if it is
// missing, and hands every KEY=VALUE line to the module that owns it.
// Returns 1 when the file was newly created (i.e. this is a first run).
int config_load(void);

#endif
