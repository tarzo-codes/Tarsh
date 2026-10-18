#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "pipehint.h"
#include "parser.h"
#include "syntax.h"

// ---------------------------------------------------------------------------
// Text building
// ---------------------------------------------------------------------------

typedef struct {
  char *text;
  size_t length, capacity;
}Text;

static void text_add(Text *t, const char *s) {
  size_t n = strlen(s);
  if (t->length + n + 1 > t->capacity) {
    size_t capacity = t->capacity ? t->capacity : 256;
    while (t->length + n + 1 > capacity) capacity *= 2;
    char *grown = realloc(t->text, capacity);
    if (!grown) return;
    t->text = grown;
    t->capacity = capacity;
  }
  memcpy(t->text + t->length, s, n + 1);
  t->length += n;
}

static void text_addf(Text *t, const char *format, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  text_add(t, buffer);
}

static char *text_take(Text *t) { return t->text ? t->text : strdup(""); }

// ---------------------------------------------------------------------------
// Regex explanation
// ---------------------------------------------------------------------------

static const char *class_name(const char *inside) {
  static const struct { const char *set; const char *name; } known[] = {
    { "0-9", "a digit" }, { "a-z", "a lowercase letter" }, { "A-Z", "an uppercase letter" },
    { "a-zA-Z", "a letter" }, { "A-Za-z", "a letter" }, { "a-zA-Z0-9", "a letter or digit" },
    { "A-Za-z0-9", "a letter or digit" }, { "a-zA-Z0-9_", "a word character" },
    { "[:digit:]", "a digit" }, { "[:alpha:]", "a letter" }, { "[:alnum:]", "a letter or digit" },
    { "[:space:]", "a space or tab" }, { "[:upper:]", "an uppercase letter" },
    { "[:lower:]", "a lowercase letter" }, { "[:punct:]", "a punctuation mark" },
    { " \\t", "a space or tab" }, { NULL, NULL }
  };
  for (int i = 0;known[i].set;i++) if (strcmp(inside, known[i].set) == 0) return known[i].name;
  return NULL;
}

// "a digit" -> "digits", "a character NOT in [x]" -> "characters NOT in [x]".
static void pluralize(const char *thing, char *out, size_t size) {
  const char *rest = thing;
  if (strncmp(thing, "a ", 2) == 0) rest = thing + 2;
  else if (strncmp(thing, "an ", 3) == 0) rest = thing + 3;
  else if (strncmp(thing, "one character from", 18) == 0) { snprintf(out, size, "characters from%s", thing + 18); return; }
  else { snprintf(out, size, "%s", thing); return; }
  const char *cut = rest + strlen(rest);
  const char *markers[] = { " NOT", " from", " (", " except", NULL };
  for (int i = 0;markers[i];i++) {
    const char *found = strstr(rest, markers[i]);
    if (found && found < cut) cut = found;
  }
  snprintf(out, size, "%.*ss%s", (int)(cut - rest), rest, cut);
}

// Appends "x", repeated as `quantifier` says.
static void add_quantified(Text *out, const char *thing, char quantifier, const char *braces) {
  int literal = thing[0] == '"' || thing[0] == '(';
  char plural[200];
  pluralize(thing, plural, sizeof(plural));
  switch (quantifier) {
    case '*':
      if (literal) text_addf(out, "%s repeated any number of times (or not at all)", thing);
      else text_addf(out, "any number of %s (or none)", plural);
      break;
    case '+':
      if (literal) text_addf(out, "%s one or more times", thing);
      else text_addf(out, "one or more %s", plural);
      break;
    case '?':
      text_addf(out, "optionally %s", thing);
      break;
    case '{': {
      int low = 0, high = -1;
      char comma = 0;
      char count[64];
      if (sscanf(braces, "%d%c%d", &low, &comma, &high) == 3) snprintf(count, sizeof(count), "%d to %d", low, high);
      else if (sscanf(braces, "%d%c", &low, &comma) == 2 && comma == ',') snprintf(count, sizeof(count), "%d or more", low);
      else snprintf(count, sizeof(count), "exactly %d", low);
      if (literal) text_addf(out, "%s repeated %s times", thing, count);
      else text_addf(out, "%s %s", count, plural);
      break;
    }
    default:
      text_add(out, thing);
      break;
  }
}

