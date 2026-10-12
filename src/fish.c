#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <fnmatch.h>
#include <regex.h>
#include <time.h>
#include <sys/stat.h>

#include "fish.h"
#include "builtins.h"
#include "config.h"
#include "exec.h"
#include "fzf.h"
#include "jobs.h"
#include "script.h"
#include "util.h"

static const char *fish_names[] = {
  "abbr", "set", "string", "math", "count", "contains", "random",
  "funcsave", "funced", "functions", "prevd", "nextd", "dirh", "cdh", NULL
};

int fish_builtin_exists(const char *name) {
  for (int i = 0;fish_names[i];i++) {
    if (strcmp(fish_names[i], name) == 0) return 1;
  }
  return 0;
}

void fish_write_builtin_names(ArgList *out) {
  for (int i = 0;fish_names[i];i++) arglist_append(out, fish_names[i]);
}

// ---------------------------------------------------------------------------
// Abbreviations
// ---------------------------------------------------------------------------

static ArgList abbr_names;
static ArgList abbr_values;
static int abbr_ready = 0;

static void abbr_init(void) {
  if (!abbr_ready) {
    arglist_init(&abbr_names);
    arglist_init(&abbr_values);
    abbr_ready = 1;
  }
}

const char *abbr_get(const char *name) {
  abbr_init();
  for (int i = 0;i < abbr_names.count;i++) {
    if (strcmp(abbr_names.items[i], name) == 0) return abbr_values.items[i];
  }
  return NULL;
}

static void abbr_remove(int index) {
  free(abbr_names.items[index]);
  free(abbr_values.items[index]);
  for (int i = index;i < abbr_names.count - 1;i++) {
    abbr_names.items[i] = abbr_names.items[i + 1];
    abbr_values.items[i] = abbr_values.items[i + 1];
  }
  abbr_names.count--;
  abbr_values.count--;
  abbr_names.items[abbr_names.count] = NULL;
  abbr_values.items[abbr_values.count] = NULL;
}

static int builtin_abbr(ArgList *args) {
  abbr_init();
  int i = 1;
  int erase = 0, list_names = 0;
  while (i < args->count && args->items[i][0] == '-') {
    const char *flag = args->items[i];
    if (strcmp(flag, "-a") == 0 || strcmp(flag, "--add") == 0) { i++; continue; }
    if (strcmp(flag, "-e") == 0 || strcmp(flag, "--erase") == 0) { erase = 1; i++; continue; }
    if (strcmp(flag, "-l") == 0 || strcmp(flag, "--list") == 0) { list_names = 1; i++; continue; }
    if (strcmp(flag, "-s") == 0 || strcmp(flag, "--show") == 0) { i++; continue; }
    if (strcmp(flag, "-g") == 0 || strcmp(flag, "-U") == 0) { i++; continue; }
    if (strcmp(flag, "--") == 0) { i++; break; }
    fprintf(stderr, "tarsh: abbr: unknown option %s\n", flag);
    return 2;
  }

  if (erase) {
    int status = 0;
    for (;i < args->count;i++) {
      int found = 0;
      for (int k = 0;k < abbr_names.count;k++) {
        if (strcmp(abbr_names.items[k], args->items[i]) == 0) { abbr_remove(k); found = 1; break; }
      }
      if (!found) { fprintf(stderr, "tarsh: abbr: no abbreviation '%s'\n", args->items[i]); status = 1; }
    }
    return status;
  }
  if (i >= args->count) {
    for (int k = 0;k < abbr_names.count;k++) {
      if (list_names) {
        printf("%s\n", abbr_names.items[k]);
      }
      else {
        char *quoted = shell_quote(abbr_values.items[k]);
        printf("abbr %s %s\n", abbr_names.items[k], quoted ? quoted : abbr_values.items[k]);
        free(quoted);
      }
    }
    if (abbr_names.count == 0 && !list_names) {
      printf("no abbreviations yet. add one: abbr gco git checkout\n");
      printf("(typing gco then space or Enter turns it into git checkout; put them in tarshrc to keep them)\n");
    }
    return 0;
  }
  if (i + 1 >= args->count) {
    const char *existing = abbr_get(args->items[i]);
    if (existing) { printf("abbr %s %s\n", args->items[i], existing); return 0; }
    fprintf(stderr, "usage: abbr NAME EXPANSION...   (e.g. abbr gco git checkout)\n");
    return 2;
  }

  // Everything after the name is the expansion.
  size_t length = 0;
  for (int k = i + 1;k < args->count;k++) length += strlen(args->items[k]) + 1;
  char *expansion = calloc(length + 1, 1);
  for (int k = i + 1;k < args->count;k++) {
    if (k > i + 1) strcat(expansion, " ");
    strcat(expansion, args->items[k]);
  }
  for (int k = 0;k < abbr_names.count;k++) {
    if (strcmp(abbr_names.items[k], args->items[i]) == 0) { abbr_remove(k); break; }
  }
  arglist_append(&abbr_names, args->items[i]);
  arglist_append(&abbr_values, expansion);
  free(expansion);
  return 0;
}

