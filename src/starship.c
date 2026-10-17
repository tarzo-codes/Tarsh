// pipe2()
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#include "starship.h"
#include "util.h"

#define MODE_OFF 0
#define MODE_ON 1
#define MODE_AUTO 2

// Longer than this and we fall back to tarsh's own prompt for this line.
#define RENDER_TIMEOUT_MS 2000
#define OUTPUT_MAX 65536

static int mode = MODE_AUTO;
static int looked_up = 0;
static char program[PATH_MAX];

void starship_setup(void) {
  mode = MODE_AUTO;
  // starship uses these to tell shells apart and to group its logs.
  if (getenv("STARSHIP_SESSION_KEY") == NULL) {
    char key[17];
    unsigned seed = (unsigned)time(NULL) ^ ((unsigned)getpid() << 16);
    for (int i = 0;i < 16;i++) {
      seed = seed * 1103515245u + 12345u;
      key[i] = "0123456789abcdef"[(seed >> 16) & 15];
    }
    key[16] = '\0';
    setenv("STARSHIP_SESSION_KEY", key, 1);
  }
}

int starship_configure(const char *key, const char *value) {
  if (strcmp(key, "STARSHIP") != 0) {
    return 0;
  }
  if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0) mode = MODE_ON;
  else if (strcmp(value, "off") == 0 || strcmp(value, "0") == 0) mode = MODE_OFF;
  else mode = MODE_AUTO;
  return 1;
}

void starship_write_defaults(FILE *file) {
  fprintf(file, "# Use the starship prompt (starship.rs): auto, on or off.\n");
  fprintf(file, "# auto uses it when it's installed and ~/.config/starship.toml exists,\n");
  fprintf(file, "# unless a tarsh theme is in use.\n");
  fprintf(file, "STARSHIP=auto\n");
}

void starship_set_mode(int new_mode) {
  mode = new_mode;
}

int starship_mode(void) {
  return mode;
}

const char *starship_path(void) {
  if (!looked_up) {
    looked_up = 1;
    if (!path_lookup("starship", program, sizeof(program))) {
      program[0] = '\0';
    }
  }
  return program[0] ? program : NULL;
}

static int has_config(void) {
  const char *custom = getenv("STARSHIP_CONFIG");
  if (custom != NULL && custom[0] != '\0') {
    return access(custom, R_OK) == 0;
  }
  char path[PATH_MAX];
  const char *xdg = getenv("XDG_CONFIG_HOME");
  const char *home = getenv("HOME");
  if (xdg != NULL && xdg[0] != '\0') {
    snprintf(path, sizeof(path), "%s/starship.toml", xdg);
    if (access(path, R_OK) == 0) return 1;
  }
  if (home != NULL) {
    snprintf(path, sizeof(path), "%s/.config/starship.toml", home);
    if (access(path, R_OK) == 0) return 1;
  }
  return 0;
}

int starship_wanted(int theme_in_use) {
  if (mode == MODE_OFF) {
    return 0;
  }
  // A program installed while tarsh is running should be found too.
  if (starship_path() == NULL) {
    looked_up = 0;
    if (starship_path() == NULL) return 0;
  }
  if (mode == MODE_ON) {
    return 1;
  }
  return !theme_in_use && has_config();
}

static long long now_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

char *starship_render(int last_status, double last_duration, int jobs) {
  const char *path = starship_path();
  if (path == NULL) {
    return NULL;
  }

  int columns = 80;
  struct winsize size;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
    columns = size.ws_col;
  }

  char status_arg[32], duration_arg[48], jobs_arg[32], width_arg[32];
  snprintf(status_arg, sizeof(status_arg), "--status=%d", last_status);
  snprintf(duration_arg, sizeof(duration_arg), "--cmd-duration=%lld", (long long)(last_duration * 1000.0));
  snprintf(jobs_arg, sizeof(jobs_arg), "--jobs=%d", jobs);
  snprintf(width_arg, sizeof(width_arg), "--terminal-width=%d", columns);
  char *argv[] = { (char *)path, "prompt", status_arg, duration_arg, jobs_arg, width_arg, NULL };

  int out[2];
  if (pipe2(out, O_CLOEXEC) != 0) {
    return NULL;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(out[0]);
    close(out[1]);
    return NULL;
  }
  if (pid == 0) {
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    dup2(out[1], STDOUT_FILENO);
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, STDIN_FILENO);
      // starship's warnings would land in the middle of the screen.
      dup2(null, STDERR_FILENO);
    }
    // An unknown shell name gets plain escape codes, which is what the line
    // editor measures (bash would get \[ \] and zsh %{ %} around them).
    setenv("STARSHIP_SHELL", "tarsh", 1);
    execv(path, argv);
    _exit(127);
  }
  close(out[1]);

  char *text = malloc(OUTPUT_MAX + 1);
  size_t used = 0;
  long long deadline = now_ms() + RENDER_TIMEOUT_MS;
  int timed_out = 0;
  while (text != NULL && used < OUTPUT_MAX) {
    long long left = deadline - now_ms();
    if (left <= 0) {
      timed_out = 1;
      break;
    }
    struct pollfd watch = { out[0], POLLIN, 0 };
    int ready = poll(&watch, 1, (int)left);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) {
      timed_out = ready == 0;
      break;
    }
    ssize_t got = read(out[0], text + used, OUTPUT_MAX - used);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) break;
    used += (size_t)got;
  }
  close(out[0]);
  if (timed_out) {
    kill(pid, SIGKILL);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }

  if (text == NULL || timed_out || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || used == 0) {
    free(text);
    // Don't make every prompt wait for it: off until asked again.
    mode = MODE_OFF;
    fprintf(stderr, "tarsh: starship %s, so tarsh's own prompt is back for now.\n"
                    "       check it with: starship prompt   then: theme starship\n",
            timed_out ? "took over 2 seconds to draw the prompt" : "couldn't draw the prompt");
    return NULL;
  }

  // Drop the readline-style markers some setups still emit, and carriage
  // returns, which would send the cursor back over the prompt.
  size_t kept = 0;
  for (size_t i = 0;i < used;i++) {
    char c = text[i];
    if (c == '\001' || c == '\002' || c == '\r') continue;
    text[kept++] = c;
  }
  text[kept] = '\0';
  return text;
}
