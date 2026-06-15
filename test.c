#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>    // for fork() and execvp()
#include <sys/wait.h>  // for waitpid()
#include <limits.h>

typedef struct {
  int directory_depth;
  int show_execution_time;
  char prompt_symbol;

  const char *username;
  char *curr_dir;
  char hostname[HOST_NAME_MAX + 1];

  double last_command_duration;
}promptstate;


int main() {
  static promptstate active_config;
  active_config.username = getenv("USER");
  active_config.curr_dir = getenv("PWD");
  

  gethostname(active_config.hostname,sizeof(active_config.hostname));
  
  printf("%s@%s in %s\n",active_config.username,active_config.hostname,active_config.curr_dir);
}