// ---------------------------------------------------------------------------
// Universal variables
// ---------------------------------------------------------------------------

#define ITEM_SEPARATOR '\x1f'

static time_t universal_mtime = 0;

static const char *universal_path(void) {
  static char path[PATH_MAX];
  if (!path[0]) snprintf(path, sizeof(path), "%s/universal", config_directory());
  return path;
}

// Each line: NAME <tab> x|- <tab> item\x1fitem...  (\n and \\ escaped)
static void apply_universal(const char *name, int exported, ArgList *values) {
  if (exported) {
    // Like fish: PATH, MANPATH, LD_LIBRARY_PATH... join with ':'.
    size_t name_length = strlen(name);
    const char *separator = (name_length >= 4 && strcmp(name + name_length - 4, "PATH") == 0) ? ":" : " ";
    size_t length = 1;
    for (int i = 0;i < values->count;i++) length += strlen(values->items[i]) + 1;
    char *joined = calloc(length, 1);
    for (int i = 0;i < values->count;i++) {
      if (i) strcat(joined, separator);
      strcat(joined, values->items[i]);
    }
    setenv(name, joined, 1);
    free(joined);
  }
  else if (values->count == 1) {
    script_var_set(name, values->items[0]);
  }
  else {
    script_var_set_list(name, values);
  }
}

static char *unescape(const char *text) {
  char *out = malloc(strlen(text) + 1);
  size_t used = 0;
  for (size_t i = 0;text[i];i++) {
    if (text[i] == '\\' && text[i + 1]) {
      i++;
      out[used++] = text[i] == 'n' ? '\n' : text[i];
    }
    else {
      out[used++] = text[i];
    }
  }
  out[used] = '\0';
  return out;
}

void universal_load(void) {
  FILE *file = fopen(universal_path(), "r");
  if (file == NULL) return;
  struct stat info;
  if (stat(universal_path(), &info) == 0) universal_mtime = info.st_mtime;
  char *line = NULL;
  size_t size = 0;
  while (getline(&line, &size, file) != -1) {
    line[strcspn(line, "\n")] = '\0';
    char *name = line;
    char *flags = strchr(name, '\t');
    if (!flags) continue;
    *flags++ = '\0';
    char *data = strchr(flags, '\t');
    if (!data) continue;
    *data++ = '\0';
    ArgList values;
    arglist_init(&values);
    char *item = data;
    while (1) {
      char *separator = strchr(item, ITEM_SEPARATOR);
      if (separator) *separator = '\0';
      char *value = unescape(item);
      arglist_append(&values, value);
      free(value);
      if (!separator) break;
      item = separator + 1;
    }
    apply_universal(name, flags[0] == 'x', &values);
    arglist_free(&values);
  }
  free(line);
  fclose(file);
}

void universal_sync(void) {
  struct stat info;
  if (stat(universal_path(), &info) == 0 && info.st_mtime != universal_mtime) {
    universal_load();
  }
}

// Rewrites the file with NAME set to `values` (or removed when NULL).
static int universal_store(const char *name, int exported, ArgList *values) {
  char temp[PATH_MAX + 8];
  snprintf(temp, sizeof(temp), "%s.new", universal_path());
  FILE *out = fopen(temp, "w");
  if (out == NULL) return -1;
  FILE *in = fopen(universal_path(), "r");
  if (in != NULL) {
    char *line = NULL;
    size_t size = 0;
    size_t name_length = strlen(name);
    while (getline(&line, &size, in) != -1) {
      if (strncmp(line, name, name_length) == 0 && line[name_length] == '\t') continue;
      fputs(line, out);
    }
    free(line);
    fclose(in);
  }
  if (values != NULL) {
    fprintf(out, "%s\t%s\t", name, exported ? "x" : "-");
    for (int i = 0;i < values->count;i++) {
      if (i) fputc(ITEM_SEPARATOR, out);
      for (const char *c = values->items[i];*c;c++) {
        if (*c == '\n') fputs("\\n", out);
        else if (*c == '\\') fputs("\\\\", out);
        else fputc(*c, out);
      }
    }
    fputc('\n', out);
  }
  fclose(out);
  if (rename(temp, universal_path()) != 0) return -1;
  struct stat info;
  if (stat(universal_path(), &info) == 0) universal_mtime = info.st_mtime;
  return 0;
}

