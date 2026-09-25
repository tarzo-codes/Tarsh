#ifndef PROMPTS_H
#define PROMPTS_H

#include <limits.h>

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

#define PROMPT_FORMAT_MAX 128
#define PROMPT_SYMBOL_MAX 8

typedef struct {
  int directory_depth;
  int show_execution_time;
  char prompt_symbol[PROMPT_SYMBOL_MAX];
  char format[PROMPT_FORMAT_MAX];

  const char *username;
  char hostname[HOST_NAME_MAX + 1];

  double last_command_duration;
  int last_command_status;
}PromptState;

#include <stdio.h>

// Sets the built-in defaults. Call before config_load().
void setup_prompt(void);

// Applies one KEY=VALUE pair from the config file. Returns 1 if the key
// belongs to the prompt, 0 otherwise.
int prompt_configure(const char *key, const char *value);

// Writes the prompt's section of a fresh config file.
void prompt_write_defaults(FILE *file);

// Records how the previous command went so the next prompt can show it.
void update_prompt(double duration_seconds, int status);

// Renders the current prompt. The returned string is owned by the prompt
// module and stays valid until the next call.
const char *render_prompt(void);

#endif
