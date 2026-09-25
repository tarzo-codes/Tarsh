#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <errno.h>
#include <limits.h>

#include "lineedit.h"
#include "config.h"
#include "fzf.h"

#define HISTORY_MAX 1000

#define KEY_CTRL(k) ((k) & 0x1f)
#define KEY_BACKSPACE 127
#define KEY_ESCAPE 27

static struct termios original_termios;
static int raw_enabled = 0;

static char *history[HISTORY_MAX];
static int history_count = 0;
static char history_path[PATH_MAX];

// ---------------------------------------------------------------------------
// Terminal mode
// ---------------------------------------------------------------------------

int lineedit_raw_on(void) {
  if (raw_enabled) {
    return 0;
  }
  if (tcgetattr(STDIN_FILENO, &original_termios) != 0) {
    return -1;
  }

  struct termios raw = original_termios;
  // No echo, no line buffering, and Ctrl+C / Ctrl+Z arrive as plain bytes
  // so the editor can decide what they mean.
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  raw.c_cflag |= CS8;
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
    return -1;
  }
  raw_enabled = 1;
  return 0;
}

void lineedit_raw_off(void) {
  if (raw_enabled) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
    raw_enabled = 0;
  }
}

// ---------------------------------------------------------------------------
// Edit buffer
// ---------------------------------------------------------------------------

static void editbuf_reserve(EditBuffer *line, size_t extra) {
  if (line->length + extra + 1 <= line->capacity) {
    return;
  }
  size_t new_capacity = line->capacity ? line->capacity : 128;
  while (line->length + extra + 1 > new_capacity) {
    new_capacity *= 2;
  }
  char *grown = realloc(line->data, new_capacity);
  if (grown == NULL) {
    perror("tarsh: out of memory");
    exit(EXIT_FAILURE);
  }
  line->data = grown;
  line->capacity = new_capacity;
}

void editbuf_insert(EditBuffer *line, const char *text) {
  size_t length = strlen(text);
  editbuf_reserve(line, length);
  memmove(line->data + line->cursor + length, line->data + line->cursor,
          line->length - line->cursor + 1);
  memcpy(line->data + line->cursor, text, length);
  line->length += length;
  line->cursor += length;
}

void editbuf_delete_before(EditBuffer *line, size_t count) {
  if (count > line->cursor) {
    count = line->cursor;
  }
  memmove(line->data + line->cursor - count, line->data + line->cursor,
          line->length - line->cursor + 1);
  line->cursor -= count;
  line->length -= count;
}

static void editbuf_set(EditBuffer *line, const char *text) {
  line->length = 0;
  line->cursor = 0;
  line->data[0] = '\0';
  editbuf_insert(line, text);
}

// UTF-8 continuation bytes look like 10xxxxxx; skip over them so the
// cursor always lands on a whole character.
static int is_continuation(char c) {
  return ((unsigned char)c & 0xC0) == 0x80;
}

static size_t previous_char(const EditBuffer *line, size_t at) {
  if (at == 0) {
    return 0;
  }
  at--;
  while (at > 0 && is_continuation(line->data[at])) {
    at--;
  }
  return at;
}

static size_t next_char(const EditBuffer *line, size_t at) {
  if (at >= line->length) {
    return line->length;
  }
  at++;
  while (at < line->length && is_continuation(line->data[at])) {
    at++;
  }
  return at;
}