static int universal_has(const char *name) {
  FILE *in = fopen(universal_path(), "r");
  if (in == NULL) return 0;
  char *line = NULL;
  size_t size = 0;
  size_t name_length = strlen(name);
  int found = 0;
  while (!found && getline(&line, &size, in) != -1) {
    found = strncmp(line, name, name_length) == 0 && line[name_length] == '\t';
  }
  free(line);
  fclose(in);
  return found;
}

// ---------------------------------------------------------------------------
// set
// ---------------------------------------------------------------------------

static int valid_name(const char *name) {
  if (!name[0] || (name[0] >= '0' && name[0] <= '9')) return 0;
  for (const char *c = name;*c;c++) {
    if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) return 0;
  }
  return 1;
}

static int builtin_set(ArgList *args) {
  int exported = 0, universal = 0, erase = 0, query = 0, append = 0, prepend = 0, unexport = 0;
  int i = 1;
  for (;i < args->count && args->items[i][0] == '-' && args->items[i][1];i++) {
    const char *flag = args->items[i];
    if (strcmp(flag, "--") == 0) { i++; break; }
    if (strncmp(flag, "--", 2) == 0) {
      if (!strcmp(flag, "--export")) exported = 1;
      else if (!strcmp(flag, "--universal")) universal = 1;
      else if (!strcmp(flag, "--erase")) erase = 1;
      else if (!strcmp(flag, "--query")) query = 1;
      else if (!strcmp(flag, "--append")) append = 1;
      else if (!strcmp(flag, "--prepend")) prepend = 1;
      else if (!strcmp(flag, "--unexport")) unexport = 1;
      else if (!strcmp(flag, "--global") || !strcmp(flag, "--local")) {}
      else { fprintf(stderr, "tarsh: set: unknown option %s\n", flag); return 2; }
      continue;
    }
    for (const char *c = flag + 1;*c;c++) {
      switch (*c) {
        case 'x': exported = 1; break;
        case 'U': universal = 1; break;
        case 'e': erase = 1; break;
        case 'q': query = 1; break;
        case 'a': append = 1; break;
        case 'p': prepend = 1; break;
        case 'u': unexport = 1; break;
        case 'g': case 'l': case 'f': break;
        default:
          fprintf(stderr, "tarsh: set: unknown option -%c\n", *c);
          return 2;
      }
    }
  }

  // `set -e` / `set -x` / `set -o pipefail` with no name: bash options.
  if (i >= args->count && (erase || exported) && !query) {
    fprintf(stderr, "tarsh: `%s` is a bash/sh option (%s). It only matters inside bash scripts; nothing changed in tarsh.\n",
            args->items[1], erase ? "stop at the first error" : "print each command");
    fprintf(stderr, "  in tarsh, `set NAME value` sets a variable, like in fish.\n");
    return 0;
  }
  if (i < args->count && (strcmp(args->items[i], "-o") == 0 || strcmp(args->items[i], "+o") == 0 || args->items[i][0] == '+')) {
    fprintf(stderr, "tarsh: `set %s` is a bash option; it only applies inside bash. nothing changed in tarsh.\n", args->items[i]);
    return 0;
  }

  if (i >= args->count) {
    script_var_print_all();
    return 0;
  }

  if (query) {
    int missing = 0;
    for (;i < args->count;i++) {
      if (!script_var_exists(args->items[i]) && getenv(args->items[i]) == NULL) missing++;
    }
    return missing;
  }

  if (erase) {
    int status = 0;
    for (;i < args->count;i++) {
      const char *name = args->items[i];
      int found = script_var_unset(name);
      if (getenv(name)) { unsetenv(name); found = 1; }
      if (universal_has(name)) { universal_store(name, 0, NULL); found = 1; }
      if (!found) status = 1;
    }
    return status;
  }

  const char *name = args->items[i++];
  if (!valid_name(name)) {
    fprintf(stderr, "tarsh: set: '%s' isn't a valid variable name\n", name);
    return 2;
  }

  ArgList values;
  arglist_init(&values);
  if (append || prepend) {
    ArgList current;
    arglist_init(&current);
    char *text = NULL;
    int kind = script_var_get(name, &text, &current);
    if (kind == 1) arglist_append(&current, text);
    else if (kind == 0 && getenv(name)) arglist_append(&current, getenv(name));
    free(text);
    if (prepend) for (int k = i;k < args->count;k++) arglist_append(&values, args->items[k]);
    for (int k = 0;k < current.count;k++) arglist_append(&values, current.items[k]);
    if (append) for (int k = i;k < args->count;k++) arglist_append(&values, args->items[k]);
    arglist_free(&current);
  }
  else {
    for (int k = i;k < args->count;k++) arglist_append(&values, args->items[k]);
  }

  int export_it = exported || (!unexport && getenv(name) != NULL);
  if (unexport && getenv(name)) unsetenv(name);
  if (export_it) script_var_unset(name);
  apply_universal(name, export_it, &values);
  if (universal && universal_store(name, export_it, &values) != 0) {
    fprintf(stderr, "tarsh: set: couldn't save %s: %s\n", universal_path(), strerror(errno));
  }
  arglist_free(&values);
  return 0;
}

