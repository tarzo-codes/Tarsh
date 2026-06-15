#ifndef PROMPTS_H
#define PROMPTS_H
#include <limits.h>

typedef struct {
  int directory_depth;
  int show_execution_time;
  char prompt_symbol;

  const char *username;
  char *curr_dir;
  char hostname[HOST_NAME_MAX + 1];

  double last_command_duration;
}PromptState;




void setup_prompt();
void update_prompt();

#endif