char *pipehint_regex(const char *pattern, int extended) {
  Text out = { 0 };
  Text literal = { 0 };
  size_t i = 0;
  int parts = 0;
  size_t length = strlen(pattern);

#define SEP() do { if (parts++) text_add(&out, ", then "); } while (0)
#define FLUSH() do { if (literal.length) { SEP(); text_addf(&out, "\"%s\"", literal.text); literal.length = 0; literal.text[0] = '\0'; } } while (0)

  while (i < length) {
    char c = pattern[i];
    char thing[160] = "";
    size_t next = i + 1;

    if (c == '^' && i == 0) { SEP(); text_add(&out, "the line starts"); i++; continue; }
    if (c == '$' && i == length - 1) { FLUSH(); SEP(); text_add(&out, "the line ends"); i++; continue; }

    // In basic regex, \+ \? \| \( \) \{ \} are the special ones.
    int special_escape = !extended && c == '\\' && i + 1 < length && strchr("+?|(){}", pattern[i + 1]);
    char effective = special_escape ? pattern[i + 1] : c;
    if (special_escape) next = i + 2;

    if ((extended && c == '|') || (special_escape && effective == '|')) {
      FLUSH();
      text_add(&out, ", OR ");
      parts = 0;
      i = next;
      continue;
    }
    if ((extended && c == '(') || (special_escape && effective == '(')) {
      // Find the matching group close and explain it as one thing.
      size_t depth = 1, j = next;
      while (j < length && depth) {
        if (extended) { if (pattern[j] == '\\') j++; else if (pattern[j] == '(') depth++; else if (pattern[j] == ')') depth--; }
        else if (pattern[j] == '\\' && j + 1 < length) { if (pattern[j + 1] == '(') depth++; else if (pattern[j + 1] == ')') depth--; j++; }
        j++;
      }
      size_t inner_end = extended ? j - 1 : j - 2;
      char *inner = strndup(pattern + next, inner_end > next ? inner_end - next : 0);
      char *explained = pipehint_regex(inner, extended);
      snprintf(thing, sizeof(thing), "(%s)", explained);
      free(inner);
      free(explained);
      next = j;
    }
    else if (c == '.') {
      snprintf(thing, sizeof(thing), "any character");
    }
    else if (c == '[') {
      size_t j = i + 1;
      if (j < length && pattern[j] == '^') j++;
      if (j < length && pattern[j] == ']') j++;
      while (j < length && pattern[j] != ']') { if (pattern[j] == '[' && pattern[j + 1] == ':') { j = strstr(pattern + j, ":]") ? (size_t)(strstr(pattern + j, ":]") - pattern) + 2 : j + 1; continue; } j++; }
      int negated = pattern[i + 1] == '^';
      char *inside = strndup(pattern + i + 1 + negated, j > i + 1 + negated ? j - i - 1 - negated : 0);
      const char *named = class_name(inside);
      if (named) snprintf(thing, sizeof(thing), "%s%s", negated ? "anything except " : "", named);
      else snprintf(thing, sizeof(thing), "%s [%s]", negated ? "a character NOT in" : "one character from", inside);
      free(inside);
      next = j < length ? j + 1 : j;
    }
    else if (c == '\\' && i + 1 < length && !special_escape) {
      char e = pattern[i + 1];
      next = i + 2;
      switch (e) {
        case 'd':
          if (extended == 2) snprintf(thing, sizeof(thing), "a digit");
          else snprintf(thing, sizeof(thing), "\"d\" (not a digit: only grep -P knows \\d; use [0-9])");
          break;
        case 'D':
          if (extended == 2) snprintf(thing, sizeof(thing), "a non-digit");
          else snprintf(thing, sizeof(thing), "\"D\" (only grep -P knows \\D; use [^0-9])");
          break;
        case 'w': snprintf(thing, sizeof(thing), "a word character (letter, digit, _)"); break;
        case 'W': snprintf(thing, sizeof(thing), "a non-word character"); break;
        case 's': snprintf(thing, sizeof(thing), "a space"); break;
        case 'S': snprintf(thing, sizeof(thing), "a non-space"); break;
        case 'b': FLUSH(); SEP(); text_add(&out, "a word boundary"); i = next; continue;
        case '<': FLUSH(); SEP(); text_add(&out, "a word starts"); i = next; continue;
        case '>': FLUSH(); SEP(); text_add(&out, "a word ends"); i = next; continue;
        case 't': snprintf(thing, sizeof(thing), "a tab"); break;
        default: {
          // An escaped ordinary character is itself: \. is a literal dot.
          char pair[2] = { e, 0 };
          if (i + 2 < length && (strchr("*+?", pattern[i + 2]) || pattern[i + 2] == '{')) {
            snprintf(thing, sizeof(thing), "\"%s\"", pair);
          }
          else {
            text_add(&literal, pair);
            i = next;
            continue;
          }
        }
      }
    }
    else {
      // A plain character joins the literal run, unless it's quantified.
      char quantifier = (next < length) ? pattern[next] : 0;
      int quantified = (quantifier == '*') || (extended && (quantifier == '+' || quantifier == '?' || quantifier == '{')) ||
                       (!extended && quantifier == '\\' && next + 1 < length && strchr("+?{", pattern[next + 1]));
      if (!quantified) {
        char single[2] = { c, 0 };
        text_add(&literal, single);
        i = next;
        continue;
      }
      snprintf(thing, sizeof(thing), "\"%c\"", c);
    }

    // Quantifier after the thing?
    char quantifier = 0;
    char braces[32] = "";
    if (next < length) {
      char q = pattern[next];
      if (q == '*') { quantifier = '*'; next++; }
      else if (extended && (q == '+' || q == '?')) { quantifier = q; next++; }
      else if (extended && q == '{') {
        const char *close = strchr(pattern + next, '}');
        if (close) { snprintf(braces, sizeof(braces), "%.*s", (int)(close - pattern - next - 1), pattern + next + 1); quantifier = '{'; next = close - pattern + 1; }
      }
      else if (!extended && q == '\\' && next + 1 < length && strchr("+?{", pattern[next + 1])) {
        char q2 = pattern[next + 1];
        if (q2 == '{') {
          const char *close = strstr(pattern + next, "\\}");
          if (close) { snprintf(braces, sizeof(braces), "%.*s", (int)(close - pattern - next - 2), pattern + next + 2); quantifier = '{'; next = close - pattern + 2; }
        }
        else { quantifier = q2; next += 2; }
      }
    }
    FLUSH();
    SEP();
    if (strcmp(thing, "any character") == 0 && quantifier == '*') text_add(&out, "anything");
    else add_quantified(&out, thing, quantifier, braces);
    i = next;
  }
  FLUSH();
#undef SEP
#undef FLUSH
  free(literal.text);
  if (out.length == 0) text_add(&out, "an empty pattern (matches every line)");
  return text_take(&out);
}

// ---------------------------------------------------------------------------
// Splitting a command line into stages
// ---------------------------------------------------------------------------

typedef enum { TOK_WORD, TOK_OP }TokKind;

typedef struct {
  TokKind kind;
  char *text;
}Tok;

typedef struct {
  ArgList words;        // unquoted
  ArgList raws;         // as typed
  ArgList redirects;    // "> file", ">> file", "< file", "2>&1"... (op and target)
  char *connector;      // what follows this stage: "|", "&&", "||", ";", "&" or NULL
}Stage;

static const char *operators[] = {
  "||", "|&", "|", "&&", "&>>", "&>", "&", ";;", ";", "<<<", "<<-", "<<", "<&", "<", ">>", ">&", ">|", ">", NULL
};