// ---------------------------------------------------------------------------
// string
// ---------------------------------------------------------------------------

// The strings a subcommand works on: its arguments, or stdin's lines.
static void string_inputs(ArgList *args, int from, ArgList *out) {
  if (from < args->count) {
    for (int i = from;i < args->count;i++) arglist_append(out, args->items[i]);
    return;
  }
  if (isatty(STDIN_FILENO)) return;
  char *line = NULL;
  size_t size = 0;
  while (getline(&line, &size, stdin) != -1) {
    line[strcspn(line, "\n")] = '\0';
    arglist_append(out, line);
  }
  free(line);
  clearerr(stdin);
}

static int builtin_string(ArgList *args) {
  if (args->count < 2) {
    fprintf(stderr, "usage: string length|upper|lower|trim|split|join|replace|repeat|sub|match ...\n");
    return 2;
  }
  const char *sub = args->items[1];
  int i = 2;
  ArgList inputs;
  arglist_init(&inputs);
  int status = 1;

  if (!strcmp(sub, "length") || !strcmp(sub, "upper") || !strcmp(sub, "lower") || !strcmp(sub, "trim") ||
      !strcmp(sub, "escape")) {
    string_inputs(args, i, &inputs);
    for (int k = 0;k < inputs.count;k++) {
      char *text = inputs.items[k];
      if (!strcmp(sub, "length")) { printf("%zu\n", strlen(text)); if (strlen(text)) status = 0; continue; }
      if (!strcmp(sub, "trim")) {
        char *start = text;
        while (*start == ' ' || *start == '\t') start++;
        size_t length = strlen(start);
        while (length && (start[length - 1] == ' ' || start[length - 1] == '\t')) start[--length] = '\0';
        printf("%s\n", start);
        status = 0;
        continue;
      }
      if (!strcmp(sub, "escape")) { char *q = shell_quote(text); printf("%s\n", q); free(q); status = 0; continue; }
      for (char *c = text;*c;c++) {
        if (!strcmp(sub, "upper") && *c >= 'a' && *c <= 'z') *c -= 32;
        if (!strcmp(sub, "lower") && *c >= 'A' && *c <= 'Z') *c += 32;
      }
      printf("%s\n", text);
      status = 0;
    }
  }
  else if (!strcmp(sub, "split") || !strcmp(sub, "join")) {
    if (i >= args->count) { fprintf(stderr, "usage: string %s SEPARATOR [STRING...]\n", sub); arglist_free(&inputs); return 2; }
    const char *separator = args->items[i++];
    string_inputs(args, i, &inputs);
    if (!strcmp(sub, "join")) {
      for (int k = 0;k < inputs.count;k++) printf("%s%s", k ? separator : "", inputs.items[k]);
      if (inputs.count) printf("\n");
      status = inputs.count > 1 ? 0 : 1;
    }
    else {
      size_t length = strlen(separator);
      for (int k = 0;k < inputs.count;k++) {
        char *cursor = inputs.items[k], *found;
        while (length && (found = strstr(cursor, separator)) != NULL) {
          printf("%.*s\n", (int)(found - cursor), cursor);
          cursor = found + length;
          status = 0;
        }
        printf("%s\n", cursor);
      }
    }
  }
  else if (!strcmp(sub, "replace")) {
    int all = 0;
    if (i < args->count && (!strcmp(args->items[i], "-a") || !strcmp(args->items[i], "--all"))) { all = 1; i++; }
    if (i + 1 >= args->count) { fprintf(stderr, "usage: string replace [-a] OLD NEW [STRING...]\n"); arglist_free(&inputs); return 2; }
    const char *from = args->items[i], *to = args->items[i + 1];
    string_inputs(args, i + 2, &inputs);
    size_t length = strlen(from);
    for (int k = 0;k < inputs.count;k++) {
      char *cursor = inputs.items[k], *found;
      while (length && (found = strstr(cursor, from)) != NULL) {
        printf("%.*s%s", (int)(found - cursor), cursor, to);
        cursor = found + length;
        status = 0;
        if (!all) break;
      }
      printf("%s\n", cursor);
    }
  }
  else if (!strcmp(sub, "repeat")) {
    long times = 1;
    if (i + 1 < args->count && (!strcmp(args->items[i], "-n") || !strcmp(args->items[i], "--count"))) { times = atol(args->items[i + 1]); i += 2; }
    string_inputs(args, i, &inputs);
    for (int k = 0;k < inputs.count;k++) {
      for (long t = 0;t < times;t++) fputs(inputs.items[k], stdout);
      printf("\n");
      status = 0;
    }
  }
  else if (!strcmp(sub, "sub")) {
    long start = 1, length = -1;
    while (i + 1 < args->count && args->items[i][0] == '-') {
      if (!strcmp(args->items[i], "-s") || !strcmp(args->items[i], "--start")) start = atol(args->items[i + 1]);
      else if (!strcmp(args->items[i], "-l") || !strcmp(args->items[i], "--length")) length = atol(args->items[i + 1]);
      else break;
      i += 2;
    }
    string_inputs(args, i, &inputs);
    for (int k = 0;k < inputs.count;k++) {
      long total = (long)strlen(inputs.items[k]);
      long from = start > 0 ? start - 1 : total + start;
      if (from < 0) from = 0;
      if (from > total) from = total;
      long count = length < 0 ? total - from : length;
      if (from + count > total) count = total - from;
      printf("%.*s\n", (int)count, inputs.items[k] + from);
      status = 0;
    }
  }
  else if (!strcmp(sub, "match")) {
    int use_regex = 0, quiet = 0;
    while (i < args->count && args->items[i][0] == '-') {
      if (!strcmp(args->items[i], "-r") || !strcmp(args->items[i], "--regex")) use_regex = 1;
      else if (!strcmp(args->items[i], "-q") || !strcmp(args->items[i], "--quiet")) quiet = 1;
      else break;
      i++;
    }
    if (i >= args->count) { fprintf(stderr, "usage: string match [-r] [-q] PATTERN [STRING...]\n"); arglist_free(&inputs); return 2; }
    const char *pattern = args->items[i++];
    string_inputs(args, i, &inputs);
    regex_t regex;
    if (use_regex && regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB) != 0) {
      fprintf(stderr, "tarsh: string match: bad regular expression\n");
      arglist_free(&inputs);
      return 2;
    }
    for (int k = 0;k < inputs.count;k++) {
      int matched = use_regex ? regexec(&regex, inputs.items[k], 0, NULL, 0) == 0
                              : fnmatch(pattern, inputs.items[k], 0) == 0;
      if (matched) { status = 0; if (!quiet) printf("%s\n", inputs.items[k]); }
    }
    if (use_regex) regfree(&regex);
  }
  else {
    fprintf(stderr, "tarsh: string: unknown subcommand '%s'\n", sub);
    status = 2;
  }
  arglist_free(&inputs);
  return status;
}

