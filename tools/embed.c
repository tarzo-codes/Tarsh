// Turns a file into a C header so tarsh can carry it inside the binary:
//   embed NAME < input > output.h
// produces `static const unsigned char NAME[]` and `NAME_length`.
#include <stdio.h>

int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "usage: embed NAME < input > output.h\n");
    return 2;
  }
  printf("// Generated from the editor page by tools/embed.c. Do not edit.\n");
  printf("#include <stddef.h>\n\n");
  printf("static const unsigned char %s[] = {", argv[1]);
  int c;
  unsigned long count = 0;
  while ((c = getchar()) != EOF) {
    printf("%s0x%02x,", (count % 16 == 0) ? "\n  " : "", c);
    count++;
  }
  printf("\n  0x00\n};\n");
  printf("static const size_t %s_length = %lu;\n", argv[1], count);
  return 0;
}
