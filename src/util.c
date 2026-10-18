#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

#include "util.h"

static int is_executable_file(const char *path) {
  struct stat info;
  return stat(path, &info) == 0 && S_ISREG(info.st_mode) && access(path, X_OK) == 0;
}

int path_lookup(const char *name, char *out, size_t out_size) {
  if (name == NULL || name[0] == '\0') {
    return 0;
  }

  if (strchr(name, '/') != NULL) {
    if (is_executable_file(name)) {
      snprintf(out, out_size, "%s", name);
      return 1;
    }
    return 0;
  }

  const char *path = getenv("PATH");
  if (path == NULL) {
    path = "/usr/local/bin:/usr/bin:/bin";
  }

  const char *start = path;
  while (1) {
    const char *end = strchr(start, ':');
    size_t length = (end == NULL) ? strlen(start) : (size_t)(end - start);

    char candidate[PATH_MAX];
    // An empty PATH entry means the current directory.
    if (length == 0) {
      snprintf(candidate, sizeof(candidate), "./%s", name);
    }
    else {
      snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)length, start, name);
    }

    if (is_executable_file(candidate)) {
      snprintf(out, out_size, "%s", candidate);
      return 1;
    }

    if (end == NULL) {
      break;
    }
    start = end + 1;
  }
  return 0;
}

static int is_plain_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         strchr("._/+-@%:,=", c) != NULL;
}

char *shell_quote(const char *text) {
  int plain = text[0] != '\0';
  for (const char *c = text;*c != '\0';c++) {
    // Bytes >= 0x80 are UTF-8; they are safe unquoted too.
    if (!is_plain_char(*c) && (unsigned char)*c < 0x80) {
      plain = 0;
      break;
    }
  }
  if (plain) {
    return strdup(text);
  }

  // Wrap in single quotes; an embedded ' becomes '\'' (close, escaped
  // quote, reopen).
  size_t length = 2;
  for (const char *c = text;*c != '\0';c++) {
    length += (*c == '\'') ? 4 : 1;
  }

  char *quoted = malloc(length + 1);
  if (quoted == NULL) {
    return NULL;
  }

  char *out = quoted;
  *out++ = '\'';
  for (const char *c = text;*c != '\0';c++) {
    if (*c == '\'') {
      memcpy(out, "'\\''", 4);
      out += 4;
    }
    else {
      *out++ = *c;
    }
  }
  *out++ = '\'';
  *out = '\0';
  return quoted;
}

size_t utf8_decode(const char *text, size_t length, unsigned *codepoint) {
  const unsigned char *bytes = (const unsigned char *)text;
  unsigned char first = bytes[0];
  size_t need;
  unsigned value;
  if (first < 0x80) { *codepoint = first; return 1; }
  else if ((first & 0xE0) == 0xC0) { need = 2; value = first & 0x1F; }
  else if ((first & 0xF0) == 0xE0) { need = 3; value = first & 0x0F; }
  else if ((first & 0xF8) == 0xF0) { need = 4; value = first & 0x07; }
  else { *codepoint = 0xFFFD; return 1; }
  if (need > length) { *codepoint = 0xFFFD; return 1; }
  for (size_t i = 1;i < need;i++) {
    if ((bytes[i] & 0xC0) != 0x80) { *codepoint = 0xFFFD; return i; }
    value = (value << 6) | (bytes[i] & 0x3F);
  }
  *codepoint = value;
  return need;
}

typedef struct { unsigned first, last; } Range;

static int in_ranges(unsigned c, const Range *ranges, size_t count) {
  size_t low = 0, high = count;
  while (low < high) {
    size_t middle = (low + high) / 2;
    if (c < ranges[middle].first) high = middle;
    else if (c > ranges[middle].last) low = middle + 1;
    else return 1;
  }
  return 0;
}

