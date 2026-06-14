#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h> // Required for chdir() and getcwd()
#include "cd.h"

void handle_cd(char **args, char *prompt, int prompt_size) {
    // If user typed just "cd", send them to their HOME directory
    if (args[1] == NULL) {
        char *home = getenv("HOME");
        if (home != NULL) {
            chdir(home);
        }
    }
    // Otherwise, move to the specific folder they typed
    else {
        if (chdir(args[1]) != 0) {
            // If chdir returns non-zero, the directory doesn't exist
            perror("cd failed");
        }
    }

    // Update the prompt buffer dynamically based on our new location
    getcwd(prompt, prompt_size); 
    strcat(prompt, " $ ");       
}
