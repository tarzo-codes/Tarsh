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

// Loads ~/.config/tarsh/config.t, writing a default one first if it is
// missing. Safe to call once at startup.
void setup_prompt(void);

// Records how the previous command went so the next prompt can show it.
void update_prompt(double duration_seconds, int status);

// Renders the current prompt. The returned string is owned by the prompt
// module and stays valid until the next call.
const char *render_prompt(void);

#endif