// Combining marks, joiners and variation selectors (they draw on top of
// the character before them).
static const Range zero_width[] = {
  {0x0300,0x036F},{0x0483,0x0489},{0x0591,0x05BD},{0x05BF,0x05BF},{0x05C1,0x05C2},{0x05C4,0x05C5},
  {0x05C7,0x05C7},{0x0610,0x061A},{0x064B,0x065F},{0x0670,0x0670},{0x06D6,0x06DC},{0x06DF,0x06E4},
  {0x06E7,0x06E8},{0x06EA,0x06ED},{0x0711,0x0711},{0x0730,0x074A},{0x07A6,0x07B0},{0x07EB,0x07F3},
  {0x0816,0x082D},{0x0859,0x085B},{0x08D3,0x0902},{0x093A,0x093A},{0x093C,0x093C},{0x0941,0x0948},
  {0x094D,0x094D},{0x0951,0x0957},{0x0962,0x0963},{0x0981,0x0981},{0x09BC,0x09BC},{0x09C1,0x09C4},
  {0x09CD,0x09CD},{0x0A01,0x0A02},{0x0A3C,0x0A3C},{0x0A41,0x0A51},{0x0A70,0x0A71},{0x0A81,0x0A82},
  {0x0ABC,0x0ABC},{0x0AC1,0x0AC8},{0x0ACD,0x0ACD},{0x0B01,0x0B01},{0x0B3C,0x0B3C},{0x0B41,0x0B44},
  {0x0B4D,0x0B4D},{0x0BC0,0x0BC0},{0x0BCD,0x0BCD},{0x0C3E,0x0C40},{0x0C46,0x0C56},{0x0CBC,0x0CBC},
  {0x0CCC,0x0CCD},{0x0D41,0x0D44},{0x0D4D,0x0D4D},{0x0DCA,0x0DCA},{0x0DD2,0x0DD6},{0x0E31,0x0E31},
  {0x0E34,0x0E3A},{0x0E47,0x0E4E},{0x0EB1,0x0EB1},{0x0EB4,0x0EBC},{0x0EC8,0x0ECD},{0x0F18,0x0F19},
  {0x0F35,0x0F35},{0x0F37,0x0F37},{0x0F39,0x0F39},{0x0F71,0x0F7E},{0x0F80,0x0F84},{0x0F86,0x0F87},
  {0x0F8D,0x0FBC},{0x0FC6,0x0FC6},{0x102D,0x1030},{0x1032,0x1037},{0x1039,0x103A},{0x1160,0x11FF},
  {0x135D,0x135F},{0x1712,0x1714},{0x17B4,0x17B5},{0x17B7,0x17BD},{0x17C6,0x17C6},{0x17C9,0x17D3},
  {0x180B,0x180F},{0x1AB0,0x1AFF},{0x1DC0,0x1DFF},{0x200B,0x200F},{0x2028,0x202E},{0x2060,0x2064},
  {0x20D0,0x20FF},{0x2CEF,0x2CF1},{0x2DE0,0x2DFF},{0x302A,0x302D},{0x3099,0x309A},{0xA66F,0xA672},
  {0xA674,0xA67D},{0xA69E,0xA69F},{0xA6F0,0xA6F1},{0xFE00,0xFE0F},{0xFE20,0xFE2F},{0xFEFF,0xFEFF},
  {0x1D167,0x1D169},{0x1D17B,0x1D182},{0xE0001,0xE007F},{0xE0100,0xE01EF},
};