// ---------------------------------------------------------------------------
// math, count, contains, random
// ---------------------------------------------------------------------------

static int builtin_math(ArgList *args) {
  if (args->count < 2) {
    fprintf(stderr, "usage: math EXPRESSION   (e.g. math 2 + 3 x 4, math \"10 / 3\")\n");
    return 2;
  }
  // fish writes x for multiply and ^ for power.
  size_t length = 0;
  for (int i = 1;i < args->count;i++) length += strlen(args->items[i]) * 2 + 2;
  char *expression = calloc(length + 1, 1);
  for (int i = 1;i < args->count;i++) {
    if (i > 1) strcat(expression, " ");
    if (strcmp(args->items[i], "x") == 0) { strcat(expression, "*"); continue; }
    for (const char *c = args->items[i];*c;c++) {
      if (*c == '^') strcat(expression, "**");
      else strncat(expression, c, 1);
    }
  }
  char *result = NULL;
  int status = script_math(expression, &result);
  free(expression);
  if (status != 0) return 1;
  // Like fish: at most 6 decimals, no trailing zeros.
  if (strchr(result, '.') && !strchr(result, 'e')) {
    char formatted[64];
    snprintf(formatted, sizeof(formatted), "%.6f", strtod(result, NULL));
    size_t end = strlen(formatted);
    while (end > 0 && formatted[end - 1] == '0') formatted[--end] = '\0';
    if (end > 0 && formatted[end - 1] == '.') formatted[--end] = '\0';
    printf("%s\n", formatted);
  }
  else {
    printf("%s\n", result);
  }
  free(result);
  return 0;
}

