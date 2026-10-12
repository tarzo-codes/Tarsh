#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <limits.h>
#include <sys/utsname.h>

#include "theme.h"
#include "config.h"
#include "util.h"

#define MAX_SEGMENTS 40
#define SMALL_TEXT 64
#define LARGE_TEXT 160

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef enum {
  COLOR_NONE,
  COLOR_BASIC,   // the 16 terminal colors, value 0-15
  COLOR_INDEX,   // 256-color palette, value 0-255
  COLOR_RGB,     // #rrggbb
  COLOR_AUTO     // languages segment: the language's own color
}ColorKind;

typedef struct {
  ColorKind kind;
  int value;
  unsigned char r, g, b;
}Color;

typedef enum {
  SEG_INVALID,
  SEG_TEXT, SEG_USER, SEG_HOST, SEG_USERHOST, SEG_CWD, SEG_GIT, SEG_LANGS,
  SEG_VENV, SEG_STATUS, SEG_DURATION, SEG_TIME, SEG_JOBS, SEG_SHELL, SEG_OS,
  SEG_SYMBOL, SEG_NEWLINE
}SegmentType;

typedef enum { SEP_POWERLINE, SEP_ROUND, SEP_SLANT, SEP_PLAIN, SEP_NONE, SEP_CUSTOM }Separator;
typedef enum { GLYPHS_NERD, GLYPHS_UNICODE, GLYPHS_ASCII }Glyphs;
typedef enum { COLORS_AUTO, COLORS_TRUE, COLORS_256, COLORS_16 }ColorMode;

typedef struct {
  SegmentType type;
  Color fg, bg, error_fg, error_bg;
  int bold, italic;
  char icon[SMALL_TEXT];      // "auto", "none", or the glyph itself
  char prefix[SMALL_TEXT];
  char suffix[SMALL_TEXT];
  char text[LARGE_TEXT];      // text / symbol: what to show
  char error_text[SMALL_TEXT];// symbol: what to show after a failure
  char format[SMALL_TEXT];    // time: strftime format
  int depth;                  // cwd: folders to show (0 = all)
  int dirty;                  // git: run `git status` for changes
  int counts;                 // git: +staged ~modified ?untracked
  int ahead_behind;           // git: ⇡1 ⇣2
  int version;                // langs: show versions
  int show_success;           // status: show ✔ when it worked
  double min;                 // duration: hide below this many seconds
}Segment;

struct Theme {
  char name[SMALL_TEXT];
  int enabled;
  Separator separator;
  char separator_char[16];
  Glyphs glyphs;
  ColorMode colors;
  int blank_line;
  Segment segments[MAX_SEGMENTS];
  int count;
};

// ---------------------------------------------------------------------------
// String builder
// ---------------------------------------------------------------------------

typedef struct {
  char *text;
  size_t length;
  size_t capacity;
}Buf;

static void buf_add(Buf *buf, const char *text) {
  size_t length = strlen(text);
  if (buf->length + length + 1 > buf->capacity) {
    size_t capacity = buf->capacity ? buf->capacity : 256;
    while (buf->length + length + 1 > capacity) {
      capacity *= 2;
    }
    char *grown = realloc(buf->text, capacity);
    if (grown == NULL) {
      return;
    }
    buf->text = grown;
    buf->capacity = capacity;
  }
  memcpy(buf->text + buf->length, text, length + 1);
  buf->length += length;
}

static void buf_addf(Buf *buf, const char *format, ...) {
  char small[512];
  va_list args;
  va_start(args, format);
  vsnprintf(small, sizeof(small), format, args);
  va_end(args);
  buf_add(buf, small);
}

static char *buf_take(Buf *buf) {
  if (buf->text == NULL) {
    return strdup("");
  }
  return buf->text;
}

// ---------------------------------------------------------------------------
// Colors
// ---------------------------------------------------------------------------

static const char *basic_names[16] = {
  "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white",
  "bright_black", "bright_red", "bright_green", "bright_yellow",
  "bright_blue", "bright_magenta", "bright_cyan", "bright_white"
};

// xterm's defaults, used to convert colors down for simpler terminals.
static const unsigned char basic_rgb[16][3] = {
  {0, 0, 0}, {205, 0, 0}, {0, 205, 0}, {205, 205, 0}, {0, 0, 238}, {205, 0, 205},
  {0, 205, 205}, {229, 229, 229}, {127, 127, 127}, {255, 0, 0}, {0, 255, 0},
  {255, 255, 0}, {92, 92, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}
};

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Returns 0 and fills *color, or -1 if `text` isn't a color.
static int parse_color(const char *text, Color *color, int allow_auto) {
  memset(color, 0, sizeof(*color));
  if (text[0] == '\0' || strcmp(text, "none") == 0 || strcmp(text, "default") == 0) {
    color->kind = COLOR_NONE;
    return 0;
  }
  if (allow_auto && strcmp(text, "auto") == 0) {
    color->kind = COLOR_AUTO;
    return 0;
  }
  for (int i = 0;i < 16;i++) {
    if (strcmp(text, basic_names[i]) == 0) {
      color->kind = COLOR_BASIC;
      color->value = i;
      return 0;
    }
  }
  if (text[0] == '#') {
    size_t length = strlen(text + 1);
    int digits[6];
    if (length == 3 || length == 6) {
      for (size_t i = 0;i < length;i++) {
        digits[i] = hex_digit(text[1 + i]);
        if (digits[i] < 0) return -1;
      }
      color->kind = COLOR_RGB;
      if (length == 3) {
        color->r = digits[0] * 17; color->g = digits[1] * 17; color->b = digits[2] * 17;
      }
      else {
        color->r = digits[0] * 16 + digits[1];
        color->g = digits[2] * 16 + digits[3];
        color->b = digits[4] * 16 + digits[5];
      }
      return 0;
    }
    return -1;
  }
  char *end;
  long index = strtol(text, &end, 10);
  if (*end == '\0' && index >= 0 && index <= 255) {
    color->kind = COLOR_INDEX;
    color->value = (int)index;
    return 0;
  }
  return -1;
}

static Color rgb(unsigned char r, unsigned char g, unsigned char b) {
  Color color = { COLOR_RGB, 0, r, g, b };
  return color;
}

static void index_to_rgb(int index, unsigned char out[3]) {
  if (index < 16) {
    memcpy(out, basic_rgb[index], 3);
  }
  else if (index < 232) {
    static const unsigned char steps[6] = { 0, 95, 135, 175, 215, 255 };
    index -= 16;
    out[0] = steps[index / 36];
    out[1] = steps[(index / 6) % 6];
    out[2] = steps[index % 6];
  }
  else {
    unsigned char level = (unsigned char)(8 + (index - 232) * 10);
    out[0] = out[1] = out[2] = level;
  }
}

static int nearest_basic(const unsigned char c[3]) {
  int best = 0;
  long best_distance = -1;
  for (int i = 0;i < 16;i++) {
    long dr = c[0] - basic_rgb[i][0], dg = c[1] - basic_rgb[i][1], db = c[2] - basic_rgb[i][2];
    long distance = dr * dr + dg * dg + db * db;
    if (best_distance < 0 || distance < best_distance) {
      best_distance = distance;
      best = i;
    }
  }
  return best;
}

