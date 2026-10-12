#ifndef THEME_UI_H
#define THEME_UI_H

// Serves the theme editor web page on 127.0.0.1 until the user clicks
// Done or presses Enter/Ctrl+C in the terminal. With `open_browser`, the
// page is opened automatically. `port` 0 picks a free port.
int theme_editor_run(int open_browser, int port);

#endif
