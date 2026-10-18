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
#include "complete.h"
#include "fish.h"
#include "highlight.h"
#include "live.h"
#include "pipehint.h"
#include <sys/ioctl.h>
#include "parser.h"
#include "util.h"

#define HISTORY_MAX 1000

#define KEY_CTRL(k) ((k) & 0x1f)
#define KEY_BACKSPACE 127
#define KEY_ESCAPE 27

static struct termios original_termios;
static int raw_enabled = 0;

static int highlighting = 1;
static int suggestions = 1;
static int private_mode = 0;

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

static size_t previous_codepoint(const EditBuffer *line, size_t at) {
  if (at == 0) {
    return 0;
  }
  at--;
  while (at > 0 && is_continuation(line->data[at])) {
    at--;
  }
  return at;
}

static unsigned codepoint_at(const EditBuffer *line, size_t at) {
  unsigned codepoint = 0;
  if (at < line->length) utf8_decode(line->data + at, line->length - at, &codepoint);
  return codepoint;
}

// One step left or right moves over a whole character, accents and other
// combining marks included (they take no column of their own).
static size_t previous_char(const EditBuffer *line, size_t at) {
  at = previous_codepoint(line, at);
  while (at > 0 && char_columns(codepoint_at(line, at)) == 0) {
    at = previous_codepoint(line, at);
  }
  return at;
}

static size_t next_char(const EditBuffer *line, size_t at) {
  if (at >= line->length) {
    return line->length;
  }
  unsigned codepoint;
  at += utf8_decode(line->data + at, line->length - at, &codepoint);
  while (at < line->length) {
    size_t step = utf8_decode(line->data + at, line->length - at, &codepoint);
    if (char_columns(codepoint) != 0) break;
    at += step;
  }
  return at;
}

// Columns `text` takes on screen (wide characters count two, color codes
// and other escape sequences zero).
static size_t display_width(const char *text, size_t length) {
  return text_columns(text, length);
}