static int rgb_to_256(const unsigned char c[3]) {
  // Grays have their own finer ramp.
  if (c[0] == c[1] && c[1] == c[2]) {
    if (c[0] < 8) return 16;
    if (c[0] > 248) return 231;
    return 232 + (c[0] - 8) / 10;
  }
  int r = (c[0] < 48) ? 0 : (c[0] < 115) ? 1 : (c[0] - 35) / 40;
  int g = (c[1] < 48) ? 0 : (c[1] < 115) ? 1 : (c[1] - 35) / 40;
  int b = (c[2] < 48) ? 0 : (c[2] < 115) ? 1 : (c[2] - 35) / 40;
  return 16 + 36 * r + 6 * g + b;
}

static ColorMode detect_color_mode(void) {
  const char *colorterm = getenv("COLORTERM");
  if (colorterm != NULL && (strcmp(colorterm, "truecolor") == 0 || strcmp(colorterm, "24bit") == 0)) {
    return COLORS_TRUE;
  }
  const char *term = getenv("TERM");
  if (term != NULL && strstr(term, "256") != NULL) {
    return COLORS_256;
  }
  if (term != NULL && (strstr(term, "kitty") || strstr(term, "alacritty") || strstr(term, "wezterm"))) {
    return COLORS_TRUE;
  }
  return COLORS_256;
}

// Appends the SGR parameters for one color (";38;5;208" etc).
static void add_color_code(Buf *out, Color color, int background, ColorMode mode) {
  unsigned char c[3];
  switch (color.kind) {
    case COLOR_NONE:
    case COLOR_AUTO:
      return;
    case COLOR_BASIC:
      buf_addf(out, ";%d", (color.value < 8 ? 30 + color.value : 82 + color.value) + (background ? 10 : 0));
      return;
    case COLOR_INDEX:
      if (mode == COLORS_16) {
        index_to_rgb(color.value, c);
        color.value = nearest_basic(c);
        buf_addf(out, ";%d", (color.value < 8 ? 30 + color.value : 82 + color.value) + (background ? 10 : 0));
        return;
      }
      buf_addf(out, ";%d;5;%d", background ? 48 : 38, color.value);
      return;
    case COLOR_RGB:
      c[0] = color.r; c[1] = color.g; c[2] = color.b;
      if (mode == COLORS_TRUE) {
        buf_addf(out, ";%d;2;%d;%d;%d", background ? 48 : 38, c[0], c[1], c[2]);
      }
      else if (mode == COLORS_256) {
        buf_addf(out, ";%d;5;%d", background ? 48 : 38, rgb_to_256(c));
      }
      else {
        int basic = nearest_basic(c);
        buf_addf(out, ";%d", (basic < 8 ? 30 + basic : 82 + basic) + (background ? 10 : 0));
      }
      return;
  }
}

static void add_style(Buf *out, Color fg, Color bg, int bold, int italic, ColorMode mode) {
  buf_add(out, "\x1b[0");
  if (bold) buf_add(out, ";1");
  if (italic) buf_add(out, ";3");
  add_color_code(out, fg, 0, mode);
  add_color_code(out, bg, 1, mode);
  buf_add(out, "m");
}

static int same_color(Color a, Color b) {
  if (a.kind != b.kind) return 0;
  if (a.kind == COLOR_RGB) return a.r == b.r && a.g == b.g && a.b == b.b;
  return a.value == b.value;
}

static void color_rgb(Color color, unsigned char out[3]) {
  if (color.kind == COLOR_RGB) {
    out[0] = color.r; out[1] = color.g; out[2] = color.b;
  }
  else if (color.kind == COLOR_BASIC || color.kind == COLOR_INDEX) {
    index_to_rgb(color.value, out);
  }
  else {
    out[0] = out[1] = out[2] = 0;
  }
}

// Black or white, whichever reads better on `background`.
static Color readable_on(Color background) {
  unsigned char c[3];
  color_rgb(background, c);
  double luminance = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
  return luminance > 150 ? rgb(20, 20, 20) : rgb(250, 250, 250);
}

// ---------------------------------------------------------------------------
// Segment catalog
// ---------------------------------------------------------------------------

static const struct {
  const char *name;
  SegmentType type;
  // default icons for nerd / unicode / ascii glyphs
  const char *icons[3];
}segment_types[] = {
  { "text",     SEG_TEXT,     { "", "", "" } },
  { "user",     SEG_USER,     { "", "", "" } },
  { "host",     SEG_HOST,     { "", "", "" } },
  { "userhost", SEG_USERHOST, { "", "", "" } },
  { "cwd",      SEG_CWD,      { "", "", "" } },
  { "git",      SEG_GIT,      { "", "⎇", "git:" } },
  { "langs",    SEG_LANGS,    { "", "", "" } },
  { "venv",     SEG_VENV,     { "", "", "venv:" } },
  { "status",   SEG_STATUS,   { "", "✘", "x" } },
  { "duration", SEG_DURATION, { "", "", "took" } },
  { "time",     SEG_TIME,     { "", "", "" } },
  { "jobs",     SEG_JOBS,     { "", "&", "&" } },
  { "shell",    SEG_SHELL,    { "", "", "" } },
  { "os",       SEG_OS,       { "", "", "" } },
  { "symbol",   SEG_SYMBOL,   { "", "", "" } },
  { "newline",  SEG_NEWLINE,  { "", "", "" } },
  { NULL, SEG_INVALID, { "", "", "" } }
};

static int type_index(SegmentType type) {
  for (int i = 0;segment_types[i].name != NULL;i++) {
    if (segment_types[i].type == type) return i;
  }
  return 0;
}

static const char *icon_for(const Segment *segment, Glyphs glyphs) {
  if (strcmp(segment->icon, "none") == 0) {
    return "";
  }
  if (strcmp(segment->icon, "auto") != 0) {
    return segment->icon;
  }
  return segment_types[type_index(segment->type)].icons[glyphs];
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

static void warn(ArgList *warnings, int line, const char *format, ...) {
  if (warnings == NULL) {
    return;
  }
  char message[256];
  int used = snprintf(message, sizeof(message), "line %d: ", line);
  va_list args;
  va_start(args, format);
  vsnprintf(message + used, sizeof(message) - used, format, args);
  va_end(args);
  arglist_append(warnings, message);
}

static void segment_defaults(Segment *segment) {
  memset(segment, 0, sizeof(*segment));
  snprintf(segment->icon, sizeof(segment->icon), "auto");
  snprintf(segment->format, sizeof(segment->format), "%%H:%%M");
  segment->depth = 3;
  segment->dirty = 1;
  segment->counts = 0;
  segment->ahead_behind = 1;
  segment->min = 2.0;
}

static void theme_defaults(Theme *theme) {
  memset(theme, 0, sizeof(*theme));
  snprintf(theme->name, sizeof(theme->name), "custom");
  theme->enabled = 1;
  theme->separator = SEP_POWERLINE;
  theme->glyphs = GLYPHS_NERD;
  theme->colors = COLORS_AUTO;
}

static int parse_flag(const char *value) {
  return strcmp(value, "1") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "true") == 0 ||
         strcmp(value, "on") == 0;
}

static void copy_text(char *target, size_t size, const char *value) {
  snprintf(target, size, "%s", value);
}