static size_t display_width(const char *text, size_t length) {
  size_t width = 0;
  for (size_t i = 0;i < length;i++) {
    if (!is_continuation(text[i])) {
      width++;
    }
  }
  return width;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void refresh_line(const char *prompt, const EditBuffer *line) {
  // Build the whole frame first so the terminal gets one write.
  size_t frame_size = strlen(prompt) + line->length + 64;
  char *frame = malloc(frame_size);
  if (frame == NULL) {
    return;
  }

  int used = snprintf(frame, frame_size, "\r%s%s\x1b[K", prompt, line->data);
  size_t tail = display_width(line->data + line->cursor, line->length - line->cursor);
  if (tail > 0) {
    used += snprintf(frame + used, frame_size - used, "\x1b[%zuD", tail);
  }

  if (write(STDOUT_FILENO, frame, used) < 0) {
    // Nothing useful to do if the terminal went away.
  }
  free(frame);
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

static void history_push(const char *line) {
  if (history_count == HISTORY_MAX) {
    free(history[0]);
    memmove(history, history + 1, (HISTORY_MAX - 1) * sizeof(char *));
    history_count--;
  }
  history[history_count] = strdup(line);
  if (history[history_count] != NULL) {
    history_count++;
  }
}

void lineedit_history_load(void) {
  snprintf(history_path, sizeof(history_path), "%s/history", config_directory());

  FILE *file = fopen(history_path, "r");
  if (file == NULL) {
    return;
  }

  char *entry = NULL;
  size_t entry_size = 0;
  while (getline(&entry, &entry_size, file) != -1) {
    entry[strcspn(entry, "\n")] = '\0';
    if (entry[0] != '\0') {
      history_push(entry);
    }
  }
  free(entry);
  fclose(file);
}

void lineedit_history_add(const char *line) {
  // Like bash's ignorespace: a leading space keeps a line out of history.
  if (line[0] == '\0' || line[0] == ' ') {
    return;
  }
  if (history_count > 0 && strcmp(history[history_count - 1], line) == 0) {
    return;
  }

  history_push(line);

  if (history_path[0] != '\0') {
    FILE *file = fopen(history_path, "a");
    if (file != NULL) {
      fprintf(file, "%s\n", line);
      fclose(file);
    }
  }
}

int lineedit_history_count(void) {
  return history_count;
}

const char *lineedit_history_get(int index) {
  if (index < 0 || index >= history_count) {
    return NULL;
  }
  return history[index];
}

// ---------------------------------------------------------------------------
// Key reading
// ---------------------------------------------------------------------------

static int read_byte(char *out) {
  while (1) {
    ssize_t result = read(STDIN_FILENO, out, 1);
    if (result == 1) {
      return 1;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return 0;
  }
}

// Escape sequences arrive as ESC + more bytes; a lone ESC key press does
// not. Wait briefly to tell them apart.
static int read_byte_soon(char *out) {
  struct pollfd poller = { STDIN_FILENO, POLLIN, 0 };
  if (poll(&poller, 1, 50) <= 0) {
    return 0;
  }
  return read_byte(out);
}

typedef enum {
  ARROW_NONE,
  ARROW_UP,
  ARROW_DOWN,
  ARROW_LEFT,
  ARROW_RIGHT,
  ARROW_HOME,
  ARROW_END,
  ARROW_DELETE,
  ARROW_WORD_LEFT,
  ARROW_WORD_RIGHT
}ArrowKey;

static ArrowKey read_escape_sequence(void) {
  char first;
  char second;
  if (!read_byte_soon(&first) || !read_byte_soon(&second)) {
    return ARROW_NONE;
  }

  if (first == 'O') {
    if (second == 'H') return ARROW_HOME;
    if (second == 'F') return ARROW_END;
    return ARROW_NONE;
  }
  if (first != '[') {
    return ARROW_NONE;
  }

  switch (second) {
    case 'A': return ARROW_UP;
    case 'B': return ARROW_DOWN;
    case 'C': return ARROW_RIGHT;
    case 'D': return ARROW_LEFT;
    case 'H': return ARROW_HOME;
    case 'F': return ARROW_END;
    default: break;
  }

  if (second >= '0' && second <= '9') {
    // ESC [ n ~   or   ESC [ 1 ; 5 C  (Ctrl+arrow)
    char third;
    if (!read_byte_soon(&third)) {
      return ARROW_NONE;
    }
    if (third == '~') {
      if (second == '1' || second == '7') return ARROW_HOME;
      if (second == '4' || second == '8') return ARROW_END;
      if (second == '3') return ARROW_DELETE;
      return ARROW_NONE;
    }
    if (third == ';') {
      char modifier;
      char direction;
      if (!read_byte_soon(&modifier) || !read_byte_soon(&direction)) {
        return ARROW_NONE;
      }
      if (direction == 'C') return ARROW_WORD_RIGHT;
      if (direction == 'D') return ARROW_WORD_LEFT;
    }
  }
  return ARROW_NONE;
}

static size_t word_start_before(const EditBuffer *line, size_t at) {
  while (at > 0 && line->data[at - 1] == ' ') {
    at--;
  }
  while (at > 0 && line->data[at - 1] != ' ') {
    at--;
  }
  return at;
}

static size_t word_end_after(const EditBuffer *line, size_t at) {
  while (at < line->length && line->data[at] == ' ') {
    at++;
  }
  while (at < line->length && line->data[at] != ' ') {
    at++;
  }
  return at;
}

// ---------------------------------------------------------------------------
// Main editing loop
// ---------------------------------------------------------------------------

char *lineedit_read(const char *prompt) {
  EditBuffer line = { NULL, 0, 0, 0 };
  editbuf_reserve(&line, 0);
  line.data[0] = '\0';

  // Browsing history: index into history, or history_count for the line
  // being typed. `draft` keeps that line while browsing.
  int history_index = history_count;
  char *draft = NULL;

  if (lineedit_raw_on() != 0) {
    // Not a usable terminal after all; fall back to plain reading.
    free(line.data);
    fputs(prompt, stdout);
    fflush(stdout);
    char *fallback = NULL;
    size_t fallback_size = 0;
    if (getline(&fallback, &fallback_size, stdin) == -1) {
      free(fallback);
      return NULL;
    }
    fallback[strcspn(fallback, "\n")] = '\0';
    return fallback;
  }

  refresh_line(prompt, &line);

  while (1) {
    char key;
    if (!read_byte(&key)) {
      // Input closed underneath us: treat it like Ctrl+D.
      lineedit_raw_off();
      free(line.data);
      free(draft);
      return NULL;
    }

    if (key == '\r' || key == '\n') {
      break;
    }

    switch (key) {
      case KEY_CTRL('c'):
        // Abandon the line, like bash.
        if (write(STDOUT_FILENO, "^C\r\n", 4) < 0) {}
        line.length = 0;
        line.cursor = 0;
        line.data[0] = '\0';
        history_index = history_count;
        break;

      case KEY_CTRL('d'):
        if (line.length == 0) {
          lineedit_raw_off();
          free(line.data);
          free(draft);
          return NULL;
        }
        if (line.cursor < line.length) {
          size_t next = next_char(&line, line.cursor);
          size_t count = next - line.cursor;
          line.cursor = next;
          editbuf_delete_before(&line, count);
        }
        break;

      case KEY_BACKSPACE:
      case KEY_CTRL('h'):
        if (line.cursor > 0) {
          editbuf_delete_before(&line, line.cursor - previous_char(&line, line.cursor));
        }
        break;

      case KEY_CTRL('a'):
        line.cursor = 0;
        break;

      case KEY_CTRL('e'):
        line.cursor = line.length;
        break;

      case KEY_CTRL('b'):
        line.cursor = previous_char(&line, line.cursor);
        break;

      case KEY_CTRL('f'):
        line.cursor = next_char(&line, line.cursor);
        break;

      case KEY_CTRL('k'):
        line.length = line.cursor;
        line.data[line.length] = '\0';
        break;

      case KEY_CTRL('u'):
        editbuf_delete_before(&line, line.cursor);
        break;

      case KEY_CTRL('w'):
        editbuf_delete_before(&line, line.cursor - word_start_before(&line, line.cursor));
        break;

      case KEY_CTRL('l'):
        if (write(STDOUT_FILENO, "\x1b[H\x1b[2J", 7) < 0) {}
        break;

      case '\t':
        fzf_on_tab(&line);
        break;

      case KEY_CTRL('r'):
        fzf_on_history(&line);
        break;

      case KEY_CTRL('p'):
      case KEY_CTRL('n'):
      case KEY_ESCAPE: {
        ArrowKey arrow = ARROW_NONE;
        if (key == KEY_CTRL('p')) {
          arrow = ARROW_UP;
        }
        else if (key == KEY_CTRL('n')) {
          arrow = ARROW_DOWN;
        }
        else {
          arrow = read_escape_sequence();
        }

        switch (arrow) {
          case ARROW_LEFT:
            line.cursor = previous_char(&line, line.cursor);
            break;
          case ARROW_RIGHT:
            line.cursor = next_char(&line, line.cursor);
            break;
          case ARROW_HOME:
            line.cursor = 0;
            break;
          case ARROW_END:
            line.cursor = line.length;
            break;
          case ARROW_WORD_LEFT:
            line.cursor = word_start_before(&line, line.cursor);
            break;
          case ARROW_WORD_RIGHT:
            line.cursor = word_end_after(&line, line.cursor);
            break;
          case ARROW_DELETE:
            if (line.cursor < line.length) {
              size_t next = next_char(&line, line.cursor);
              size_t count = next - line.cursor;
              line.cursor = next;
              editbuf_delete_before(&line, count);
            }
            break;
          case ARROW_UP:
            if (history_index > 0) {
              if (history_index == history_count) {
                free(draft);
                draft = strdup(line.data);
              }
              history_index--;
              editbuf_set(&line, history[history_index]);
            }
            break;
          case ARROW_DOWN:
            if (history_index < history_count) {
              history_index++;
              if (history_index == history_count) {
                editbuf_set(&line, draft != NULL ? draft : "");
              }
              else {
                editbuf_set(&line, history[history_index]);
              }
            }
            break;
          case ARROW_NONE:
            break;
        }
        break;
      }

      case ' ':
        // Space right after a command like `cd` or `nvim` opens a picker;
        // if the picker isn't wanted it falls through to a plain space.
        if (line.cursor != line.length || !fzf_on_space(&line)) {
          editbuf_insert(&line, " ");
        }
        break;

      default:
        if ((unsigned char)key >= 32) {
          char text[2] = { key, '\0' };
          editbuf_insert(&line, text);
        }
        break;
    }

    refresh_line(prompt, &line);
  }

  if (write(STDOUT_FILENO, "\r\n", 2) < 0) {}
  lineedit_raw_off();
  free(draft);
  return line.data;
}
