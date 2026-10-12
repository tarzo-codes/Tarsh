#ifndef HIGHLIGHT_H
#define HIGHLIGHT_H

// Colors a command line as it's typed, like fish: commands that exist in
// green, unknown ones in red, options, strings, variables, operators,
// comments, and paths that exist underlined. Returns a malloc'd copy with
// color codes added (the visible characters are unchanged).
char *highlight_line(const char *text);

#endif