static void apply_theme_key(Theme *theme, const char *key, const char *value, int line, ArgList *warnings) {
  if (strcmp(key, "name") == 0) {
    copy_text(theme->name, sizeof(theme->name), value);
  }
  else if (strcmp(key, "enabled") == 0) {
    theme->enabled = parse_flag(value);
  }
  else if (strcmp(key, "separator") == 0) {
    static const char *names[] = { "powerline", "round", "slant", "plain", "none", "custom" };
    int found = 0;
    for (int i = 0;i < 6;i++) {
      if (strcmp(value, names[i]) == 0) {
        theme->separator = (Separator)i;
        found = 1;
      }
    }
    if (!found) warn(warnings, line, "separator '%s' should be powerline, round, slant, plain, none or custom", value);
  }
  else if (strcmp(key, "separator_char") == 0) {
    copy_text(theme->separator_char, sizeof(theme->separator_char), value);
  }
  else if (strcmp(key, "glyphs") == 0) {
    if (strcmp(value, "nerd") == 0) theme->glyphs = GLYPHS_NERD;
    else if (strcmp(value, "unicode") == 0) theme->glyphs = GLYPHS_UNICODE;
    else if (strcmp(value, "ascii") == 0) theme->glyphs = GLYPHS_ASCII;
    else warn(warnings, line, "glyphs '%s' should be nerd, unicode or ascii", value);
  }
  else if (strcmp(key, "colors") == 0) {
    if (strcmp(value, "auto") == 0) theme->colors = COLORS_AUTO;
    else if (strcmp(value, "truecolor") == 0) theme->colors = COLORS_TRUE;
    else if (strcmp(value, "256") == 0) theme->colors = COLORS_256;
    else if (strcmp(value, "16") == 0) theme->colors = COLORS_16;
    else warn(warnings, line, "colors '%s' should be auto, truecolor, 256 or 16", value);
  }
  else if (strcmp(key, "blank_line") == 0) {
    theme->blank_line = parse_flag(value);
  }
  else {
    warn(warnings, line, "unknown theme setting '%s'", key);
  }
}

static void apply_color(Color *target, const char *key, const char *value, int allow_auto,
                        int line, ArgList *warnings) {
  if (parse_color(value, target, allow_auto) != 0) {
    warn(warnings, line, "%s '%s' isn't a color (try red, bright_blue, 208 or #ff8800)", key, value);
  }
}

static void apply_segment_key(Segment *segment, const char *key, const char *value, int line, ArgList *warnings) {
  int is_langs = segment->type == SEG_LANGS;
  if (strcmp(key, "type") == 0) {
    segment->type = SEG_INVALID;
    for (int i = 0;segment_types[i].name != NULL;i++) {
      if (strcmp(segment_types[i].name, value) == 0) {
        segment->type = segment_types[i].type;
      }
    }
    if (segment->type == SEG_INVALID) {
      warn(warnings, line, "unknown segment type '%s'", value);
    }
    if (segment->type == SEG_SYMBOL && segment->text[0] == '\0') {
      copy_text(segment->text, sizeof(segment->text), "auto");
    }
  }
  else if (strcmp(key, "fg") == 0) apply_color(&segment->fg, key, value, is_langs, line, warnings);
  else if (strcmp(key, "bg") == 0) apply_color(&segment->bg, key, value, is_langs, line, warnings);
  else if (strcmp(key, "error_fg") == 0) apply_color(&segment->error_fg, key, value, 0, line, warnings);
  else if (strcmp(key, "error_bg") == 0) apply_color(&segment->error_bg, key, value, 0, line, warnings);
  else if (strcmp(key, "bold") == 0) segment->bold = parse_flag(value);
  else if (strcmp(key, "italic") == 0) segment->italic = parse_flag(value);
  else if (strcmp(key, "icon") == 0) copy_text(segment->icon, sizeof(segment->icon), value[0] ? value : "none");
  else if (strcmp(key, "prefix") == 0) copy_text(segment->prefix, sizeof(segment->prefix), value);
  else if (strcmp(key, "suffix") == 0) copy_text(segment->suffix, sizeof(segment->suffix), value);
  else if (strcmp(key, "text") == 0) copy_text(segment->text, sizeof(segment->text), value);
  else if (strcmp(key, "error_text") == 0) copy_text(segment->error_text, sizeof(segment->error_text), value);
  else if (strcmp(key, "format") == 0) copy_text(segment->format, sizeof(segment->format), value);
  else if (strcmp(key, "depth") == 0) segment->depth = atoi(value);
  else if (strcmp(key, "dirty") == 0) segment->dirty = parse_flag(value);
  else if (strcmp(key, "counts") == 0) segment->counts = parse_flag(value);
  else if (strcmp(key, "ahead_behind") == 0) segment->ahead_behind = parse_flag(value);
  else if (strcmp(key, "version") == 0) segment->version = parse_flag(value);
  else if (strcmp(key, "show_success") == 0) segment->show_success = parse_flag(value);
  else if (strcmp(key, "min") == 0) segment->min = atof(value);
  else warn(warnings, line, "unknown segment setting '%s'", key);
}

// Splits off the next line (blank lines included); NULL at the end.
static char *next_line(char **cursor) {
  if (*cursor == NULL || **cursor == '\0') {
    return NULL;
  }
  char *line = *cursor;
  char *newline = strchr(line, '\n');
  if (newline != NULL) {
    *newline = '\0';
    *cursor = newline + 1;
  }
  else {
    *cursor = line + strlen(line);
  }
  return line;
}

static char *trim(char *text) {
  while (*text == ' ' || *text == '\t') text++;
  size_t length = strlen(text);
  while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                        text[length - 1] == '\r' || text[length - 1] == '\n')) {
    text[--length] = '\0';
  }
  return text;
}

Theme *theme_parse(const char *text, ArgList *warnings) {
  Theme *theme = malloc(sizeof(Theme));
  if (theme == NULL) {
    return NULL;
  }
  theme_defaults(theme);

  char *copy = strdup(text);
  if (copy == NULL) {
    return theme;
  }

  enum { NOWHERE, IN_THEME, IN_SEGMENT, IN_SKIPPED } section = NOWHERE;
  Segment *segment = NULL;
  int line_number = 0;
  char *cursor = copy;

  for (char *line = next_line(&cursor);line != NULL;line = next_line(&cursor)) {
    line_number++;
    char *body = trim(line);
    if (body[0] == '\0' || body[0] == '#') {
      continue;
    }
    if (strcmp(body, "[theme]") == 0) {
      section = IN_THEME;
      continue;
    }
    if (strcmp(body, "[segment]") == 0) {
      if (theme->count >= MAX_SEGMENTS) {
        warn(warnings, line_number, "too many segments (the limit is %d)", MAX_SEGMENTS);
        section = IN_SKIPPED;
        continue;
      }
      segment = &theme->segments[theme->count++];
      segment_defaults(segment);
      section = IN_SEGMENT;
      continue;
    }
    if (body[0] == '[') {
      warn(warnings, line_number, "unknown section %s (use [theme] or [segment])", body);
      section = IN_SKIPPED;
      continue;
    }

    char *equals = strchr(body, '=');
    if (equals == NULL) {
      warn(warnings, line_number, "expected key=value");
      continue;
    }
    *equals = '\0';
    char *key = trim(body);
    char *value = trim(equals + 1);
    // "  quoted values keep their spaces  "
    size_t value_length = strlen(value);
    if (value_length >= 2 && value[0] == '"' && value[value_length - 1] == '"') {
      value[value_length - 1] = '\0';
      value++;
    }

    if (section == IN_THEME) {
      apply_theme_key(theme, key, value, line_number, warnings);
    }
    else if (section == IN_SEGMENT) {
      apply_segment_key(segment, key, value, line_number, warnings);
    }
    else if (section == NOWHERE) {
      warn(warnings, line_number, "'%s' must be inside [theme] or [segment]", key);
    }
  }

  // A segment with no valid type can't render.
  for (int i = 0;i < theme->count;i++) {
    if (theme->segments[i].type == SEG_INVALID && warnings != NULL) {
      char message[96];
      snprintf(message, sizeof(message), "segment %d has no valid type= and is skipped", i + 1);
      arglist_append(warnings, message);
    }
  }

  free(copy);
  return theme;
}

