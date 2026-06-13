#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>


int main() {
  printf("%s\n",getenv("HOME"));
}