static int builtin_count(ArgList *args) {
  long total = args->count - 1;
  if (total == 0 && !isatty(STDIN_FILENO)) {
    char *line = NULL;
    size_t size = 0;
    while (getline(&line, &size, stdin) != -1) total++;
    free(line);
    clearerr(stdin);
  }
  printf("%ld\n", total);
  return total == 0;
}

static int builtin_contains(ArgList *args) {
  int i = 1, show_index = 0;
  if (i < args->count && (!strcmp(args->items[i], "-i") || !strcmp(args->items[i], "--index"))) { show_index = 1; i++; }
  if (i < args->count && !strcmp(args->items[i], "--")) i++;
  if (i >= args->count) { fprintf(stderr, "usage: contains [-i] KEY [VALUE...]\n"); return 2; }
  const char *key = args->items[i];
  for (int k = i + 1;k < args->count;k++) {
    if (strcmp(args->items[k], key) == 0) {
      if (show_index) printf("%d\n", k - i);
      return 0;
    }
  }
  return 1;
}

static int builtin_random(ArgList *args) {
  static int seeded = 0;
  if (!seeded) { srandom((unsigned)time(NULL) ^ (unsigned)getpid()); seeded = 1; }
  if (args->count >= 2 && strcmp(args->items[1], "choice") == 0) {
    if (args->count < 3) { fprintf(stderr, "usage: random choice ITEM...\n"); return 2; }
    printf("%s\n", args->items[2 + random() % (args->count - 2)]);
    return 0;
  }
  long low = 0, high = 32767;
  if (args->count == 3) { low = atol(args->items[1]); high = atol(args->items[2]); }
  else if (args->count == 2) { high = atol(args->items[1]); }
  if (high < low) { fprintf(stderr, "tarsh: random: the first number must be the smaller one\n"); return 2; }
  printf("%ld\n", low + random() % (high - low + 1));
  return 0;
}

// ---------------------------------------------------------------------------
// functions, funcsave, funced
// ---------------------------------------------------------------------------

static void function_path(const char *name, char *out, size_t size) {
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s/functions", config_directory());
  mkdir(dir, 0700);
  snprintf(out, size, "%s/%s.tsh", dir, name);
}

static int builtin_funcsave(ArgList *args) {
  if (args->count < 2) { fprintf(stderr, "usage: funcsave NAME...\n"); return 2; }
  int status = 0;
  for (int i = 1;i < args->count;i++) {
    char *source = script_function_source(args->items[i]);
    if (source == NULL) {
      fprintf(stderr, "tarsh: funcsave: no function called '%s' (define one with: def %s():)\n", args->items[i], args->items[i]);
      status = 1;
      continue;
    }
    char path[PATH_MAX + 32];
    function_path(args->items[i], path, sizeof(path));
    FILE *file = fopen(path, "w");
    if (file == NULL) { fprintf(stderr, "tarsh: funcsave: %s: %s\n", path, strerror(errno)); free(source); status = 1; continue; }
    fputs(source, file);
    fclose(file);
    printf("saved %s to %s (it loads by itself the first time you use it)\n", args->items[i], path);
    free(source);
  }
  return status;
}