// East Asian wide and full-width characters, and emoji shown as pictures.
static const Range wide[] = {
  {0x1100,0x115F},{0x231A,0x231B},{0x2329,0x232A},{0x23E9,0x23EC},{0x23F0,0x23F0},{0x23F3,0x23F3},
  {0x25FD,0x25FE},{0x2614,0x2615},{0x2648,0x2653},{0x267F,0x267F},{0x2693,0x2693},{0x26A1,0x26A1},
  {0x26AA,0x26AB},{0x26BD,0x26BE},{0x26C4,0x26C5},{0x26CE,0x26CE},{0x26D4,0x26D4},{0x26EA,0x26EA},
  {0x26F2,0x26F3},{0x26F5,0x26F5},{0x26FA,0x26FA},{0x26FD,0x26FD},{0x2705,0x2705},{0x270A,0x270B},
  {0x2728,0x2728},{0x274C,0x274C},{0x274E,0x274E},{0x2753,0x2755},{0x2757,0x2757},{0x2795,0x2797},
  {0x27B0,0x27B0},{0x27BF,0x27BF},{0x2B1B,0x2B1C},{0x2B50,0x2B50},{0x2B55,0x2B55},{0x2E80,0x303E},
  {0x3041,0x3247},{0x3250,0x4DBF},{0x4E00,0xA4CF},{0xA960,0xA97F},{0xAC00,0xD7A3},{0xF900,0xFAFF},
  {0xFE10,0xFE19},{0xFE30,0xFE6F},{0xFF00,0xFF60},{0xFFE0,0xFFE6},{0x16FE0,0x16FE4},{0x17000,0x18CFF},
  {0x1B000,0x1B2FF},{0x1F004,0x1F004},{0x1F0CF,0x1F0CF},{0x1F18E,0x1F18E},{0x1F191,0x1F19A},
  {0x1F200,0x1F202},{0x1F210,0x1F23B},{0x1F240,0x1F248},{0x1F250,0x1F251},{0x1F260,0x1F265},
  {0x1F300,0x1F320},{0x1F32D,0x1F335},{0x1F337,0x1F37C},{0x1F37E,0x1F393},{0x1F3A0,0x1F3CA},
  {0x1F3CF,0x1F3D3},{0x1F3E0,0x1F3F0},{0x1F3F4,0x1F3F4},{0x1F3F8,0x1F43E},{0x1F440,0x1F440},
  {0x1F442,0x1F4FC},{0x1F4FF,0x1F53D},{0x1F54B,0x1F54E},{0x1F550,0x1F567},{0x1F57A,0x1F57A},
  {0x1F595,0x1F596},{0x1F5A4,0x1F5A4},{0x1F5FB,0x1F64F},{0x1F680,0x1F6C5},{0x1F6CC,0x1F6CC},
  {0x1F6D0,0x1F6D2},{0x1F6D5,0x1F6D7},{0x1F6DC,0x1F6DF},{0x1F6EB,0x1F6EC},{0x1F6F4,0x1F6FC},
  {0x1F7E0,0x1F7EB},{0x1F7F0,0x1F7F0},{0x1F90C,0x1F93A},{0x1F93C,0x1F945},{0x1F947,0x1F9FF},
  {0x1FA70,0x1FAFF},{0x20000,0x2FFFD},{0x30000,0x3FFFD},
};

int char_columns(unsigned c) {
  if (c < 0x300) return c >= 0x20 && c != 0x7F;
  if (in_ranges(c, zero_width, sizeof(zero_width) / sizeof(zero_width[0]))) return 0;
  if (in_ranges(c, wide, sizeof(wide) / sizeof(wide[0]))) return 2;
  return 1;
}

size_t escape_length(const char *text, size_t length) {
  if (length < 2) return length;
  size_t i = 2;
  if (text[1] == '[') {
    while (i < length && !((unsigned char)text[i] >= 0x40 && (unsigned char)text[i] <= 0x7E)) i++;
    return i < length ? i + 1 : length;
  }
  if (text[1] == ']') {
    // OSC (window titles, hyperlinks): ends with BEL or ESC \.
    while (i < length) {
      if (text[i] == '\a') return i + 1;
      if (text[i] == '\x1b' && i + 1 < length && text[i + 1] == '\\') return i + 2;
      i++;
    }
    return length;
  }
  return 2;
}

size_t text_columns(const char *text, size_t length) {
  size_t columns = 0;
  for (size_t i = 0;i < length;) {
    if (text[i] == '\x1b') {
      i += escape_length(text + i, length - i);
      continue;
    }
    unsigned codepoint;
    i += utf8_decode(text + i, length - i, &codepoint);
    columns += (size_t)char_columns(codepoint);
  }
  return columns;
}
