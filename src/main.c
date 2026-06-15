#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>    // For fork() and execvp()
#include <sys/wait.h>  // For waitpid()

#include "parser.h"
#include "prompts.h"

int main() {
    char *command_buffer = NULL;
    size_t buffer_size = 0;
    // NOTE: Initial termianl prompt setup;
    setup_prompt();

    while (true) {
    
    printf(" $ ");
    ssize_t charecter_read = getline(&command_buffer,&buffer_size,stdin);
    printf("%s",command_buffer);
    
    // NOTE: Error Handling
    // Handling ctrl+D EOF 
    if (charecter_read == -1) {
      printf("\n EOF detecterd \n Exitting.... \n");
      break;
    }
    
    command_buffer[strcspn(command_buffer,"\n")] = '\0';
    
    // Handle Empty inputs
    if (strlen(command_buffer) == 0) {
      continue;
    } 
    if (strcmp(command_buffer,"exit") == 0) {
      printf("Exited the shell");
      break;
    }
    // NOTE: Parsing commands and separating arguments

    char **args = malloc(64 * sizeof(char *));
    
    if (args == NULL) {
      perror("Fatal: Allocation failure at loop start");
      exit(EXIT_FAILURE);
    }

    main_command_parser(command_buffer, args);

    printf("[Parser status] Parser Arguments: \n");
    for (int i = 0;args[i] != NULL;i++) {
      printf("  -> Arguments[%d]: %s\n",i,args[i]);
    }

    // NOTE: Args processing
    pid_t pid = fork();

    if (pid < 0) {
      perror("Frok failure");
      exit(EXIT_FAILURE);
    }
    else if (pid == 0) {
      //child process
      execvp(args[0],args);

      //Handle failed command
      printf("Command: %s",command_buffer);
      perror("Command failed ");
      exit(EXIT_FAILURE);
    }
    else {
      //parent process;
      int status;
      waitpid(pid, &status, 0);
    }

    free(args);
  }
  free(command_buffer);
  return 0;
}