static int builtin_funced(ArgList *args) {
  if (args->count < 2) { fprintf(stderr, "usage: funced NAME\n"); return 2; }
  const char *name = args->items[1];
  char path[PATH_MAX + 32];
  function_path(name, path, sizeof(path));
  if (access(path, F_OK) != 0) {
    char *source = script_function_source(name);
    FILE *file = fopen(path, "w");
    if (file == NULL) { free(source); fprintf(stderr, "tarsh: funced: %s: %s\n", path, strerror(errno)); return 1; }
    if (source) fputs(source, file);
    else fprintf(file, "def %s():\n    echo \"hello from %s\"\n", name, name);
    fclose(file);
    free(source);
  }
  const char *editor = getenv("VISUAL");
  if (!editor || !*editor) editor = getenv("EDITOR");
  char found[PATH_MAX];
  if (!editor || !*editor) editor = path_lookup("nano", found, sizeof(found)) ? "nano" : "vi";
  char *argv[] = { (char *)editor, path, NULL };
  int status = jobs_run_argv(argv, editor, NULL);
  if (status == 0) {
    exec_run_file(path, 0);
    printf("%s is loaded (and saved in %s)\n", name, path);
  }
  return status;
}

static int builtin_functions(ArgList *args) {
  if (args->count >= 3 && (!strcmp(args->items[1], "-e") || !strcmp(args->items[1], "--erase"))) {
    int status = 0;
    for (int i = 2;i < args->count;i++) {
      if (script_function_erase(args->items[i]) != 0) { fprintf(stderr, "tarsh: functions: no function '%s'\n", args->items[i]); status = 1; }
    }
    return status;
  }
  if (args->count >= 2) {
    int status = 0;
    for (int i = 1;i < args->count;i++) {
      char *source = script_function_source(args->items[i]);
      if (!source) { fprintf(stderr, "tarsh: functions: no function '%s'\n", args->items[i]); status = 1; continue; }
      fputs(source, stdout);
      free(source);
    }
    return status;
  }
  ArgList names;
  arglist_init(&names);
  script_function_names(&names);
  for (int i = 0;i < names.count;i++) printf("%s\n", names.items[i]);
  if (names.count == 0) printf("no functions yet. define one in tarsh script:  def hello(name):\n");
  arglist_free(&names);
  return 0;
}

// ---------------------------------------------------------------------------
// Directory history
// ---------------------------------------------------------------------------

#define DIR_HISTORY 25

static char *back[DIR_HISTORY];
static int back_count = 0;
static char *forward[DIR_HISTORY];
static int forward_count = 0;
static char last_cwd[PATH_MAX];
static int navigating = 0;

static void push(char **stack, int *count, const char *dir) {
  if (*count == DIR_HISTORY) {
    free(stack[0]);
    memmove(stack, stack + 1, (DIR_HISTORY - 1) * sizeof(char *));
    (*count)--;
  }
  stack[(*count)++] = strdup(dir);
}

void dirs_note_cwd(void) {
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) == NULL) return;
  if (last_cwd[0] && strcmp(cwd, last_cwd) != 0 && !navigating) {
    push(back, &back_count, last_cwd);
    for (int i = 0;i < forward_count;i++) free(forward[i]);
    forward_count = 0;
  }
  navigating = 0;
  snprintf(last_cwd, sizeof(last_cwd), "%s", cwd);
}

static int go_to(const char *dir) {
  char previous[PATH_MAX];
  if (getcwd(previous, sizeof(previous)) == NULL) previous[0] = '\0';
  if (chdir(dir) != 0) {
    fprintf(stderr, "tarsh: %s: %s\n", dir, strerror(errno));
    return 1;
  }
  if (previous[0]) setenv("OLDPWD", previous, 1);
  setenv("PWD", dir, 1);
  snprintf(last_cwd, sizeof(last_cwd), "%s", dir);
  navigating = 1;
  return 0;
}