void theme_free(Theme *theme) {
  free(theme);
}

// ---------------------------------------------------------------------------
// Data sources
// ---------------------------------------------------------------------------

typedef struct {
  int in_repo;
  char branch[256];
  int staged, modified, untracked, conflicts, ahead, behind;
}GitInfo;

// Finds the folder holding .git, walking up from the current directory.
static int find_git_dir(char *git_dir, size_t size) {
  char path[PATH_MAX];
  if (getcwd(path, sizeof(path)) == NULL) {
    return 0;
  }
  while (1) {
    char candidate[PATH_MAX + 8];
    snprintf(candidate, sizeof(candidate), "%s/.git", strcmp(path, "/") == 0 ? "" : path);
    if (access(candidate, F_OK) == 0) {
      snprintf(git_dir, size, "%.*s", (int)size - 1, candidate);
      return 1;
    }
    char *slash = strrchr(path, '/');
    if (slash == NULL || slash == path) {
      if (strcmp(path, "/") == 0) return 0;
      snprintf(path, sizeof(path), "/");
      continue;
    }
    *slash = '\0';
  }
}

// Reads the branch from .git/HEAD without running git (used when git
// isn't installed or `dirty` is off).
static void read_head(const char *git_dir, GitInfo *info) {
  char head_path[PATH_MAX + 16];
  char gitdir_buffer[PATH_MAX];
  const char *dir = git_dir;

  // Worktrees and submodules have a .git *file* pointing elsewhere.
  FILE *pointer = fopen(git_dir, "r");
  if (pointer != NULL) {
    char line[PATH_MAX];
    if (fgets(line, sizeof(line), pointer) != NULL && strncmp(line, "gitdir: ", 8) == 0) {
      line[strcspn(line, "\n")] = '\0';
      if (line[8] == '/') {
        snprintf(gitdir_buffer, sizeof(gitdir_buffer), "%s", line + 8);
      }
      else {
        char base[PATH_MAX];
        snprintf(base, sizeof(base), "%s", git_dir);
        char *slash = strrchr(base, '/');
        if (slash) *slash = '\0';
        snprintf(gitdir_buffer, sizeof(gitdir_buffer), "%.2000s/%.2000s", base, line + 8);
      }
      dir = gitdir_buffer;
    }
    fclose(pointer);
  }

  snprintf(head_path, sizeof(head_path), "%s/HEAD", dir);
  FILE *head = fopen(head_path, "r");
  if (head == NULL) {
    return;
  }
  char line[256];
  if (fgets(line, sizeof(line), head) != NULL) {
    line[strcspn(line, "\n")] = '\0';
    if (strncmp(line, "ref: refs/heads/", 16) == 0) {
      snprintf(info->branch, sizeof(info->branch), "%.200s", line + 16);
    }
    else {
      snprintf(info->branch, sizeof(info->branch), "%.7s", line);
    }
  }
  fclose(head);
}

static void git_info(GitInfo *info, int want_status) {
  memset(info, 0, sizeof(*info));
  char git_dir[PATH_MAX];
  if (!find_git_dir(git_dir, sizeof(git_dir))) {
    return;
  }
  info->in_repo = 1;

  char git_path[PATH_MAX];
  if (!want_status || !path_lookup("git", git_path, sizeof(git_path))) {
    read_head(git_dir, info);
    return;
  }

  // One git call gives the branch, ahead/behind and every changed file.
  FILE *status = popen("git --no-optional-locks status --porcelain=v2 --branch 2>/dev/null", "r");
  if (status == NULL) {
    read_head(git_dir, info);
    return;
  }
  char line[1024];
  char oid[64] = "";
  while (fgets(line, sizeof(line), status) != NULL) {
    line[strcspn(line, "\n")] = '\0';
    if (strncmp(line, "# branch.oid ", 13) == 0) {
      snprintf(oid, sizeof(oid), "%.63s", line + 13);
    }
    else if (strncmp(line, "# branch.head ", 14) == 0) {
      snprintf(info->branch, sizeof(info->branch), "%.200s", line + 14);
    }
    else if (strncmp(line, "# branch.ab ", 12) == 0) {
      sscanf(line + 12, "+%d -%d", &info->ahead, &info->behind);
    }
    else if ((line[0] == '1' || line[0] == '2') && line[1] == ' ') {
      if (line[2] != '.') info->staged++;
      if (line[3] != '.') info->modified++;
    }
    else if (line[0] == 'u' && line[1] == ' ') {
      info->conflicts++;
    }
    else if (line[0] == '?' && line[1] == ' ') {
      info->untracked++;
    }
  }
  pclose(status);

  if (info->branch[0] == '\0') {
    read_head(git_dir, info);
  }
  else if (strcmp(info->branch, "(detached)") == 0) {
    snprintf(info->branch, sizeof(info->branch), "%.7s", oid);
  }
}

typedef struct {
  const char *name;
  const char *nerd_icon;
  const char *markers;      // file names, or *.ext, separated by commas
  const char *version_command;
  unsigned char r, g, b;
}Language;

static const Language languages[] = {
  { "python", "", "pyproject.toml,requirements.txt,setup.py,Pipfile,.python-version,*.py", "python3 --version", 55, 118, 171 },
  { "node",   "", "package.json,.nvmrc,*.js,*.mjs,*.cjs,*.ts,*.tsx", "node --version", 95, 160, 78 },
  { "rust",   "", "Cargo.toml,*.rs", "rustc --version", 222, 165, 132 },
  { "go",     "", "go.mod,*.go", "go version", 0, 173, 216 },
  { "java",   "", "pom.xml,build.gradle,build.gradle.kts,*.java", "java -version", 237, 139, 0 },
  { "kotlin", "", "*.kt,*.kts", NULL, 127, 82, 255 },
  { "c",      "", "*.c", "cc --version", 168, 185, 204 },
  { "cpp",    "", "*.cpp,*.cc,*.cxx,*.hpp", "c++ --version", 0, 89, 156 },
  { "ruby",   "", "Gemfile,*.rb", "ruby --version", 204, 52, 45 },
  { "php",    "", "composer.json,*.php", "php --version", 119, 123, 180 },
  { "lua",    "", "*.lua", "lua -v", 81, 160, 207 },
  { "elixir", "", "mix.exs,*.ex,*.exs", NULL, 110, 74, 126 },
  { "haskell","", "stack.yaml,*.cabal,*.hs", "ghc --numeric-version", 143, 78, 139 },
  { "dart",   "", "pubspec.yaml,*.dart", "dart --version", 1, 117, 194 },
  { "zig",    "", "build.zig,*.zig", "zig version", 247, 164, 29 },
  { "swift",  "", "Package.swift,*.swift", "swift --version", 240, 81, 56 },
  { "dotnet", "", "*.csproj,*.sln,*.cs,*.fsproj", "dotnet --version", 123, 97, 255 },
  { "deno",   "", "deno.json,deno.jsonc", "deno --version", 112, 255, 175 },
  { "docker", "", "Dockerfile,docker-compose.yml,docker-compose.yaml,compose.yaml", "docker --version", 36, 150, 237 },
};