static int tokenize_line(const char *line, Tok **out) {
  Tok *tokens = NULL;
  int count = 0;
  size_t i = 0;
  while (line[i]) {
    if (line[i] == ' ' || line[i] == '\t' || line[i] == '\n') { i++; continue; }
    if (line[i] == '#' && (i == 0 || line[i - 1] == ' ')) break;
    const char *op = NULL;
    for (int k = 0;operators[k];k++) {
      if (strncmp(line + i, operators[k], strlen(operators[k])) == 0) { op = operators[k]; break; }
    }
    tokens = realloc(tokens, (count + 1) * sizeof(Tok));
    if (op) {
      tokens[count].kind = TOK_OP;
      tokens[count].text = strdup(op);
      i += strlen(op);
    }
    else {
      int unterminated = 0;
      size_t end = syntax_word_end(line, i, &unterminated);
      if (end == i) end = i + 1;
      // 2>file, 2>&1: digits glued to a redirection.
      if ((line[end] == '>' || line[end] == '<') && end - i <= 2 && line[i] >= '0' && line[i] <= '9') {
        size_t op_end = end;
        while (line[op_end] == '>' || line[op_end] == '<' || line[op_end] == '&') op_end++;
        tokens[count].kind = TOK_OP;
        tokens[count].text = strndup(line + i, op_end - i);
        i = op_end;
        // 2>&1 keeps its target glued on
        if (line[i] >= '0' && line[i] <= '9' && tokens[count].text[strlen(tokens[count].text) - 1] == '&') {
          char *glued = malloc(strlen(tokens[count].text) + 2);
          sprintf(glued, "%s%c", tokens[count].text, line[i]);
          free(tokens[count].text);
          tokens[count].text = glued;
          i++;
        }
      }
      else {
        tokens[count].kind = TOK_WORD;
        tokens[count].text = strndup(line + i, end - i);
        i = end;
      }
    }
    count++;
  }
  *out = tokens;
  return count;
}

static char *unquote(const char *raw) {
  ArgList words;
  arglist_init(&words);
  char *result = NULL;
  if (parser_split_words(raw, &words) > 0) {
    // A raw word can't split into several, but join just in case.
    size_t length = 1;
    for (int i = 0;i < words.count;i++) length += strlen(words.items[i]) + 1;
    result = calloc(length, 1);
    for (int i = 0;i < words.count;i++) { if (i) strcat(result, " "); strcat(result, words.items[i]); }
  }
  arglist_free(&words);
  return result ? result : strdup(raw);
}

static int is_redirect_op(const char *op) {
  return strchr(op, '>') != NULL || strchr(op, '<') != NULL;
}

static int split_stages(const char *line, Stage **out) {
  Tok *tokens = NULL;
  int count = tokenize_line(line, &tokens);
  Stage *stages = NULL;
  int stage_count = 0;
  Stage *current = NULL;

  for (int i = 0;i < count;i++) {
    if (current == NULL) {
      stages = realloc(stages, (stage_count + 1) * sizeof(Stage));
      current = &stages[stage_count++];
      arglist_init(&current->words);
      arglist_init(&current->raws);
      arglist_init(&current->redirects);
      current->connector = NULL;
    }
    Tok *t = &tokens[i];
    if (t->kind == TOK_WORD) {
      char *word = unquote(t->text);
      arglist_append(&current->words, word);
      arglist_append(&current->raws, t->text);
      free(word);
    }
    else if (is_redirect_op(t->text)) {
      // Redirection with its target (if typed yet).
      char joined[512];
      int has_target = strstr(t->text, ">&") && t->text[strlen(t->text) - 1] != '&';
      if (!has_target && i + 1 < count && tokens[i + 1].kind == TOK_WORD) {
        char *target = unquote(tokens[i + 1].text);
        snprintf(joined, sizeof(joined), "%s %s", t->text, target);
        free(target);
        i++;
      }
      else {
        snprintf(joined, sizeof(joined), "%s", t->text);
      }
      arglist_append(&current->redirects, joined);
    }
    else {
      current->connector = strdup(t->text);
      current = NULL;
    }
  }
  for (int i = 0;i < count;i++) free(tokens[i].text);
  free(tokens);
  *out = stages;
  return stage_count;
}

static void free_stages(Stage *stages, int count) {
  for (int i = 0;i < count;i++) {
    arglist_free(&stages[i].words);
    arglist_free(&stages[i].raws);
    arglist_free(&stages[i].redirects);
    free(stages[i].connector);
  }
  free(stages);
}

// ---------------------------------------------------------------------------
// Describing commands
// ---------------------------------------------------------------------------

typedef struct {
  char action[320];   // "keeps lines matching /x/"
  char output[160];   // "matching lines"
  char regex[400];    // regex explanation, if any
  char warning[200];  // "edits the file in place"
}Desc;

// Short flag `c` present (in -abc groups) or the long one?
static int has_flag(const ArgList *w, char c, const char *long_name) {
  for (int i = 1;i < w->count;i++) {
    const char *a = w->items[i];
    if (strcmp(a, "--") == 0) break;
    if (long_name && strcmp(a, long_name) == 0) return 1;
    if (a[0] == '-' && a[1] != '-' && a[1] && c && strchr(a + 1, c) && !(a[1] >= '0' && a[1] <= '9')) return 1;
  }
  return 0;
}

// Value of -X VALUE / -XVALUE / --long=VALUE / --long VALUE.
static const char *option_value(const ArgList *w, char c, const char *long_name) {
  for (int i = 1;i < w->count;i++) {
    const char *a = w->items[i];
    if (c && a[0] == '-' && a[1] == c && a[2] == '\0') return i + 1 < w->count ? w->items[i + 1] : NULL;
    if (c && a[0] == '-' && a[1] == c && a[2] != '\0' && a[1] != '-') return a + 2;
    if (long_name) {
      size_t n = strlen(long_name);
      if (strncmp(a, long_name, n) == 0 && a[n] == '=') return a + n + 1;
      if (strcmp(a, long_name) == 0) return i + 1 < w->count ? w->items[i + 1] : NULL;
    }
  }
  return NULL;
}

