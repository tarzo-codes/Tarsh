#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void handleEcho(char **args) {
    char sanitizedText[128] = "";
    int sanitizedIndex = 0;

    if (args[1] == NULL) {
        printf("\n");
    }
    else {
        // Loop 1: Process every word in the argument list
        for (int i = 1; args[i] != NULL; i++) {

            // Stage A: Handle Environment Variables ($)
            if (args[i][0] == '$') {
                char *val = getenv(args[i] + 1);
                if (val != NULL) {
                    for (int k = 0; val[k] != '\0'; k++) {
                        sanitizedText[sanitizedIndex] = val[k];
                        sanitizedIndex++;
                    }
                }
                // Word is fully handled (even if variable was empty), skip to next word
                continue; 
            }

            // Stage B: Handle Regular Text and Escapes
            for (int j = 0; args[i][j] != '\0'; j++) {
                
                // Handle escaped characters
                if (args[i][j] == '\\') {
                    j++; // Step past the backslash to look at the target character
                    
                    if (args[i][j] != '\0') {
                        // Exception Rule 1: Handle newline
                        if (args[i][j] == 'n') {
                            sanitizedText[sanitizedIndex] = '\n';
                        }
                        // Exception Rule 2: Handle tab
                        else if (args[i][j] == 't') {
                            sanitizedText[sanitizedIndex] = '\t';
                        }
                        // Default Rule: Handle escaped quotes, slashes, or normal text
                        else {
                            sanitizedText[sanitizedIndex] = args[i][j];
                        }
                        sanitizedIndex++; // Advance clean buffer index exactly once
                    }
                }
                // Handle standard bounding quotes (ignore them completely)
                else if (args[i][j] == '"' || args[i][j] == '\'') {
                    continue; 
                }
                // Handle normal text characters
                else {
                    sanitizedText[sanitizedIndex] = args[i][j];
                    sanitizedIndex++;
                }
            }

            // Stage C: Add spaces between distinct words
            if (args[i + 1] != NULL) {
                sanitizedText[sanitizedIndex] = ' ';
                sanitizedIndex++;
            }
        }

        // Stage D: Output Phase (Seal the buffer and print to the screen)
        sanitizedText[sanitizedIndex] = '\0';
        printf("%s\n", sanitizedText);
    }
}