#define LANGUAGE_COUNT (int)(sizeof(languages) / sizeof(languages[0]))

static int matches_marker(const char *file, const char *markers) {
  const char *start = markers;
  while (*start != '\0') {
    size_t length = strcspn(start, ",");
    if (start[0] == '*') {
      size_t suffix_length = length - 1;
      size_t file_length = strlen(file);
      if (file_length > suffix_length && strncmp(file + file_length - suffix_length, start + 1, suffix_length) == 0) {
        return 1;
      }
    }
    else if (strlen(file) == length && strncmp(file, start, length) == 0) {
      return 1;
    }
    start += length;
    if (*start == ',') start++;
  }
  return 0;
}

// Marks which languages the current folder uses.
static int detect_languages(int found[LANGUAGE_COUNT]) {
  memset(found, 0, sizeof(int) * LANGUAGE_COUNT);
  DIR *dir = opendir(".");
  if (dir == NULL) {
    return 0;
  }
  int total = 0;
  int scanned = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL && scanned < 2000) {
    scanned++;
    for (int i = 0;i < LANGUAGE_COUNT;i++) {
      if (!found[i] && matches_marker(entry->d_name, languages[i].markers)) {
        found[i] = 1;
        total++;
      }
    }
  }
  closedir(dir);
  return total;
}

// Versions are looked up once per language (and per $PATH, so activating
// a venv shows the venv's python).
static const char *language_version(int index) {
  static char versions[LANGUAGE_COUNT][32];
  static unsigned long path_hashes[LANGUAGE_COUNT];
  static int fetched[LANGUAGE_COUNT];

  if (languages[index].version_command == NULL) {
    return "";
  }
  unsigned long hash = 5381;
  const char *path = getenv("PATH");
  for (const char *c = path ? path : "";*c;c++) hash = hash * 33 + (unsigned char)*c;

  if (fetched[index] && path_hashes[index] == hash) {
    return versions[index];
  }
  fetched[index] = 1;
  path_hashes[index] = hash;
  versions[index][0] = '\0';

  char command[128];
  snprintf(command, sizeof(command), "%s 2>&1 </dev/null", languages[index].version_command);
  FILE *pipe = popen(command, "r");
  if (pipe == NULL) {
    return "";
  }
  char output[512];
  size_t length = fread(output, 1, sizeof(output) - 1, pipe);
  output[length] = '\0';
  pclose(pipe);

  // The first thing that looks like 1.2 or 1.2.3.
  for (char *c = output;*c;c++) {
    if (*c >= '0' && *c <= '9') {
      char *end = c;
      int dots = 0;
      while ((*end >= '0' && *end <= '9') || (*end == '.' && end[1] >= '0' && end[1] <= '9')) {
        if (*end == '.') dots++;
        end++;
      }
      if (dots >= 1) {
        snprintf(versions[index], sizeof(versions[index]), "%.*s", (int)(end - c), c);
        break;
      }
      c = end;
      if (*c == '\0') break;
    }
  }
  return versions[index];
}

static const struct {
  const char *id;
  const char *icon;
}os_icons[] = {
  { "ubuntu", "" }, { "debian", "" }, { "fedora", "" },
  { "arch", "" }, { "manjaro", "" }, { "alpine", "" },
  { "nixos", "" }, { "opensuse", "" }, { "opensuse-tumbleweed", "" },
  { "opensuse-leap", "" }, { "gentoo", "" }, { "linuxmint", "" },
  { "pop", "" }, { "centos", "" }, { "rhel", "" },
  { "endeavouros", "" }, { "macos", "" }, { NULL, NULL }
};

static const char *os_id(void) {
  static char id[32] = "";
  if (id[0] != '\0') {
    return id;
  }
  struct utsname name;
  if (uname(&name) == 0 && strcmp(name.sysname, "Darwin") == 0) {
    snprintf(id, sizeof(id), "macos");
    return id;
  }
  snprintf(id, sizeof(id), "linux");
  FILE *release = fopen("/etc/os-release", "r");
  if (release == NULL) {
    return id;
  }
  char line[256];
  while (fgets(line, sizeof(line), release) != NULL) {
    if (strncmp(line, "ID=", 3) == 0) {
      char *value = trim(line + 3);
      if (value[0] == '"') {
        value++;
        value[strcspn(value, "\"")] = '\0';
      }
      snprintf(id, sizeof(id), "%s", value);
      break;
    }
  }
  fclose(release);
  return id;
}

static const char *os_icon(void) {
  const char *id = os_id();
  for (int i = 0;os_icons[i].id != NULL;i++) {
    if (strcmp(os_icons[i].id, id) == 0) {
      return os_icons[i].icon;
    }
  }
  return "";  // Tux
}

static void format_cwd(char *out, size_t size, int depth, int *at_home) {
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    snprintf(out, size, "?");
    return;
  }
  const char *home = getenv("HOME");
  char shown[PATH_MAX + 2];
  if (home != NULL && home[0] && strncmp(cwd, home, strlen(home)) == 0 &&
      (cwd[strlen(home)] == '\0' || cwd[strlen(home)] == '/')) {
    snprintf(shown, sizeof(shown), "~%s", cwd + strlen(home));
  }
  else {
    snprintf(shown, sizeof(shown), "%s", cwd);
  }
  *at_home = strcmp(shown, "~") == 0;

  const char *display = shown;
  if (depth > 0) {
    int separators = 0;
    const char *cut = shown + strlen(shown);
    while (cut > shown) {
      if (*(cut - 1) == '/' && ++separators >= depth) {
        break;
      }
      cut--;
    }
    if (cut > shown && *cut) {
      display = cut;
    }
  }
  snprintf(out, size, "%s", display);
}