// Where text lands on screen, the way a terminal wraps it: a character
// that doesn't fit at the end of a row moves to the next one (a wide one
// can leave a gap). `column` may end equal to `columns`: the row is full
// and the terminal is waiting to wrap.
static void layout_advance(const char *text, size_t length, int columns, int *row, int *column) {
  for (size_t i = 0;i < length;) {
    if (text[i] == '\x1b') {
      i += escape_length(text + i, length - i);
      continue;
    }
    unsigned codepoint;
    i += utf8_decode(text + i, length - i, &codepoint);
    int width = text[i - 1] == '\t' ? 1 : char_columns(codepoint);
    if (width == 0) continue;
    if (*column + width > columns) {
      (*row)++;
      *column = 0;
    }
    *column += width;
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Below the line: the pipeline hint and the live preview
// ---------------------------------------------------------------------------

static char *preview_text = NULL;     // last preview output
static char *preview_for = NULL;      // the line it belongs to
static int preview_total = 0;
static int preview_failed = 0;
static char preview_note[512];        // "would write this to pids.txt"
static int below_off = 0;             // set while finishing a line

static void preview_clear(void) {
  free(preview_text);
  free(preview_for);
  preview_text = NULL;
  preview_for = NULL;
}

static int terminal_width(void) {
  struct winsize size;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 10) return size.ws_col;
  return 80;
}

// Copies `text` into `out`, keeping color codes, stopping after `width`
// visible columns (tabs become spaces).
static void clip(const char *text, int width, char *out, size_t size) {
  size_t used = 0;
  int columns = 0;
  for (size_t i = 0;text[i] && used < size - 16;i++) {
    if (text[i] == '\x1b' && text[i + 1] == '[') {
      size_t j = i + 2;
      while (text[j] && text[j] != 'm') j++;
      if (!text[j]) break;
      memcpy(out + used, text + i, j - i + 1);
      used += j - i + 1;
      i = j;
      continue;
    }
    if (text[i] == '\x1b') {
      i += escape_length(text + i, strlen(text + i)) - 1;
      continue;
    }
    unsigned codepoint;
    size_t step = utf8_decode(text + i, strlen(text + i), &codepoint);
    int char_width = text[i] == '\t' ? 1 : char_columns(codepoint);
    if (columns + char_width > width) break;
    if (used + step >= size - 16) break;
    columns += char_width;
    if (text[i] == '\t') out[used++] = ' ';
    else {
      memcpy(out + used, text + i, step);
      used += step;
    }
    i += step - 1;
  }
  memcpy(out + used, "\x1b[0m", 5);
  out[used + 4] = '\0';
}

#define MAX_BELOW 40

// The lines to show under the input line. Returns how many.
static int compose_below(const EditBuffer *line, char below[][1024]) {
  if (below_off || line->length == 0) return 0;
  int count = 0;
  int width = terminal_width() - 3;

  if (live_hints_enabled()) {
    char *hint = pipehint_summary(line->data);
    if (hint) {
      // Wrap at the arrows so every step, and the expected output at the
      // end, stays readable (up to 6 lines).
      char row[2048];
      size_t row_used = 0;
      int row_columns = 0;
      int lines_used = 0;
      const char *cursor = hint;
      row_used = snprintf(row, sizeof(row), "\x1b[2m\u21B3 ");
      row_columns = 2;
      while (*cursor && lines_used < 6) {
        // Next piece: up to (and including) the next arrow, or the rest.
        const char *arrow = strstr(cursor + 1, " \u2192 ");
        const char *result = strstr(cursor + 1, "  \u21D2 ");
        const char *end = arrow;
        if (result && (!end || result < end)) end = result;
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        int piece_columns = (int)display_width(cursor, length);
        if (row_columns + piece_columns > width && row_columns > 2) {
          clip(row, width, below[count++], 1024);
          lines_used++;
          row_used = snprintf(row, sizeof(row), "\x1b[2m  ");
          row_columns = 2;
          while (*cursor == ' ') { cursor++; length--; }
          piece_columns = (int)display_width(cursor, length);
        }
        // A step too long for a row of its own wraps between words.
        size_t done = 0;
        while (done < length && lines_used < 6) {
          size_t word = done;
          while (word < length && cursor[word] == ' ') word++;
          while (word < length && cursor[word] != ' ') word++;
          int word_columns = (int)display_width(cursor + done, word - done);
          if (row_columns + word_columns > width && row_columns > 2) {
            clip(row, width, below[count++], 1024);
            lines_used++;
            row_used = snprintf(row, sizeof(row), "\x1b[2m    ");
            row_columns = 4;
            while (done < word && cursor[done] == ' ') done++;
            word_columns = (int)display_width(cursor + done, word - done);
          }
          if (row_used + (word - done) < sizeof(row) - 16) {
            memcpy(row + row_used, cursor + done, word - done);
            row_used += word - done;
            row[row_used] = '\0';
            row_columns += word_columns;
          }
          done = word;
        }
        cursor += length;
      }
      if (row_columns > 2 && lines_used < 6) clip(row, width, below[count++], 1024);
      free(hint);
    }
  }

  if (preview_text != NULL && preview_for != NULL) {
    char header[1024];
    int shown = 0;
    for (const char *c = preview_text;*c;c++) if (*c == '\n') shown++;
    if (preview_text[0] && preview_text[strlen(preview_text) - 1] != '\n') shown++;
    int stale = strcmp(preview_for, line->data) != 0;
    if (preview_failed) snprintf(header, sizeof(header), "\x1b[2;33m\u25B8 live%s:\x1b[0m", stale ? " (updating)" : "");
    else if (preview_total == 0) snprintf(header, sizeof(header), "\x1b[2m\u25B8 live%s: no output\x1b[0m", stale ? " (updating)" : "");
    else if (preview_total > shown) snprintf(header, sizeof(header), "\x1b[2m\u25B8 live%s: first %d of %s%d lines\x1b[0m", stale ? " (updating)" : "", shown, preview_total > 60 ? "more than " : "", preview_total > 60 ? 50 : preview_total);
    else snprintf(header, sizeof(header), "\x1b[2m\u25B8 live%s: %d line%s\x1b[0m", stale ? " (updating)" : "", shown, shown == 1 ? "" : "s");
    if (preview_note[0] && !preview_failed) {
      size_t used = strlen(header);
      snprintf(header + used, sizeof(header) - used, "\x1b[2;36m (%s)\x1b[0m", preview_note);
    }
    clip(header, width, below[count++], 1024);

    const char *cursor = preview_text;
    while (*cursor && count < MAX_BELOW) {
      size_t length = strcspn(cursor, "\n");
      char row[2048];
      snprintf(row, sizeof(row), "%s\u2502 %.*s", preview_failed ? "\x1b[33m" : "\x1b[2m\x1b[0m", (int)(length < 1900 ? length : 1900), cursor);
      clip(row, width, below[count++], 1024);
      cursor += length;
      if (*cursor == '\n') cursor++;
    }
  }
  return count;
}

// Decides whether the line has a fresh preview coming.
static int preview_wanted(const EditBuffer *line) {
  if (!live_enabled() || line->length == 0) {
    return 0;
  }
  if (!live_eligible(line->data, NULL, 0)) {
    preview_clear();
    return 0;
  }
  // A lone echo/ls/cat shows nothing you won't see anyway; previews are
  // for pipelines and for commands that filter or transform.
  if (strpbrk(line->data, "|>;&$`") == NULL) {
    static const char *plain[] = { "echo", "printf", "cat", "ls", "seq", NULL };
    size_t word = strspn(line->data, " ");
    size_t length = strcspn(line->data + word, " ");
    for (int i = 0;plain[i];i++) {
      if (strlen(plain[i]) == length && strncmp(line->data + word, plain[i], length) == 0) {
        preview_clear();
        return 0;
      }
    }
  }
  return preview_for == NULL || strcmp(preview_for, line->data) != 0;
}

static void preview_update(const EditBuffer *line) {
  int total = 0, failed = 0;
  char *output = live_run(line->data, live_lines(), &total, &failed);
  free(preview_text);
  free(preview_for);
  preview_text = output ? output : strdup("(couldn't run the preview)");
  preview_for = strdup(line->data);
  preview_total = total;
  preview_failed = failed || output == NULL;
  snprintf(preview_note, sizeof(preview_note), "%s", live_last_note());
}

// Which row of the input (counting from the prompt's row) the cursor was
// left on by the last redraw, so the next one can start from the top even
// when the line has wrapped onto several rows.
static int cursor_row = 0;
// The last row the input reached in that redraw.
static int last_end_row = 0;

// The right-hand prompt (starship's right_format, RIGHT_FORMAT), or "".
static char right_prompt[2048];

void lineedit_set_right_prompt(const char *text) {
  size_t kept = 0;
  // One row only: anything after a newline is dropped.
  for (size_t i = 0;text != NULL && text[i] && text[i] != '\n' && kept < sizeof(right_prompt) - 1;i++) {
    if (text[i] != '\r') right_prompt[kept++] = text[i];
  }
  right_prompt[kept] = '\0';
}

// Draws the prompt and the line (highlighted, possibly wrapping over
// several rows), the grey autosuggestion, the right prompt, and the
// hint/preview below.
static void refresh_line_with(const char *prompt, const EditBuffer *line, const char *suggestion) {
  char *shown = highlighting ? highlight_line(line->data) : NULL;
  const char *body = shown ? shown : line->data;
  if (suggestion == NULL || line->cursor != line->length) {
    suggestion = "";
  }
  int columns = terminal_width();

  static char below[MAX_BELOW][1024];
  int below_count = compose_below(line, below);
  size_t frame_size = strlen(prompt) + strlen(body) + strlen(suggestion) + strlen(right_prompt) + 256;
  for (int i = 0;i < below_count;i++) frame_size += strlen(below[i]) + 8;
  char *frame = malloc(frame_size);
  if (frame == NULL) {
    free(shown);
    return;
  }

  // Where everything lands, worked out the way the terminal will wrap it.
  int end_row = 0, end_column = 0;
  layout_advance(prompt, strlen(prompt), columns, &end_row, &end_column);
  int target_row = end_row, target_column = end_column;
  layout_advance(line->data, line->cursor, columns, &target_row, &target_column);
  if (line->cursor < line->length) {
    // The cursor sits on the next character, which may itself wrap.
    unsigned codepoint;
    utf8_decode(line->data + line->cursor, line->length - line->cursor, &codepoint);
    int width = char_columns(codepoint);
    if (target_column + (width > 0 ? width : 1) > columns) {
      target_row++;
      target_column = 0;
    }
  }
  else if (target_column >= columns) {
    target_row++;
    target_column = 0;
  }
  layout_advance(line->data, line->length, columns, &end_row, &end_column);
  layout_advance(suggestion, strlen(suggestion), columns, &end_row, &end_column);

  int used = 0;
  // Back to the prompt's first row, and clear everything from there down.
  if (cursor_row > 0) {
    used += snprintf(frame + used, frame_size - used, "\x1b[%dA", cursor_row);
  }
  used += snprintf(frame + used, frame_size - used, "\r\x1b[J%s%s", prompt, body);
  if (suggestion[0]) {
    used += snprintf(frame + used, frame_size - used, "\x1b[90m%s\x1b[0m", suggestion);
  }

  // The right prompt: on the input row, while everything fits on one row
  // with a gap; it steps aside as soon as the line gets long.
  int right_width = (int)display_width(right_prompt, strlen(right_prompt));
  if (right_width > 0 && end_row == 0 && end_column + 2 + right_width < columns) {
    used += snprintf(frame + used, frame_size - used, "\x1b[%dG%s\x1b[0m", columns - right_width, right_prompt);
  }

  // Exactly filling the last row leaves the terminal waiting to wrap;
  // move down so the arithmetic below holds.
  // (The input itself still ends on the full row: that's where Enter
  // leaves from.)
  last_end_row = end_row;
  if (end_column >= columns) {
    used += snprintf(frame + used, frame_size - used, "\n\r");
    end_row++;
    end_column = 0;
  }
  int row = end_row;

  if (below_count > 0) {
    // Make room first so drawing below never scrolls the screen under us,
    // then come back up and draw.
    for (int i = 0;i < below_count;i++) frame[used++] = '\n';
    used += snprintf(frame + used, frame_size - used, "\x1b[%dA", below_count);
    for (int i = 0;i < below_count;i++) {
      used += snprintf(frame + used, frame_size - used, "\n\r  %s", below[i]);
    }
    row = end_row + below_count;
  }

  // Put the cursor where it belongs.
  if (row > target_row) {
    used += snprintf(frame + used, frame_size - used, "\x1b[%dA", row - target_row);
  }
  used += snprintf(frame + used, frame_size - used, "\r");
  if (target_column > 0) {
    used += snprintf(frame + used, frame_size - used, "\x1b[%dC", target_column);
  }
  cursor_row = target_row;

  if (write(STDOUT_FILENO, frame, used) < 0) {
    // Nothing useful to do if the terminal went away.
  }
  free(frame);
  free(shown);
}

static void refresh_line(const char *prompt, const EditBuffer *line) {
  refresh_line_with(prompt, line, NULL);
}

void lineedit_configure_defaults(FILE *file) {
  fprintf(file, "# Line editing: color the command as you type (red = not found),\n");
  fprintf(file, "# and suggest the rest from history in grey (Right arrow accepts).\n");
  fprintf(file, "HIGHLIGHT=%d\n", highlighting);
  fprintf(file, "AUTOSUGGEST=%d\n", suggestions);
}

int lineedit_configure(const char *key, const char *value) {
  if (strcmp(key, "HIGHLIGHT") == 0) highlighting = atoi(value) != 0;
  else if (strcmp(key, "AUTOSUGGEST") == 0) suggestions = atoi(value) != 0;
  else return 0;
  return 1;
}

void lineedit_set_private(int on) {
  private_mode = on;
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
  if (private_mode) {
    return;
  }
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

  if (history_path[0] != '\0' && !private_mode) {
    FILE *file = fopen(history_path, "a");
    if (file != NULL) {
      fprintf(file, "%s\n", line);
      fclose(file);
    }
  }
}

static void history_rewrite(void) {
  if (history_path[0] == '\0' || private_mode) {
    return;
  }
  FILE *file = fopen(history_path, "w");
  if (file == NULL) {
    return;
  }
  for (int i = 0;i < history_count;i++) {
    fprintf(file, "%s\n", history[i]);
  }
  fclose(file);
}

int lineedit_history_delete(const char *text) {
  int removed = 0;
  int kept = 0;
  for (int i = 0;i < history_count;i++) {
    if (strstr(history[i], text) != NULL) {
      free(history[i]);
      removed++;
    }
    else {
      history[kept++] = history[i];
    }
  }
  history_count = kept;
  if (removed) {
    history_rewrite();
  }
  return removed;
}

void lineedit_history_clear(void) {
  for (int i = 0;i < history_count;i++) {
    free(history[i]);
  }
  history_count = 0;
  history_rewrite();
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

static int input_pending(void) {
  struct pollfd poller = { STDIN_FILENO, POLLIN, 0 };
  return poll(&poller, 1, 0) > 0;
}

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
  ARROW_WORD_RIGHT,
  ARROW_ALT_LEFT,
  ARROW_ALT_RIGHT,
  ARROW_ALT_DOT,
  ARROW_ALT_F,
  ARROW_ALT_B
}ArrowKey;

static ArrowKey read_escape_sequence(void) {
  char first;
  char second;
  if (!read_byte_soon(&first)) {
    return ARROW_NONE;
  }
  // Alt+key arrives as ESC followed by the key.
  if (first != '[' && first != 'O') {
    if (first == '.' || first == '_') return ARROW_ALT_DOT;
    if (first == 'f') return ARROW_ALT_F;
    if (first == 'b') return ARROW_ALT_B;
    return ARROW_NONE;
  }
  if (!read_byte_soon(&second)) {
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
      // ;3 is Alt, ;5 is Ctrl
      if (direction == 'C') return modifier == '3' ? ARROW_ALT_RIGHT : ARROW_WORD_RIGHT;
      if (direction == 'D') return modifier == '3' ? ARROW_ALT_LEFT : ARROW_WORD_LEFT;
    }
  }
  return ARROW_NONE;
}