// The n-th argument that isn't an option (skipping the values of options
// listed in `takes_value`, e.g. "efm" for grep -e/-f/-m).
static const char *positional(const ArgList *w, int n, const char *takes_value) {
  int seen = 0;
  int options_done = 0;
  for (int i = 1;i < w->count;i++) {
    const char *a = w->items[i];
    if (!options_done && strcmp(a, "--") == 0) { options_done = 1; continue; }
    if (!options_done && a[0] == '-' && a[1]) {
      if (a[1] != '-' && a[2] == '\0' && takes_value && strchr(takes_value, a[1])) i++;
      continue;
    }
    if (seen++ == n) return a;
  }
  return NULL;
}

static void files_phrase(char *out, size_t size, const ArgList *w, int from, const char *takes_value) {
  out[0] = '\0';
  const char *first = positional(w, from, takes_value);
  const char *second = positional(w, from + 1, takes_value);
  if (!first) return;
  if (second) snprintf(out, size, " in %s and more", first);
  else snprintf(out, size, " in %s", first);
}

static void describe(const ArgList *w, int has_input, Desc *d) {
  memset(d, 0, sizeof(*d));
  const char *name = w->count ? w->items[0] : "";
  char files[200];
  if (strcmp(name, "sudo") == 0 && w->count > 1) {
    ArgList rest;
    arglist_init(&rest);
    for (int i = 1;i < w->count;i++) arglist_append(&rest, w->items[i]);
    describe(&rest, has_input, d);
    arglist_free(&rest);
    char action[320];
    snprintf(action, sizeof(action), "as administrator: %.290s", d->action);
    snprintf(d->action, sizeof(d->action), "%s", action);
    return;
  }

  if (!strcmp(name, "grep") || !strcmp(name, "egrep") || !strcmp(name, "fgrep") || !strcmp(name, "rg")) {
    int perl = !strcmp(name, "rg") || has_flag(w, 'P', "--perl-regexp");
    int extended = perl ? 2 : (!strcmp(name, "egrep") || has_flag(w, 'E', "--extended-regexp"));
    int fixed = !strcmp(name, "fgrep") || has_flag(w, 'F', "--fixed-strings");
    const char *pattern = option_value(w, 'e', "--regexp");
    int file_index = 0;
    if (!pattern) { pattern = positional(w, 0, "efmABCd"); file_index = 1; }
    files_phrase(files, sizeof(files), w, file_index, "efmABCd");
    if (has_flag(w, 'r', "--recursive") || has_flag(w, 'R', NULL) || !strcmp(name, "rg")) {
      const char *where = positional(w, file_index, "efmABCd");
      snprintf(files, sizeof(files), " in every file under %s", where ? where : "this folder");
    }
    const char *invert = has_flag(w, 'v', "--invert-match") ? "NOT " : "";
    const char *caseless = has_flag(w, 'i', "--ignore-case") ? ", ignoring case" : "";
    const char *word = has_flag(w, 'w', "--word-regexp") ? " as a whole word" : has_flag(w, 'x', "--line-regexp") ? " as the whole line" : "";
    if (!pattern) { snprintf(d->action, sizeof(d->action), "searches for a pattern (type it next)"); snprintf(d->output, sizeof(d->output), "matching lines"); return; }
    if (has_flag(w, 'c', "--count")) {
      snprintf(d->action, sizeof(d->action), "counts lines %smatching /%s/%s%s%s", invert, pattern, word, caseless, files);
      snprintf(d->output, sizeof(d->output), "a number");
    }
    else if (has_flag(w, 'l', "--files-with-matches")) {
      snprintf(d->action, sizeof(d->action), "lists the files containing /%s/%s%s", pattern, caseless, files);
      snprintf(d->output, sizeof(d->output), "file names");
    }
    else if (has_flag(w, 'o', "--only-matching")) {
      snprintf(d->action, sizeof(d->action), "prints just the parts matching /%s/%s%s", pattern, caseless, files);
      snprintf(d->output, sizeof(d->output), "the matched text, one per line");
    }
    else {
      snprintf(d->action, sizeof(d->action), "keeps lines %smatching /%s/%s%s%s%s", invert, pattern, word, caseless, files,
               has_flag(w, 'n', "--line-number") ? " (with line numbers)" : "");
      snprintf(d->output, sizeof(d->output), "%s", invert[0] ? "the lines that don't match" : "the matching lines");
    }
    if (!fixed && !perl && (strstr(pattern, "\\d") || strstr(pattern, "\\D"))) {
      // A classic surprise: grep and grep -E don't know \d.
      size_t used = strlen(d->action);
      snprintf(d->action + used, sizeof(d->action) - used, " (careful: \\d isn't a digit here: use [0-9], or grep -P)");
    }
    if (!fixed) {
      char *explained = pipehint_regex(pattern, extended);
      snprintf(d->regex, sizeof(d->regex), "/%s/ means: %s", pattern, explained);
      free(explained);
    }
    return;
  }

  if (!strcmp(name, "awk") || !strcmp(name, "gawk") || !strcmp(name, "mawk")) {
    const char *separator = option_value(w, 'F', NULL);
    const char *program = positional(w, 0, "Fvf");
    files_phrase(files, sizeof(files), w, 1, "Fvf");
    char split[64] = "";
    if (separator) snprintf(split, sizeof(split), ", splitting columns on '%s'", separator);
    if (!program) { snprintf(d->action, sizeof(d->action), "runs an awk program (type it next)"); return; }
    char columns[128] = "";
    // {print $2} / {print $1, $3}
    const char *print = strstr(program, "print");
    if (print) {
      int n, used = 0;
      const char *p = print + 5;
      while (*p) {
        if (*p == '$' && sscanf(p + 1, "%d", &n) == 1) {
          used += snprintf(columns + used, sizeof(columns) - used, "%s%d", used ? ", " : "", n);
          p++;
          while (*p >= '0' && *p <= '9') p++;
          continue;
        }
        if (*p == '}' || *p == ';') break;
        p++;
      }
    }
    const char *slash = strchr(program, '/');
    char filter[200] = "";
    if (slash && program[0] == '/') {
      const char *end = strchr(slash + 1, '/');
      if (end) {
        snprintf(filter, sizeof(filter), "lines matching /%.*s/, ", (int)(end - slash - 1), slash + 1);
        char *pattern = strndup(slash + 1, end - slash - 1);
        char *explained = pipehint_regex(pattern, 1);
        snprintf(d->regex, sizeof(d->regex), "/%s/ means: %s", pattern, explained);
        free(pattern);
        free(explained);
      }
    }
    if (strstr(program, "NR>1") || strstr(program, "NR > 1")) snprintf(filter + strlen(filter), sizeof(filter) - strlen(filter), "skipping the first line, ");
    if (strstr(program, "END") && strstr(program, "+=")) {
      int column = 0;
      const char *plus = strstr(program, "+=");
      const char *dollar = strchr(plus, '$');
      if (dollar) sscanf(dollar + 1, "%d", &column);
      snprintf(d->action, sizeof(d->action), "adds up column %d%s and prints the total%s", column, split, files);
      snprintf(d->output, sizeof(d->output), "a number");
    }
    else if (columns[0]) {
      snprintf(d->action, sizeof(d->action), "%sprints column%s %s%s%s", filter, strchr(columns, ',') ? "s" : "", columns, split, files);
      snprintf(d->output, sizeof(d->output), "column%s %s of each line", strchr(columns, ',') ? "s" : "", columns);
    }
    else if (filter[0]) {
      filter[strlen(filter) - 2] = '\0';
      snprintf(d->action, sizeof(d->action), "keeps %s%s", filter, files);
      snprintf(d->output, sizeof(d->output), "the matching lines");
    }
    else {
      snprintf(d->action, sizeof(d->action), "runs the awk program '%s' on each line%s%s", program, split, files);
      snprintf(d->output, sizeof(d->output), "whatever the program prints");
    }
    return;
  }

  if (!strcmp(name, "sed")) {
    int extended = has_flag(w, 'E', "--regexp-extended") || has_flag(w, 'r', NULL);
    const char *script = option_value(w, 'e', "--expression");
    int file_index = script ? 0 : 1;
    if (!script) script = positional(w, 0, "ef");
    files_phrase(files, sizeof(files), w, file_index, "ef");
    if (has_flag(w, 'i', "--in-place")) snprintf(d->warning, sizeof(d->warning), "-i changes the file itself (no undo)");
    if (!script) { snprintf(d->action, sizeof(d->action), "edits text with a sed script (type it next)"); return; }
    if (script[0] == 's' && script[1] && !(script[1] >= 'a' && script[1] <= 'z')) {
      char delim = script[1];
      const char *from = script + 2;
      const char *mid = strchr(from, delim);
      const char *end = mid ? strchr(mid + 1, delim) : NULL;
      if (mid) {
        char *pattern = strndup(from, mid - from);
        char *replacement = end ? strndup(mid + 1, end - mid - 1) : strdup(mid + 1);
        int global = end && strchr(end + 1, 'g') != NULL;
        snprintf(d->action, sizeof(d->action), "replaces /%s/ with \"%s\" (%s)%s", pattern, replacement,
                 global ? "every match on each line" : "the first match on each line", files);
        snprintf(d->output, sizeof(d->output), "the text with the replacements made");
        char *explained = pipehint_regex(pattern, extended);
        snprintf(d->regex, sizeof(d->regex), "/%s/ means: %s", pattern, explained);
        free(explained);
        free(pattern);
        free(replacement);
        return;
      }
    }
    if (script[0] == '/' && strchr(script + 1, '/')) {
      const char *end = strchr(script + 1, '/');
      char command = end[1];
      char *pattern = strndup(script + 1, end - script - 1);
      if (command == 'd') snprintf(d->action, sizeof(d->action), "deletes lines matching /%s/%s", pattern, files);
      else if (command == 'p') snprintf(d->action, sizeof(d->action), "%s lines matching /%s/%s", has_flag(w, 'n', NULL) ? "prints only" : "doubles", pattern, files);
      else snprintf(d->action, sizeof(d->action), "edits lines matching /%s/%s", pattern, files);
      snprintf(d->output, sizeof(d->output), command == 'd' ? "the other lines" : "the matching lines");
      char *explained = pipehint_regex(pattern, extended);
      snprintf(d->regex, sizeof(d->regex), "/%s/ means: %s", pattern, explained);
      free(explained);
      free(pattern);
      return;
    }
    int line_number;
    char command;
    if (sscanf(script, "%d%c", &line_number, &command) == 2 && command == 'd') {
      snprintf(d->action, sizeof(d->action), "deletes line %d%s", line_number, files);
      snprintf(d->output, sizeof(d->output), "the other lines");
      return;
    }
    snprintf(d->action, sizeof(d->action), "edits the text with the sed script '%s'%s", script, files);
    snprintf(d->output, sizeof(d->output), "the edited text");
    return;
  }

  if (!strcmp(name, "cut")) {
    const char *delim = option_value(w, 'd', "--delimiter");
    const char *fields = option_value(w, 'f', "--fields");
    const char *chars = option_value(w, 'c', "--characters");
    files_phrase(files, sizeof(files), w, 0, "dfcb");
    if (fields) {
      snprintf(d->action, sizeof(d->action), "takes field %s of each line, splitting on %s%s%s%s", fields,
               delim ? "'" : "tabs", delim ? delim : "", delim ? "'" : "", files);
      snprintf(d->output, sizeof(d->output), "field %s of each line", fields);
    }
    else if (chars) {
      snprintf(d->action, sizeof(d->action), "takes characters %s of each line%s", chars, files);
      snprintf(d->output, sizeof(d->output), "characters %s of each line", chars);
    }
    else snprintf(d->action, sizeof(d->action), "cuts out parts of each line (add -f or -c)");
    return;
  }

  if (!strcmp(name, "sort")) {
    char how[200] = "";
    size_t used = 0;
    if (has_flag(w, 'n', "--numeric-sort")) used += snprintf(how + used, sizeof(how) - used, " as numbers");
    if (has_flag(w, 'h', "--human-numeric-sort")) used += snprintf(how + used, sizeof(how) - used, " by size (2K, 1G)");
    const char *key = option_value(w, 'k', "--key");
    if (key) used += snprintf(how + used, sizeof(how) - used, " by column %s", key);
    if (has_flag(w, 'r', "--reverse")) used += snprintf(how + used, sizeof(how) - used, ", biggest/last first");
    if (has_flag(w, 'u', "--unique")) used += snprintf(how + used, sizeof(how) - used, ", dropping duplicates");
    files_phrase(files, sizeof(files), w, 0, "ktoS");
    snprintf(d->action, sizeof(d->action), "sorts the lines%s%s", how, files);
    snprintf(d->output, sizeof(d->output), "the same lines, sorted");
    return;
  }

  if (!strcmp(name, "uniq")) {
    if (has_flag(w, 'c', "--count")) {
      snprintf(d->action, sizeof(d->action), "collapses repeated lines and counts them");
      snprintf(d->output, sizeof(d->output), "each line once, with how many times it appeared");
    }
    else if (has_flag(w, 'd', "--repeated")) {
      snprintf(d->action, sizeof(d->action), "keeps only lines that repeat");
      snprintf(d->output, sizeof(d->output), "the repeated lines");
    }
    else {
      snprintf(d->action, sizeof(d->action), "drops repeated lines");
      snprintf(d->output, sizeof(d->output), "each line once");
    }
    return;
  }

  if (!strcmp(name, "head") || !strcmp(name, "tail")) {
    const char *count = option_value(w, 'n', "--lines");
    char number[32] = "10";
    if (count) snprintf(number, sizeof(number), "%s", count);
    for (int i = 1;i < w->count;i++) if (w->items[i][0] == '-' && w->items[i][1] >= '0' && w->items[i][1] <= '9') snprintf(number, sizeof(number), "%s", w->items[i] + 1);
    files_phrase(files, sizeof(files), w, 0, "nc");
    int first = !strcmp(name, "head");
    if (!first && has_flag(w, 'f', "--follow")) {
      snprintf(d->action, sizeof(d->action), "shows the end%s and keeps showing new lines as they're added (Ctrl+C stops)", files);
      snprintf(d->output, sizeof(d->output), "new lines as they arrive");
      return;
    }
    snprintf(d->action, sizeof(d->action), "keeps the %s %s line%s%s", first ? "first" : "last", number, strcmp(number, "1") ? "s" : "", files);
    snprintf(d->output, sizeof(d->output), "the %s %s line%s", first ? "first" : "last", number, strcmp(number, "1") ? "s" : "");
    return;
  }

  if (!strcmp(name, "wc")) {
    const char *what = has_flag(w, 'l', "--lines") ? "lines" : has_flag(w, 'w', "--words") ? "words" :
                       has_flag(w, 'c', "--bytes") ? "bytes" : has_flag(w, 'm', "--chars") ? "characters" : NULL;
    files_phrase(files, sizeof(files), w, 0, NULL);
    if (what) {
      snprintf(d->action, sizeof(d->action), "counts the %s%s", what, files);
      snprintf(d->output, sizeof(d->output), "a number");
    }
    else {
      snprintf(d->action, sizeof(d->action), "counts lines, words and bytes%s", files);
      snprintf(d->output, sizeof(d->output), "three numbers: lines, words, bytes");
    }
    return;
  }

  if (!strcmp(name, "tr")) {
    const char *a = positional(w, 0, NULL), *b = positional(w, 1, NULL);
    if (has_flag(w, 'd', "--delete") && a) snprintf(d->action, sizeof(d->action), "deletes the characters '%s'", a);
    else if (has_flag(w, 's', "--squeeze-repeats") && a && !b) snprintf(d->action, sizeof(d->action), "squeezes repeated '%s' into one", a);
    else if (a && b) snprintf(d->action, sizeof(d->action), "turns '%s' into '%s'", a, b);
    else snprintf(d->action, sizeof(d->action), "translates characters");
    snprintf(d->output, sizeof(d->output), "the changed text");
    return;
  }

  if (!strcmp(name, "jq")) {
    const char *filter = positional(w, 0, "f");
    files_phrase(files, sizeof(files), w, 1, "f");
    if (filter && strcmp(filter, ".") == 0) snprintf(d->action, sizeof(d->action), "pretty-prints the JSON%s", files);
    else if (filter) snprintf(d->action, sizeof(d->action), "picks %s out of the JSON%s", filter, files);
    else snprintf(d->action, sizeof(d->action), "filters JSON (type a filter like .name next)");
    snprintf(d->output, sizeof(d->output), has_flag(w, 'r', "--raw-output") ? "plain text values" : "JSON");
    return;
  }

  if (!strcmp(name, "find")) {
    const char *where = positional(w, 0, NULL);
    const char *named = option_value(w, 0, "-name");
    if (!named) named = option_value(w, 0, "-iname");
    const char *type = option_value(w, 0, "-type");
    char what[160] = "files and folders";
    if (type && !strcmp(type, "f")) snprintf(what, sizeof(what), "files");
    if (type && !strcmp(type, "d")) snprintf(what, sizeof(what), "folders");
    if (named) snprintf(what + strlen(what), sizeof(what) - strlen(what), " named %s", named);
    snprintf(d->action, sizeof(d->action), "lists %s under %s", what, (where && where[0] != '-') ? where : ".");
    snprintf(d->output, sizeof(d->output), "paths, one per line");
    for (int i = 1;i < w->count;i++) {
      if (!strcmp(w->items[i], "-delete")) snprintf(d->warning, sizeof(d->warning), "-delete removes everything it finds");
      if (!strcmp(w->items[i], "-exec")) snprintf(d->warning, sizeof(d->warning), "-exec runs a command on everything it finds");
    }
    return;
  }

  if (!strcmp(name, "xargs")) {
    const char *command = positional(w, 0, "nIdLP");
    snprintf(d->action, sizeof(d->action), "runs %s with the incoming lines as its arguments", command ? command : "a command");
    snprintf(d->output, sizeof(d->output), "whatever %s prints", command ? command : "it");
    return;
  }
  if (!strcmp(name, "tee")) {
    const char *file = positional(w, 0, NULL);
    snprintf(d->action, sizeof(d->action), "%s a copy to %s and passes the text on", has_flag(w, 'a', "--append") ? "adds" : "writes", file ? file : "a file");
    snprintf(d->output, sizeof(d->output), "the same text");
    return;
  }

  struct { const char *command; const char *action; const char *output; } simple[] = {
    { "ls", "lists files", "file names" }, { "cat", "prints", "the text" }, { "echo", "prints", "the text" },
    { "printf", "prints formatted text", "the text" }, { "less", "lets you scroll through it", "a scrollable view" },
    { "more", "shows it a page at a time", "a paged view" }, { "column", "lines up columns", "a neat table" },
    { "nl", "numbers the lines", "numbered lines" }, { "rev", "reverses each line", "reversed lines" },
    { "tac", "reverses the order of lines", "the lines, last first" }, { "seq", "counts", "numbers, one per line" },
    { "ps", "lists running programs", "a table of processes" }, { "du", "measures folder sizes", "sizes" },
    { "df", "shows free disk space", "a table of disks" }, { "date", "shows the date and time", "a date" },
    { "curl", "downloads", "the downloaded data" }, { "wget", "downloads to a file", "progress messages" },
    { "diff", "compares", "the differences" }, { "file", "says what kind of file it is", "file types" },
    { "basename", "takes the file name from a path", "a name" }, { "dirname", "takes the folder from a path", "a folder" },
    { "realpath", "turns a path into a full path", "a path" }, { "env", "lists environment variables", "NAME=value lines" },
    { "history", "lists your past commands", "numbered commands" }, { "fold", "wraps long lines", "wrapped lines" },
    { "paste", "joins lines side by side", "joined lines" }, { "base64", "encodes as base64", "base64 text" },
    { "md5sum", "computes checksums", "checksums" }, { "sha256sum", "computes checksums", "checksums" },
    { "string", "works on text (string ...)", "text" }, { "math", "does arithmetic", "a number" },
    { "count", "counts its arguments or input lines", "a number" }, { "git", "runs git", "git's output" },
    { "docker", "runs docker", "docker's output" }, { "python3", "runs Python", "the program's output" },
    { NULL, NULL, NULL }
  };
  for (int i = 0;simple[i].command;i++) {
    if (strcmp(name, simple[i].command) == 0) {
      const char *what = positional(w, 0, "n");
      if (!strcmp(name, "ls")) snprintf(d->action, sizeof(d->action), "lists files in %s%s%s", what ? what : "this folder",
                                         has_flag(w, 'l', NULL) ? " (with details)" : "", has_flag(w, 'a', "--all") ? " (including hidden ones)" : "");
      else if (!strcmp(name, "cat") && what) snprintf(d->action, sizeof(d->action), "prints the contents of %s%s", what, positional(w, 1, NULL) ? " and the others" : "");
      else if (!strcmp(name, "cat")) snprintf(d->action, sizeof(d->action), has_input ? "passes the text through" : "prints what it reads");
      else if (!strcmp(name, "echo") || !strcmp(name, "printf")) {
        // Show the text being printed (up to a point).
        char shown[120] = "";
        for (int k = 1, used = 0;k < w->count && used < 100;k++) used += snprintf(shown + used, sizeof(shown) - used, "%s%s", used ? " " : "", w->items[k]);
        snprintf(d->action, sizeof(d->action), "prints \"%s\"", shown);
      }
      else if (!strcmp(name, "seq") && positional(w, 1, NULL)) snprintf(d->action, sizeof(d->action), "counts from %s to %s", positional(w, 0, NULL), positional(w, 1, NULL));
      else if (!strcmp(name, "git") && what) snprintf(d->action, sizeof(d->action), "runs git %s", what);
      else if ((!strcmp(name, "curl") || !strcmp(name, "wget")) && what) snprintf(d->action, sizeof(d->action), "downloads %s", what);
      else snprintf(d->action, sizeof(d->action), "%s", simple[i].action);
      snprintf(d->output, sizeof(d->output), "%s", simple[i].output);
      return;
    }
  }
  snprintf(d->action, sizeof(d->action), "runs %s", name);
}

