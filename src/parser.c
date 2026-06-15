#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"


char** main_command_parser(char *buffer, char **args) {
  // TODO: Take the command and tokenize it then put it in the args pointer
  
  char *buffer_tok = strtok(buffer," ");
  int tok_counter = 0;
  int max_slots = 64;

  while (buffer_tok != NULL) {
    if (tok_counter >= max_slots - 1) {
      max_slots *= 2;
      args = realloc(args, max_slots * sizeof(char *));
      if (args == NULL) {
        perror("Fatal: Shell ran out of heap memory during parsing");
        exit(EXIT_FAILURE);
      }
    }
    args[tok_counter] = buffer_tok;
    buffer_tok = strtok(NULL," ");
    tok_counter++;
  }
  args[tok_counter] = NULL;

  return args;
}
