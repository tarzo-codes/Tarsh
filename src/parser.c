#include <stdio.h>
#include <stdlib.h>
#include <string.h>


char** main_command_parser(char *buffer, char **args) {
  // TODO: Take the command and tokenize it then put it in the args pointer
  
  char *buffer_tok = strtok(buffer," ");
  int tok_counter = 0;
  while (buffer_tok != NULL) {
    args[tok_counter] = buffer_tok;
    buffer_tok = strtok(NULL," ");
    tok_counter++;
  }
  args[tok_counter] = NULL;

  return args;
}
