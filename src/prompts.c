#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>    // For fork() and execvp()
#include <sys/wait.h>  // For waitpid()
#include <sys/stat.h>  // For waitpid()
#include <sys/types.h>  // For waitpid()
#include <limits.h>

#include "prompts.h"

static PromptState active_config;

void setup_prompt() {
  
  active_config.directory_depth = 1;
  active_config.show_execution_time = 1;
  active_config.prompt_symbol = '$';
  active_config.last_command_duration = 0.0;

  active_config.username = getenv("USER");
  gethostname(active_config.hostname,sizeof(active_config.hostname));

  char *home = getenv("HOME");
  if (home == NULL) {
    home = ".";
  }

  int dir_size = snprintf(NULL, 0, "%s/.config/tarsh",home);
  
  char *dir_path = malloc(dir_size+1);
  
  snprintf(dir_path, dir_size + 1, "%s/.config/tarsh", home);
  
  mkdir(dir_path,0700);

  int file_size = snprintf(NULL,0,"%s/config.t",dir_path);
  char *config_path = malloc(file_size+1);
  snprintf(config_path,file_size+1,"%s/config.t",dir_path);

  FILE *file = fopen(config_path, "r");
  
  if (file == NULL) {

    file = fopen(config_path,"w");
    if (file != NULL) {
      fprintf(file, "# Tarsh Configuration File\n");
      fprintf(file, "# Modify these values to customize\n");
      fprintf(file, "FORMATE=[%%u@%%h] %%w %%s \n");
      fprintf(file, "DEPTH=%d\n", active_config.directory_depth);
      fprintf(file, "SHOW_TIME=%d\n", active_config.show_execution_time);
      fprintf(file, "SYMBOL=%c\n", active_config.prompt_symbol);
      fclose(file);
      printf("[Shell Initialization] Generated default config at: %s\n",config_path);
    }

    free(dir_path);
    free(config_path);
  }
   // Building The config
    char config_directory;
}