static void format_duration(char *out, size_t size, double seconds) {
  if (seconds < 60) {
    snprintf(out, size, "%.1fs", seconds);
  }
  else if (seconds < 3600) {
    snprintf(out, size, "%dm%02ds", (int)seconds / 60, (int)seconds % 60);
  }
  else {
    snprintf(out, size, "%dh%02dm", (int)seconds / 3600, ((int)seconds % 3600) / 60);
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

typedef struct {
  char *text;
  Color fg, bg;
  int bold, italic;
  int newline;
}Piece;

// prefix + icon + " " + body + suffix
static char *compose(const Segment *segment, const char *icon, const char *body) {
  Buf buf = { NULL, 0, 0 };
  buf_add(&buf, segment->prefix);
  buf_add(&buf, icon);
  if (icon[0] != '\0' && body[0] != '\0') {
    buf_add(&buf, " ");
  }
  buf_add(&buf, body);
  buf_add(&buf, segment->suffix);
  return buf_take(&buf);
}

// Returns the segment's visible text, or NULL when it has nothing to show
// (git outside a repo, status after success...).
static char *segment_content(const Theme *theme, const Segment *segment, const ThemeContext *context,
                             Color *auto_color) {
  const char *icon = icon_for(segment, theme->glyphs);
  char body[PATH_MAX];
  body[0] = '\0';
  int failed = context->last_status != 0;

  switch (segment->type) {
    case SEG_INVALID:
    case SEG_NEWLINE:
      return NULL;

    case SEG_TEXT:
      return compose(segment, icon, segment->text);

    case SEG_USER:
      return compose(segment, icon, context->user);

    case SEG_HOST: {
      char host[256];
      snprintf(host, sizeof(host), "%s", context->host);
      host[strcspn(host, ".")] = '\0';
      return compose(segment, icon, host);
    }

    case SEG_USERHOST: {
      char host[256];
      snprintf(host, sizeof(host), "%s", context->host);
      host[strcspn(host, ".")] = '\0';
      snprintf(body, sizeof(body), "%s@%s", context->user, host);
      return compose(segment, icon, body);
    }

    case SEG_CWD: {
      int at_home = 0;
      format_cwd(body, sizeof(body), segment->depth, &at_home);
      if (at_home && strcmp(segment->icon, "auto") == 0 && theme->glyphs == GLYPHS_NERD) {
        icon = "";  // house
      }
      return compose(segment, icon, body);
    }

    case SEG_GIT: {
      GitInfo git;
      git_info(&git, segment->dirty);
      if (!git.in_repo) {
        return NULL;
      }
      Buf buf = { NULL, 0, 0 };
      buf_add(&buf, git.branch[0] ? git.branch : "?");
      int ascii = theme->glyphs == GLYPHS_ASCII;
      int changed = git.staged + git.modified + git.untracked + git.conflicts;
      if (segment->dirty && segment->counts) {
        if (git.conflicts) buf_addf(&buf, " !%d", git.conflicts);
        if (git.staged) buf_addf(&buf, " +%d", git.staged);
        if (git.modified) buf_addf(&buf, " ~%d", git.modified);
        if (git.untracked) buf_addf(&buf, " ?%d", git.untracked);
      }
      else if (segment->dirty && changed > 0) {
        buf_add(&buf, ascii ? "*" : " ●");
      }
      if (segment->ahead_behind) {
        if (git.ahead) buf_addf(&buf, ascii ? " ^%d" : " ⇡%d", git.ahead);
        if (git.behind) buf_addf(&buf, ascii ? " v%d" : " ⇣%d", git.behind);
      }
      char *text = buf_take(&buf);
      char *result = compose(segment, icon, text);
      free(text);
      return result;
    }

    case SEG_LANGS: {
      int found[LANGUAGE_COUNT];
      if (detect_languages(found) == 0) {
        return NULL;
      }
      Buf buf = { NULL, 0, 0 };
      int first = -1;
      for (int i = 0;i < LANGUAGE_COUNT;i++) {
        if (!found[i]) continue;
        if (first < 0) first = i;
        if (buf.length > 0) buf_add(&buf, " ");
        if (theme->glyphs == GLYPHS_NERD && strcmp(segment->icon, "none") != 0) {
          buf_add(&buf, languages[i].nerd_icon);
          if (segment->version) {
            const char *version = language_version(i);
            if (version[0]) buf_addf(&buf, " %s", version);
          }
        }
        else {
          buf_add(&buf, languages[i].name);
          if (segment->version) {
            const char *version = language_version(i);
            if (version[0]) buf_addf(&buf, " %s", version);
          }
        }
      }
      *auto_color = rgb(languages[first].r, languages[first].g, languages[first].b);
      char *text = buf_take(&buf);
      Segment plain = *segment;
      char *result = compose(&plain, "", text);
      free(text);
      return result;
    }

    case SEG_VENV: {
      const char *venv = getenv("VIRTUAL_ENV");
      const char *conda = getenv("CONDA_DEFAULT_ENV");
      if (venv != NULL && venv[0]) {
        const char *base = strrchr(venv, '/');
        snprintf(body, sizeof(body), "%s", base ? base + 1 : venv);
      }
      else if (conda != NULL && conda[0] && strcmp(conda, "base") != 0) {
        snprintf(body, sizeof(body), "%s", conda);
      }
      else {
        return NULL;
      }
      return compose(segment, icon, body);
    }

    case SEG_STATUS:
      if (!failed) {
        if (!segment->show_success) return NULL;
        return compose(segment, theme->glyphs == GLYPHS_ASCII ? "ok" :
                       theme->glyphs == GLYPHS_NERD ? "" : "✔", "");
      }
      snprintf(body, sizeof(body), "%d", context->last_status);
      return compose(segment, icon, body);

    case SEG_DURATION:
      if (context->last_duration < segment->min) return NULL;
      format_duration(body, sizeof(body), context->last_duration);
      return compose(segment, icon, body);

    case SEG_TIME: {
      time_t now = time(NULL);
      struct tm local;
      localtime_r(&now, &local);
      if (strftime(body, sizeof(body), segment->format, &local) == 0) body[0] = '\0';
      return compose(segment, icon, body);
    }

    case SEG_JOBS:
      if (context->jobs <= 0) return NULL;
      snprintf(body, sizeof(body), "%d", context->jobs);
      return compose(segment, icon, body);

    case SEG_SHELL:
      return compose(segment, icon, context->shell_language);

    case SEG_OS: {
      const char *glyph = icon;
      if (strcmp(segment->icon, "auto") == 0) {
        glyph = theme->glyphs == GLYPHS_NERD ? os_icon() : "";
      }
      return compose(segment, glyph, theme->glyphs == GLYPHS_NERD && strcmp(segment->icon, "auto") == 0 ? "" : os_id());
    }

    case SEG_SYMBOL: {
      const char *symbol = segment->text;
      if (failed && segment->error_text[0]) {
        symbol = segment->error_text;
      }
      else if (strcmp(symbol, "auto") == 0 || symbol[0] == '\0') {
        symbol = theme->glyphs == GLYPHS_ASCII ? "$" : "❯";
      }
      return compose(segment, "", symbol);
    }
  }
  return NULL;
}

static void add_separator(Buf *out, const Theme *theme, Color fg, Color bg, int thin, ColorMode mode) {
  const char *glyph;
  switch (theme->separator) {
    case SEP_POWERLINE: glyph = thin ? "" : ""; break;
    case SEP_ROUND:     glyph = thin ? "" : ""; break;
    case SEP_SLANT:     glyph = thin ? "" : ""; break;
    case SEP_CUSTOM:    glyph = theme->separator_char[0] ? theme->separator_char : " "; break;
    default:            glyph = ""; break;
  }
  add_style(out, fg, bg, 0, 0, mode);
  buf_add(out, glyph);
}

char *theme_render(const Theme *theme, const ThemeContext *context) {
  ColorMode mode = theme->colors == COLORS_AUTO ? detect_color_mode() : theme->colors;
  Separator style = theme->separator;
  // Powerline shapes need a Nerd/Powerline font.
  if (theme->glyphs == GLYPHS_ASCII && (style == SEP_POWERLINE || style == SEP_ROUND || style == SEP_SLANT)) {
    style = SEP_PLAIN;
  }
  int glyph_joins = style == SEP_POWERLINE || style == SEP_ROUND || style == SEP_SLANT || style == SEP_CUSTOM;
  Color none = { COLOR_NONE, 0, 0, 0, 0 };

  Piece pieces[MAX_SEGMENTS];
  int count = 0;
  for (int i = 0;i < theme->count;i++) {
    const Segment *segment = &theme->segments[i];
    if (segment->type == SEG_NEWLINE) {
      memset(&pieces[count], 0, sizeof(Piece));
      pieces[count++].newline = 1;
      continue;
    }
    Color auto_color = none;
    char *text = segment_content(theme, segment, context, &auto_color);
    if (text == NULL) {
      continue;
    }
    Piece *piece = &pieces[count++];
    piece->text = text;
    piece->newline = 0;
    piece->bold = segment->bold;
    piece->italic = segment->italic;
    piece->fg = segment->fg;
    piece->bg = segment->bg;
    if (context->last_status != 0) {
      if (segment->error_fg.kind != COLOR_NONE) piece->fg = segment->error_fg;
      if (segment->error_bg.kind != COLOR_NONE) piece->bg = segment->error_bg;
    }
    // Language colors: a colored block with readable text, or colored text.
    if (piece->bg.kind == COLOR_AUTO) {
      piece->bg = auto_color.kind != COLOR_NONE ? auto_color : none;
      if (piece->fg.kind == COLOR_AUTO || piece->fg.kind == COLOR_NONE) {
        piece->fg = readable_on(piece->bg);
      }
    }
    if (piece->fg.kind == COLOR_AUTO) {
      piece->fg = auto_color;
    }
    if (style == SEP_NONE) {
      piece->bg = none;
    }
  }

  Buf out = { NULL, 0, 0 };
  int first_on_line = 1;
  Piece *previous = NULL;

  for (int i = 0;i < count;i++) {
    Piece *piece = &pieces[i];

    if (piece->newline) {
      if (previous != NULL && previous->bg.kind != COLOR_NONE && glyph_joins) {
        add_separator(&out, theme, previous->bg, none, 0, mode);
      }
      buf_add(&out, "\x1b[0m\n");
      first_on_line = 1;
      previous = NULL;
      continue;
    }

    int has_bg = piece->bg.kind != COLOR_NONE;
    if (!first_on_line && previous != NULL) {
      int previous_bg = previous->bg.kind != COLOR_NONE;
      if (previous_bg && has_bg) {
        if (glyph_joins) {
          if (same_color(previous->bg, piece->bg)) {
            add_separator(&out, theme, previous->fg, previous->bg, 1, mode);
          }
          else {
            add_separator(&out, theme, previous->bg, piece->bg, 0, mode);
          }
        }
      }
      else if (previous_bg && !has_bg) {
        if (glyph_joins) {
          add_separator(&out, theme, previous->bg, none, 0, mode);
        }
        buf_add(&out, "\x1b[0m ");
      }
      else {
        buf_add(&out, "\x1b[0m ");
        if (has_bg && style == SEP_ROUND) {
          add_style(&out, piece->bg, none, 0, 0, mode);
          buf_add(&out, "");
        }
      }
    }
    else if (has_bg && style == SEP_ROUND) {
      add_style(&out, piece->bg, none, 0, 0, mode);
      buf_add(&out, "");
    }

    add_style(&out, piece->fg, piece->bg, piece->bold, piece->italic, mode);
    if (has_bg) {
      buf_add(&out, " ");
      buf_add(&out, piece->text);
      buf_add(&out, " ");
    }
    else {
      buf_add(&out, piece->text);
    }
    first_on_line = 0;
    previous = piece;
  }

  if (previous != NULL && previous->bg.kind != COLOR_NONE && glyph_joins) {
    add_separator(&out, theme, previous->bg, none, 0, mode);
  }
  buf_add(&out, "\x1b[0m ");

  for (int i = 0;i < count;i++) {
    free(pieces[i].text);
  }
  return buf_take(&out);
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------

static const struct {
  const char *name;
  const char *description;
  const char *text;
}presets[] = {
  { "powerline", "Classic powerline blocks with git and languages (needs a Nerd Font)",
    "[theme]\nname=powerline\nseparator=powerline\nglyphs=nerd\ncolors=auto\n\n"
    "[segment]\ntype=os\nfg=#ffffff\nbg=#3b4252\n\n"
    "[segment]\ntype=cwd\nfg=#ffffff\nbg=#5e81ac\nbold=1\ndepth=3\n\n"
    "[segment]\ntype=git\nfg=#2e3440\nbg=#ebcb8b\ncounts=1\n\n"
    "[segment]\ntype=langs\nfg=auto\nbg=#434c5e\n\n"
    "[segment]\ntype=venv\nfg=#2e3440\nbg=#a3be8c\n\n"
    "[segment]\ntype=duration\nfg=#2e3440\nbg=#d08770\n\n"
    "[segment]\ntype=status\nfg=#ffffff\nbg=#bf616a\n\n"
    "[segment]\ntype=jobs\nfg=#2e3440\nbg=#b48ead\n" },

  { "rounded", "Soft pastel pills with rounded ends (needs a Nerd Font)",
    "[theme]\nname=rounded\nseparator=round\nglyphs=nerd\ncolors=auto\n\n"
    "[segment]\ntype=userhost\nfg=#1e1e2e\nbg=#cba6f7\n\n"
    "[segment]\ntype=cwd\nfg=#1e1e2e\nbg=#89b4fa\ndepth=2\n\n"
    "[segment]\ntype=git\nfg=#1e1e2e\nbg=#a6e3a1\n\n"
    "[segment]\ntype=langs\nfg=#1e1e2e\nbg=#f9e2af\nversion=1\n\n"
    "[segment]\ntype=status\nfg=#1e1e2e\nbg=#f38ba8\n\n"
    "[segment]\ntype=symbol\nfg=#a6e3a1\nerror_fg=#f38ba8\n" },

  { "minimal", "Two lines, no backgrounds: folder, branch, languages, then a ❯",
    "[theme]\nname=minimal\nseparator=none\nglyphs=unicode\ncolors=auto\nblank_line=1\n\n"
    "[segment]\ntype=cwd\nfg=cyan\nbold=1\nicon=none\n\n"
    "[segment]\ntype=git\nfg=magenta\nprefix=\"on \"\n\n"
    "[segment]\ntype=langs\nfg=auto\nprefix=\"via \"\n\n"
    "[segment]\ntype=duration\nfg=yellow\nicon=none\nprefix=\"took \"\n\n"
    "[segment]\ntype=newline\n\n"
    "[segment]\ntype=symbol\nfg=green\nerror_fg=red\nbold=1\n" },

  { "boxed", "Two-line prompt drawn with box characters (needs a Nerd Font)",
    "[theme]\nname=boxed\nseparator=powerline\nglyphs=nerd\ncolors=auto\n\n"
    "[segment]\ntype=text\ntext=╭─\nfg=bright_black\n\n"
    "[segment]\ntype=os\nfg=#ffffff\nbg=#4c566a\n\n"
    "[segment]\ntype=cwd\nfg=#ffffff\nbg=#5e81ac\ndepth=0\n\n"
    "[segment]\ntype=git\nfg=#2e3440\nbg=#a3be8c\ncounts=1\n\n"
    "[segment]\ntype=time\nfg=#2e3440\nbg=#d8dee9\n\n"
    "[segment]\ntype=newline\n\n"
    "[segment]\ntype=text\ntext=╰─\nfg=bright_black\n\n"
    "[segment]\ntype=symbol\nfg=bright_green\nerror_fg=bright_red\n" },

  { "classic", "user@host folder (branch) $ in plain colors; works in any terminal",
    "[theme]\nname=classic\nseparator=none\nglyphs=ascii\ncolors=16\n\n"
    "[segment]\ntype=userhost\nfg=green\nbold=1\n\n"
    "[segment]\ntype=cwd\nfg=blue\nbold=1\nicon=none\n\n"
    "[segment]\ntype=git\nfg=yellow\nicon=none\nprefix=(\nsuffix=)\n\n"
    "[segment]\ntype=status\nfg=red\n\n"
    "[segment]\ntype=symbol\nfg=default\nerror_fg=red\n" },
};

#define PRESET_COUNT (int)(sizeof(presets) / sizeof(presets[0]))

int theme_preset_count(void) { return PRESET_COUNT; }
const char *theme_preset_name(int index) { return presets[index].name; }
const char *theme_preset_description(int index) { return presets[index].description; }
const char *theme_preset_text(int index) { return presets[index].text; }

int theme_preset_find(const char *name) {
  for (int i = 0;i < PRESET_COUNT;i++) {
    if (strcmp(presets[i].name, name) == 0) return i;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// The active theme
// ---------------------------------------------------------------------------

static Theme *active_theme = NULL;
static char path_buffer[PATH_MAX + 16];

const char *theme_path(void) {
  if (path_buffer[0] == '\0') {
    snprintf(path_buffer, sizeof(path_buffer), "%s/theme.t", config_directory());
  }
  return path_buffer;
}

static char *read_file(const char *path) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return NULL;
  }
  Buf buf = { NULL, 0, 0 };
  char chunk[4096];
  size_t got;
  while ((got = fread(chunk, 1, sizeof(chunk) - 1, file)) > 0) {
    chunk[got] = '\0';
    buf_add(&buf, chunk);
  }
  fclose(file);
  return buf_take(&buf);
}

void theme_init(void) {
  theme_free(active_theme);
  active_theme = NULL;

  char *text = read_file(theme_path());
  if (text == NULL) {
    return;
  }
  ArgList warnings;
  arglist_init(&warnings);
  active_theme = theme_parse(text, &warnings);
  for (int i = 0;i < warnings.count;i++) {
    fprintf(stderr, "tarsh: theme.t %s\n", warnings.items[i]);
  }
  arglist_free(&warnings);
  free(text);
}

int theme_active(void) {
  return active_theme != NULL && active_theme->enabled;
}

const char *theme_current_name(void) {
  if (active_theme == NULL) return NULL;
  return active_theme->name;
}

char *theme_render_active(const ThemeContext *context) {
  if (!theme_active()) {
    return NULL;
  }
  char *prompt = theme_render(active_theme, context);
  if (active_theme->blank_line) {
    static int first = 1;
    if (!first) {
      char *spaced = malloc(strlen(prompt) + 2);
      if (spaced != NULL) {
        sprintf(spaced, "\n%s", prompt);
        free(prompt);
        prompt = spaced;
      }
    }
    first = 0;
  }
  return prompt;
}

char *theme_saved_text(void) {
  char *text = read_file(theme_path());
  if (text != NULL) {
    return text;
  }
  return strdup(presets[0].text);
}

int theme_save_text(const char *text) {
  char temp[PATH_MAX + 32];
  snprintf(temp, sizeof(temp), "%s.new", theme_path());
  FILE *file = fopen(temp, "w");
  if (file == NULL) {
    return -1;
  }
  fputs(text, file);
  if (text[0] && text[strlen(text) - 1] != '\n') {
    fputc('\n', file);
  }
  if (fclose(file) != 0 || rename(temp, theme_path()) != 0) {
    unlink(temp);
    return -1;
  }
  theme_init();
  return 0;
}

int theme_set_enabled(int enabled) {
  char *text = read_file(theme_path());
  if (text == NULL) {
    return -1;
  }
  // Rewrite (or add) enabled= in the [theme] section.
  Buf out = { NULL, 0, 0 };
  int in_theme = 0;
  int written = 0;
  char *cursor = text;
  for (char *line = next_line(&cursor);line != NULL;line = next_line(&cursor)) {
    char copy[1024];
    snprintf(copy, sizeof(copy), "%s", line);
    char *body = trim(copy);
    if (in_theme && body[0] == '[' && !written) {
      buf_addf(&out, "enabled=%d\n", enabled);
      written = 1;
    }
    if (strcmp(body, "[theme]") == 0) {
      in_theme = 1;
    }
    else if (body[0] == '[') {
      in_theme = 0;
    }
    if (in_theme && strncmp(body, "enabled", 7) == 0 && strchr(body, '=') != NULL) {
      buf_addf(&out, "enabled=%d\n", enabled);
      written = 1;
      continue;
    }
    buf_add(&out, line);
    buf_add(&out, "\n");
  }
  if (!written) {
    // No [theme] section yet, or it was the last one.
    if (!in_theme) buf_add(&out, "[theme]\n");
    buf_addf(&out, "enabled=%d\n", enabled);
  }
  free(text);
  char *result = buf_take(&out);
  int status = theme_save_text(result);
  free(result);
  return status;
}

// ---------------------------------------------------------------------------
// JSON for the editor
// ---------------------------------------------------------------------------

void theme_json_string(FILE *out, const char *text) {
  fputc('"', out);
  for (const unsigned char *c = (const unsigned char *)text;*c;c++) {
    switch (*c) {
      case '"': fputs("\\\"", out); break;
      case '\\': fputs("\\\\", out); break;
      case '\n': fputs("\\n", out); break;
      case '\r': fputs("\\r", out); break;
      case '\t': fputs("\\t", out); break;
      default:
        if (*c < 0x20) fprintf(out, "\\u%04x", *c);
        else fputc(*c, out);
    }
  }
  fputc('"', out);
}

void theme_write_context_json(FILE *out) {
  char cwd[PATH_MAX];
  int at_home = 0;
  format_cwd(cwd, sizeof(cwd), 0, &at_home);

  GitInfo git;
  git_info(&git, 1);
  int found[LANGUAGE_COUNT];
  detect_languages(found);

  fputs("{\"cwd\":", out);
  theme_json_string(out, cwd);
  fputs(",\"os\":", out);
  theme_json_string(out, os_id());
  fprintf(out, ",\"git\":{\"in_repo\":%s,\"branch\":", git.in_repo ? "true" : "false");
  theme_json_string(out, git.branch);
  fprintf(out, ",\"staged\":%d,\"modified\":%d,\"untracked\":%d,\"ahead\":%d,\"behind\":%d}",
          git.staged, git.modified, git.untracked, git.ahead, git.behind);
  fputs(",\"langs\":[", out);
  int first = 1;
  for (int i = 0;i < LANGUAGE_COUNT;i++) {
    if (!found[i]) continue;
    if (!first) fputc(',', out);
    theme_json_string(out, languages[i].name);
    first = 0;
  }
  fputs("],\"venv\":", out);
  const char *venv = getenv("VIRTUAL_ENV");
  theme_json_string(out, venv ? venv : "");
  const char *colorterm = getenv("COLORTERM");
  const char *term = getenv("TERM");
  fputs(",\"colorterm\":", out);
  theme_json_string(out, colorterm ? colorterm : "");
  fputs(",\"term\":", out);
  theme_json_string(out, term ? term : "");
  fputs("}", out);
}
