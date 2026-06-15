#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int main() {
    char *command_buffer;
    size_t n = 0;
    while (true) {
    
    printf(" $ ");
    getline(&command_buffer,&n,stdin);
    printf("%s",command_buffer);
    
    command_buffer[strcspn(command_buffer,"\n")] = '\0';

    if (strcmp(command_buffer,"exit") == 0) {
      printf("Exited the shell");
      break;
    }
  }
}