// The rest of the newest history entry that starts with what's typed.
static const char *suggestion_for(const EditBuffer *line) {
  if (!suggestions || line->length == 0 || line->cursor != line->length) {
    return NULL;
  }
  for (int i = history_count - 1;i >= 0;i--) {
    if (strncmp(history[i], line->data, line->length) == 0 && history[i][line->length] != '\0') {
      return history[i] + line->length;
    }
  }
  return NULL;
}

// `gco` + space -> `git checkout` when gco is an abbreviation in command
// position.
static void expand_abbreviation(EditBuffer *line) {
  if (line->cursor != line->length || line->length == 0) {
    return;
  }
  size_t start = line->length;
  while (start > 0 && line->data[start - 1] != ' ' && !strchr("|;&(", line->data[start - 1])) {
    start--;
  }
  size_t before = start;
  while (before > 0 && line->data[before - 1] == ' ') {
    before--;
  }
  if (before > 0 && !strchr("|;&(", line->data[before - 1])) {
    return;
  }
  const char *expansion = abbr_get(line->data + start);
  if (expansion == NULL) {
    return;
  }
  editbuf_delete_before(line, line->length - start);
  editbuf_insert(line, expansion);
}

// Alt+. : the last argument of the previous command.
static void insert_last_argument(EditBuffer *line) {
  if (history_count == 0) {
    return;
  }
  ArgList words;
  arglist_init(&words);
  if (parser_split_words(history[history_count - 1], &words) > 0) {
    char *quoted = shell_quote(words.items[words.count - 1]);
    if (quoted) {
      editbuf_insert(line, quoted);
      free(quoted);
    }
  }
  arglist_free(&words);
}