static int builtin_prevd_nextd(ArgList *args, int backwards) {
  int steps = args->count > 1 ? atoi(args->items[1]) : 1;
  if (steps < 1) steps = 1;
  char **from = backwards ? back : forward;
  int *from_count = backwards ? &back_count : &forward_count;
  char **to = backwards ? forward : back;
  int *to_count = backwards ? &forward_count : &back_count;
  if (*from_count == 0) {
    fprintf(stderr, "tarsh: %s: no %s folder\n", backwards ? "prevd" : "nextd", backwards ? "previous" : "next");
    return 1;
  }
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) == NULL) return 1;
  char *target = NULL;
  for (int i = 0;i < steps && *from_count > 0;i++) {
    push(to, to_count, cwd);
    free(target);
    target = from[--(*from_count)];
    snprintf(cwd, sizeof(cwd), "%s", target);
  }
  int status = go_to(target);
  free(target);
  return status;
}

static int builtin_dirh(void) {
  for (int i = 0;i < back_count;i++) printf("%3d) %s\n", back_count - i, back[i]);
  char cwd[PATH_MAX];
  printf("     %s  <- you are here\n", getcwd(cwd, sizeof(cwd)) ? cwd : "?");
  for (int i = forward_count - 1;i >= 0;i--) printf("%3d) %s\n", forward_count - i, forward[i]);
  if (back_count == 0 && forward_count == 0) printf("(prevd / Alt+Left goes back, nextd / Alt+Right forward)\n");
  return 0;
}

// cdh: jump to a folder you were in recently.
static int builtin_cdh(ArgList *args) {
  ArgList recent;
  arglist_init(&recent);
  for (int i = back_count - 1;i >= 0;i--) {
    int seen = 0;
    for (int k = 0;k < recent.count;k++) if (!strcmp(recent.items[k], back[i])) seen = 1;
    if (!seen) arglist_append(&recent, back[i]);
  }
  if (recent.count == 0) {
    printf("no recent folders yet\n");
    arglist_free(&recent);
    return 1;
  }
  int status = 0;
  if (args->count > 1) {
    int pick = atoi(args->items[1]);
    if (pick < 1 || pick > recent.count) { fprintf(stderr, "tarsh: cdh: pick 1 to %d\n", recent.count); status = 1; }
    else status = go_to(recent.items[pick - 1]);
  }
  else if (fzf_enabled() && isatty(STDIN_FILENO)) {
    char list_path[] = "/tmp/tarsh-cdh-XXXXXX";
    int fd = mkstemp(list_path);
    FILE *list = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (list) {
      for (int i = 0;i < recent.count;i++) fprintf(list, "%s\n", recent.items[i]);
      fclose(list);
      char command[PATH_MAX + 128];
      snprintf(command, sizeof(command), "fzf --height=40%% --reverse --prompt='recent folder > ' < %s", list_path);
      FILE *picker = popen(command, "r");
      char choice[PATH_MAX] = "";
      if (picker && fgets(choice, sizeof(choice), picker)) {
        choice[strcspn(choice, "\n")] = '\0';
      }
      if (picker) pclose(picker);
      unlink(list_path);
      if (choice[0]) status = go_to(choice);
    }
  }
  else {
    for (int i = 0;i < recent.count;i++) printf("%3d) %s\n", i + 1, recent.items[i]);
    printf("go to one with: cdh NUMBER\n");
  }
  arglist_free(&recent);
  return status;
}

// ---------------------------------------------------------------------------

int fish_builtin_run(ArgList *args, int *status) {
  const char *name = args->items[0];
  if (!strcmp(name, "abbr")) *status = builtin_abbr(args);
  else if (!strcmp(name, "set")) *status = builtin_set(args);
  else if (!strcmp(name, "string")) *status = builtin_string(args);
  else if (!strcmp(name, "math")) *status = builtin_math(args);
  else if (!strcmp(name, "count")) *status = builtin_count(args);
  else if (!strcmp(name, "contains")) *status = builtin_contains(args);
  else if (!strcmp(name, "random")) *status = builtin_random(args);
  else if (!strcmp(name, "funcsave")) *status = builtin_funcsave(args);
  else if (!strcmp(name, "funced")) *status = builtin_funced(args);
  else if (!strcmp(name, "functions")) *status = builtin_functions(args);
  else if (!strcmp(name, "prevd")) *status = builtin_prevd_nextd(args, 1);
  else if (!strcmp(name, "nextd")) *status = builtin_prevd_nextd(args, 0);
  else if (!strcmp(name, "dirh")) *status = builtin_dirh();
  else if (!strcmp(name, "cdh")) *status = builtin_cdh(args);
  else return 0;
  return 1;
}