// ---------------------------------------------------------------------------
// Operators
// ---------------------------------------------------------------------------

static void describe_redirect(const char *redirect, char *out, size_t size, char *result_note, size_t note_size) {
  char op[16];
  char target[256] = "";
  sscanf(redirect, "%15s %255[^\n]", op, target);
  const char *file = target[0] ? target : "(a file)";
  if (!strcmp(op, ">") || !strcmp(op, ">|") || !strcmp(op, "1>")) {
    snprintf(out, size, "> %s: saves the output in %s, replacing what was there", target, file);
    snprintf(result_note, note_size, "saved in %s (nothing on screen)", file);
  }
  else if (!strcmp(op, ">>") || !strcmp(op, "1>>")) {
    snprintf(out, size, ">> %s: adds the output to the end of %s", target, file);
    snprintf(result_note, note_size, "added to the end of %s (nothing on screen)", file);
  }
  else if (!strcmp(op, "<")) snprintf(out, size, "< %s: reads its input from %s", target, file);
  else if (!strcmp(op, "<<") || !strcmp(op, "<<-")) snprintf(out, size, "<< %s: the lines you type next, up to %s, become the input (a here-document)", target, target[0] ? target : "the end word");
  else if (!strcmp(op, "<<<")) snprintf(out, size, "<<< %s: that text becomes the input", target);
  else if (!strcmp(op, "2>")) {
    if (!strcmp(target, "/dev/null")) snprintf(out, size, "2> /dev/null: throws away error messages");
    else snprintf(out, size, "2> %s: saves error messages in %s", target, file);
  }
  else if (!strcmp(op, "2>>")) snprintf(out, size, "2>> %s: adds error messages to %s", target, file);
  else if (!strcmp(op, "2>&1")) snprintf(out, size, "2>&1: error messages go wherever the output goes");
  else if (!strcmp(op, ">&2") || !strcmp(op, "1>&2")) snprintf(out, size, ">&2: the output goes to the error stream");
  else if (!strcmp(op, "&>") || !strcmp(op, ">&")) {
    snprintf(out, size, "%s %s: saves output and errors in %s", op, target, file);
    snprintf(result_note, note_size, "saved in %s with any errors (nothing on screen)", file);
  }
  else if (!strcmp(op, "&>>")) {
    snprintf(out, size, "&>> %s: adds output and errors to %s", target, file);
    snprintf(result_note, note_size, "added to %s with any errors (nothing on screen)", file);
  }
  else snprintf(out, size, "%s: a redirection", redirect);
}