static int history_matches(int index, const char *search) {
  return search == NULL || strstr(history[index], search) != NULL;
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

// Writes the upper lines of a multi-line prompt. Only the last line is
// redrawn while typing.
static void draw_prompt_head(const char *prompt, const char *tail) {
  if (tail > prompt) {
    if (write(STDOUT_FILENO, prompt, tail - prompt) < 0) {}
  }
}

char *lineedit_read(const char *full_prompt) {
  return lineedit_read_initial(full_prompt, "");
}

char *lineedit_read_initial(const char *full_prompt, const char *initial) {
  const char *last_newline = strrchr(full_prompt, '\n');
  const char *prompt = last_newline ? last_newline + 1 : full_prompt;
  EditBuffer line = { NULL, 0, 0, 0 };
  editbuf_reserve(&line, 0);
  line.data[0] = '\0';
  editbuf_insert(&line, initial);

  // Browsing history: index into history, or history_count for the line
  // being typed. `draft` keeps that line while browsing.
  int history_index = history_count;
  char *draft = NULL;
  // Up arrow with text typed searches history for that text.
  char *search = NULL;

  if (lineedit_raw_on() != 0) {
    // Not a usable terminal after all; fall back to plain reading.
    free(line.data);
    fputs(full_prompt, stdout);
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

  draw_prompt_head(full_prompt, prompt);
  refresh_line(prompt, &line);

  int preview_pending = 0;
  below_off = 0;
  cursor_row = 0;
  preview_clear();

  while (1) {
    // After a pause in typing, refresh the live preview.
    if (preview_pending) {
      struct pollfd poller = { STDIN_FILENO, POLLIN, 0 };
      if (poll(&poller, 1, live_delay_ms()) == 0) {
        preview_update(&line);
        preview_pending = 0;
        refresh_line_with(prompt, &line, suggestion_for(&line));
      }
    }
    char key;
    if (!read_byte(&key)) {
      // Input closed underneath us: treat it like Ctrl+D.
      lineedit_raw_off();
      free(line.data);
      free(draft);
      free(search);
      return NULL;
    }

    if (key == '\r' || key == '\n') {
      expand_abbreviation(&line);
      // The hint and preview go away before the command runs.
      below_off = 1;
      refresh_line(prompt, &line);
      preview_clear();
      break;
    }

    // Any key other than up/down ends a history search.
    if (key != KEY_ESCAPE && key != KEY_CTRL('p') && key != KEY_CTRL('n')) {
      free(search);
      search = NULL;
    }

    switch (key) {
      case KEY_CTRL('c'):
        // Abandon the line, like bash.
        below_off = 1;
        line.cursor = line.length;
        refresh_line(prompt, &line);
        below_off = 0;
        preview_clear();
        cursor_row = 0;
        if (write(STDOUT_FILENO, "^C\r\n", 4) < 0) {}
        draw_prompt_head(full_prompt, prompt);
        line.length = 0;
        line.cursor = 0;
        line.data[0] = '\0';
        history_index = history_count;
        break;

      case KEY_CTRL('d'):
        if (line.length == 0) {
          preview_clear();
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
        if (line.cursor == line.length && suggestion_for(&line)) {
          editbuf_insert(&line, suggestion_for(&line));
        }
        line.cursor = line.length;
        break;

      case KEY_CTRL('b'):
        line.cursor = previous_char(&line, line.cursor);
        break;

      case KEY_CTRL('f'):
        if (line.cursor == line.length && suggestion_for(&line)) {
          editbuf_insert(&line, suggestion_for(&line));
        }
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
        cursor_row = 0;
        draw_prompt_head(full_prompt, prompt);
        break;

      case '\t':
        // fzf picker when available, classic completion otherwise.
        if (!fzf_on_tab(&line)) {
          complete_basic(&line);
        }
        break;

      case KEY_CTRL('z'):
        // Nothing is running at the prompt, so there's nothing to suspend.
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
            // At the end of the line, Right accepts the grey suggestion.
            if (line.cursor == line.length && suggestion_for(&line)) {
              editbuf_insert(&line, suggestion_for(&line));
            }
            line.cursor = next_char(&line, line.cursor);
            break;
          case ARROW_ALT_RIGHT:
          case ARROW_ALT_F:
            if (line.cursor == line.length && suggestion_for(&line)) {
              // Accept one more word of the suggestion.
              const char *rest = suggestion_for(&line);
              size_t take = 0;
              while (rest[take] == ' ') take++;
              while (rest[take] && rest[take] != ' ') take++;
              char *word = strndup(rest, take);
              editbuf_insert(&line, word);
              free(word);
            }
            else if (line.length == 0 && arrow == ARROW_ALT_RIGHT) {
              // On an empty line: forward in folder history (like fish).
              free(draft);
              free(search);
              if (write(STDOUT_FILENO, "\r\n", 2) < 0) {}
              lineedit_raw_off();
              free(line.data);
              return strdup(" nextd");
            }
            else {
              line.cursor = word_end_after(&line, line.cursor);
            }
            break;
          case ARROW_ALT_LEFT:
          case ARROW_ALT_B:
            if (line.length == 0 && arrow == ARROW_ALT_LEFT) {
              free(draft);
              free(search);
              if (write(STDOUT_FILENO, "\r\n", 2) < 0) {}
              lineedit_raw_off();
              free(line.data);
              return strdup(" prevd");
            }
            line.cursor = word_start_before(&line, line.cursor);
            break;
          case ARROW_ALT_DOT:
            insert_last_argument(&line);
            break;
          case ARROW_HOME:
            line.cursor = 0;
            break;
          case ARROW_END:
            if (line.cursor == line.length && suggestion_for(&line)) {
              editbuf_insert(&line, suggestion_for(&line));
            }
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
          case ARROW_UP: {
            if (history_index == history_count) {
              free(draft);
              draft = strdup(line.data);
              free(search);
              search = line.length > 0 ? strdup(line.data) : NULL;
            }
            // Older entries containing what was typed (skipping repeats).
            int found = history_index - 1;
            while (found >= 0 && (!history_matches(found, search) ||
                   strcmp(history[found], line.data) == 0)) {
              found--;
            }
            if (found >= 0) {
              history_index = found;
              editbuf_set(&line, history[history_index]);
            }
            break;
          }
          case ARROW_DOWN: {
            int found = history_index + 1;
            while (found < history_count && (!history_matches(found, search) ||
                   strcmp(history[found], line.data) == 0)) {
              found++;
            }
            if (found < history_count) {
              history_index = found;
              editbuf_set(&line, history[history_index]);
            }
            else if (history_index < history_count) {
              history_index = history_count;
              editbuf_set(&line, draft != NULL ? draft : "");
            }
            break;
          }
          case ARROW_NONE:
            break;
        }
        break;
      }

      case ' ':
        expand_abbreviation(&line);
        // Space right after a command like `cd` or `nvim` opens a picker;
        // if the picker isn't wanted it falls through to a plain space.
        // Not while pasting or typing ahead: more keys already waiting
        // means the path is on its way.
        if (line.cursor != line.length || input_pending() || !fzf_on_space(&line)) {
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

    preview_pending = preview_wanted(&line);
    refresh_line_with(prompt, &line, suggestion_for(&line));
  }

  // Leave the cursor after the last row of the input.
  {
    int last_row = last_end_row;
    if (last_row > cursor_row) {
      char move[32];
      int n = snprintf(move, sizeof(move), "\x1b[%dB", last_row - cursor_row);
      if (write(STDOUT_FILENO, move, n) < 0) {}
    }
  }
  // A line that exactly filled its last row already moved down.
  if (cursor_row > last_end_row) {
    if (write(STDOUT_FILENO, "\r", 1) < 0) {}
  }
  else if (write(STDOUT_FILENO, "\r\n", 2) < 0) {}
  cursor_row = 0;
  below_off = 0;
  // The right prompt belongs to this line only (not to "..." continuations).
  right_prompt[0] = '\0';
  free(search);
  lineedit_raw_off();
  free(draft);
  return line.data;
}
