#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>



int main() {
  char prompt[128] = "$";
  // basic variables
  char command[256];
  char commandFilter[256];

  // Tokenizatio
  char *token;
  char *args[64];
  
  // Setup
  
  while (1) { 
    
    getcwd(prompt, sizeof(prompt)); // 1. Fill prompt with the path
    strcat(prompt, " $");           // 2. Glue the " $" right onto the end
  
  // Process management
    printf("%s ",prompt);
    fgets(command, sizeof(command),stdin);

    // Filtering the \n from the input command
    strncpy(commandFilter, command, sizeof(commandFilter) - 1);
    commandFilter[sizeof(commandFilter) - 1] = '\0';
    commandFilter[strcspn(commandFilter, "\n")] = '\0';

    if (strcmp(commandFilter,"exit") == 0) {
      printf("exited the shell\n");
      return 0;
    }
    
    token = strtok(command," \n");
    size_t i = 0;
    while (token != NULL && i < (sizeof(args) / sizeof(args[0]))) {
      args[i] = token;
      token = strtok(NULL, " \n");
      i++;
    }
    args[i] = NULL;

    if (args[0] == NULL) {
      continue;
    }

   
  else if (strcmp(args[0], "echo") == 0) {
      if (args[1] != NULL) {
        if (args[1][0] == '$') {
          if (getenv(args[1] + 1) != NULL) {
            printf("%s\n", getenv(args[1] + 1));
          }
          else {
            printf("\n");
          }
        } 
        else {
          printf("%s\n", args[1]);
        }
      } 
      else {
        printf("\n");
      }
      
      continue;
    }


    // Process management
    if (strcmp(args[0],"cd")==0) {
      //do something
      if (args[1]==NULL) {
        chdir(getenv("HOME"));
      }
      else {
        chdir(args[1]);
      }
      getcwd(prompt, sizeof(prompt)); // 1. Fill prompt with the path
      strcat(prompt, " $");           // 2. Glue the " $" right onto the end
  

      continue;
    }

    pid_t p = fork();
    if (p < 0) {
      printf("fork failed\n");
      exit(1);
    }
    else if (p == 0) {
      // child process
      execvp(args[0],args);
    }
    else {
      // parent process
      wait(NULL);
    }
  }
}