static const char *connector_text(const char *op) {
  if (!strcmp(op, "|")) return "| passes that output on as the input of";
  if (!strcmp(op, "|&")) return "|& passes output and errors on to";
  if (!strcmp(op, "&&")) return "&& if that worked, then";
  if (!strcmp(op, "||")) return "|| if that failed, then";
  if (!strcmp(op, ";")) return "; then, either way,";
  if (!strcmp(op, "&")) return "& runs that in the background, then";
  return op;
}

static int worth_explaining(Stage *stages, int count) {
  if (count == 0 || stages[0].words.count == 0) return 0;
  for (int i = 0;i < count;i++) {
    if (stages[i].connector || stages[i].redirects.count) return 1;
  }
  return 0;
}

// The words of a stage as typed, for the detail view.
static void stage_text(const Stage *s, char *out, size_t size) {
  size_t used = 0;
  out[0] = '\0';
  for (int i = 0;i < s->raws.count && used < size - 4;i++) {
    used += snprintf(out + used, size - used, "%s%s", i ? " " : "", s->raws.items[i]);
  }
}

char *pipehint_summary(const char *line) {
  Stage *stages = NULL;
  int count = split_stages(line, &stages);
  if (!worth_explaining(stages, count)) {
    free_stages(stages, count);
    return NULL;
  }
  Text out = { 0 };
  char result[200] = "";
  for (int i = 0;i < count;i++) {
    Stage *s = &stages[i];
    if (s->words.count == 0) continue;
    int has_input = i > 0 && stages[i - 1].connector && stages[i - 1].connector[0] == '|';
    Desc d;
    describe(&s->words, has_input, &d);
    text_addf(&out, "%s: %s", s->words.items[0], d.action);
    if (d.warning[0]) text_addf(&out, " (careful: %s)", d.warning);
    if (d.output[0]) snprintf(result, sizeof(result), "%s", d.output);
    for (int r = 0;r < s->redirects.count;r++) {
      char said[400], note[200] = "";
      describe_redirect(s->redirects.items[r], said, sizeof(said), note, sizeof(note));
      text_addf(&out, ", %s", strchr(said, ':') ? strchr(said, ':') + 2 : said);
      if (note[0]) snprintf(result, sizeof(result), "%s", note);
    }
    if (s->connector) {
      if (!strcmp(s->connector, "|") || !strcmp(s->connector, "|&")) text_add(&out, " → ");
      else if (!strcmp(s->connector, "&&")) text_add(&out, " → if that worked: ");
      else if (!strcmp(s->connector, "||")) text_add(&out, " → if that failed: ");
      else if (!strcmp(s->connector, "&")) text_add(&out, " (in the background) → ");
      else text_add(&out, " → then: ");
      if (i == count - 1) text_add(&out, "(type the next command)");
    }
  }
  if (result[0] && !(count && stages[count - 1].connector)) text_addf(&out, "  ⇒ %s", result);
  free_stages(stages, count);
  return text_take(&out);
}

char *pipehint_detail(const char *line) {
  Stage *stages = NULL;
  int count = split_stages(line, &stages);
  Text out = { 0 };
  if (count == 0 || stages[0].words.count == 0) {
    free_stages(stages, count);
    return strdup("nothing to explain: give a command line, e.g.  explain 'ls -l | grep txt | wc -l'\n");
  }
  char result[200] = "";
  int step = 0;
  for (int i = 0;i < count;i++) {
    Stage *s = &stages[i];
    if (s->words.count == 0) continue;
    int has_input = i > 0 && stages[i - 1].connector && stages[i - 1].connector[0] == '|';
    Desc d;
    describe(&s->words, has_input, &d);
    char typed[300];
    stage_text(s, typed, sizeof(typed));
    text_addf(&out, "%d. %s\n     %s\n", ++step, typed, d.action);
    if (d.regex[0]) text_addf(&out, "     %s\n", d.regex);
    if (d.warning[0]) text_addf(&out, "     careful: %s\n", d.warning);
    if (d.output[0]) snprintf(result, sizeof(result), "%s", d.output);
    for (int r = 0;r < s->redirects.count;r++) {
      char said[400], note[200] = "";
      describe_redirect(s->redirects.items[r], said, sizeof(said), note, sizeof(note));
      text_addf(&out, "     %s\n", said);
      if (note[0]) snprintf(result, sizeof(result), "%s", note);
    }
    if (s->connector) text_addf(&out, "   %s\n", connector_text(s->connector));
  }
  int conditional = 0;
  for (int i = 0;i < count;i++) {
    if (stages[i].connector && (!strcmp(stages[i].connector, "&&") || !strcmp(stages[i].connector, "||"))) conditional = 1;
  }
  if (conditional) text_add(&out, "result: depends on which commands run (&& and || decide)\n");
  else if (result[0]) text_addf(&out, "result: %s\n", result);
  free_stages(stages, count);
  return text_take(&out);
}
