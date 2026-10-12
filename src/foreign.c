#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "foreign.h"
#include "exec.h"
#include "jobs.h"
#include "lang.h"
#include "parser.h"

enum {
  F_FOR, F_WHILE, F_UNTIL, F_IF, F_CASE, F_SWITCH, F_FUNC_PAREN, F_FUNC_KEYWORD,
  F_FUNC_BRACE, F_ARITH, F_HEREDOC, F_HERESTRING, F_PROCSUB, F_DOUBLE_BRACKET,
  F_BRACE, F_SUBSHELL, F_GROUP, F_BEGIN, F_FISH_SUBST, F_END, F_THEN_DO,
  F_ZSH_FLAGS, F_ZSH_QUALIFIER, F_ZSH_OPTION, F_DECLARE, F_BASH_BUILTIN, F_ELSE_IF,
  F_COUNT
};

typedef struct {
  int has[F_COUNT];
  char function_name[64];
  char builtin_word[32];
}Scan;

static int enabled = 1;
static int tips_shown[64];
static char tip_kinds[64][24];
static int tip_count = 0;

void foreign_set_enabled(int on) { enabled = on; }
int foreign_enabled(void) { return enabled; }

int foreign_configure(const char *key, const char *value) {
  if (strcmp(key, "EXPLAIN") != 0) return 0;
  enabled = atoi(value) != 0;
  return 1;
}

void foreign_write_defaults(FILE *file) {
  fprintf(file, "# After running bash/zsh/fish syntax, explain what happened in one line\n");
  fprintf(file, "EXPLAIN=1\n");
}

// A copy of `line` with the insides of quotes blanked out, so quoted text
// can't look like syntax.
static char *code_only(const char *line) {
  char *code = strdup(line);
  char quote = '\0';
  for (size_t i = 0;code[i];i++) {
    if (quote) {
      if (code[i] == '\\' && quote == '"' && code[i + 1]) { code[i] = 'x'; code[++i] = 'x'; continue; }
      if (code[i] == quote) { quote = '\0'; continue; }
      code[i] = 'x';
      continue;
    }
    if (code[i] == '\\' && code[i + 1]) { i++; continue; }
    if (code[i] == '\'' || code[i] == '"') quote = code[i];
    if (code[i] == '#' && (i == 0 || code[i - 1] == ' ' || code[i - 1] == '\t' || code[i - 1] == '\n')) {
      while (code[i] && code[i] != '\n') code[i++] = ' ';
      if (!code[i]) break;
    }
  }
  return code;
}

static int word_is(const char *word, size_t length, const char *text) {
  return strlen(text) == length && strncmp(word, text, length) == 0;
}

static void scan(const char *line, Scan *out) {
  memset(out, 0, sizeof(*out));
  char *code = code_only(line);

  if (strstr(code, "$((")) out->has[F_ARITH] = 1;
  if (strstr(code, "<<<")) out->has[F_HERESTRING] = 1;
  else if (strstr(code, "<<")) out->has[F_HEREDOC] = 1;
  if (strstr(code, "<(") || strstr(code, ">(")) out->has[F_PROCSUB] = 1;
  if (strstr(code, "${(")) out->has[F_ZSH_FLAGS] = 1;
  if (strstr(code, "else if")) out->has[F_ELSE_IF] = 1;

  int command_start = 1;
  const char *previous = NULL;
  size_t previous_length = 0;
  size_t i = 0;
  while (code[i]) {
    char c = code[i];
    if (c == ' ' || c == '\t') { i++; continue; }
    if (c == '\n' || c == ';' || c == '|' || c == '&') { command_start = 1; i++; previous = NULL; continue; }
    if (c == '(' || c == ')') {
      if (c == '(') {
        int after_dollar = i > 0 && (code[i - 1] == '$' || code[i - 1] == '<' || code[i - 1] == '>' || code[i - 1] == '=');
        if (command_start) out->has[F_SUBSHELL] = 1;
        else if (code[i + 1] == ')') {
          out->has[F_FUNC_PAREN] = 1;
          if (previous) snprintf(out->function_name, sizeof(out->function_name), "%.*s", (int)previous_length, previous);
        }
        else if (!after_dollar) out->has[F_FISH_SUBST] = 1;
      }
      i++;
      command_start = (c == '(');
      continue;
    }

    size_t start = i;
    while (code[i] && !strchr(" \t\n;|&()", code[i])) i++;
    const char *word = code + start;
    size_t length = i - start;

    // name() glued together
    if (code[i] == '(' && code[i + 1] == ')' && length > 0) {
      out->has[F_FUNC_PAREN] = 1;
      snprintf(out->function_name, sizeof(out->function_name), "%.*s", (int)length, word);
      i += 2;
      previous = NULL;
      command_start = 0;
      continue;
    }
    // *(.) and friends: zsh glob qualifiers
    if (code[i] == '(' && length > 0 && strchr("*?]", word[length - 1])) {
      out->has[F_ZSH_QUALIFIER] = 1;
    }
    // {a,b} or {1..5}
    const char *brace = memchr(word, '{', length);
    if (brace && (brace == word || brace[-1] != '$')) {
      const char *close = memchr(brace, '}', length - (brace - word));
      if (close) {
        for (const char *k = brace;k < close;k++) {
          if (*k == ',' || (k[0] == '.' && k[1] == '.')) { out->has[F_BRACE] = 1; break; }
        }
      }
    }

    if (command_start) {
      if (word_is(word, length, "for")) out->has[F_FOR] = 1;
      else if (word_is(word, length, "while")) out->has[F_WHILE] = 1;
      else if (word_is(word, length, "until")) out->has[F_UNTIL] = 1;
      else if (word_is(word, length, "if")) out->has[F_IF] = 1;
      else if (word_is(word, length, "case")) out->has[F_CASE] = 1;
      else if (word_is(word, length, "switch")) out->has[F_SWITCH] = 1;
      else if (word_is(word, length, "begin")) out->has[F_BEGIN] = 1;
      else if (word_is(word, length, "end")) out->has[F_END] = 1;
      else if (word_is(word, length, "[[")) out->has[F_DOUBLE_BRACKET] = 1;
      else if (word_is(word, length, "{")) out->has[F_GROUP] = 1;
      else if (word_is(word, length, "then") || word_is(word, length, "do") || word_is(word, length, "done") ||
               word_is(word, length, "fi") || word_is(word, length, "esac") || word_is(word, length, "elif")) {
        out->has[F_THEN_DO] = 1;
      }
      else if (word_is(word, length, "function")) {
        out->has[F_FUNC_KEYWORD] = 1;
        size_t name_start = i;
        while (code[name_start] == ' ') name_start++;
        size_t name_end = name_start;
        while (code[name_end] && !strchr(" \t\n;({", code[name_end])) name_end++;
        snprintf(out->function_name, sizeof(out->function_name), "%.*s", (int)(name_end - name_start), code + name_start);
        if (strchr(code + name_end, '{') != NULL) out->has[F_FUNC_BRACE] = 1;
      }
      else if (word_is(word, length, "declare") || word_is(word, length, "local") ||
               word_is(word, length, "readonly") || word_is(word, length, "let") ||
               word_is(word, length, "typeset")) {
        out->has[F_DECLARE] = 1;
        snprintf(out->builtin_word, sizeof(out->builtin_word), "%.*s", (int)length, word);
      }
      else if (word_is(word, length, "setopt") || word_is(word, length, "unsetopt") ||
               word_is(word, length, "autoload") || word_is(word, length, "zmodload") ||
               word_is(word, length, "foreach") || word_is(word, length, "repeat")) {
        out->has[F_ZSH_OPTION] = 1;
        snprintf(out->builtin_word, sizeof(out->builtin_word), "%.*s", (int)length, word);
      }
      else if (word_is(word, length, "shopt") || word_is(word, length, "trap") || word_is(word, length, "eval") ||
               word_is(word, length, "select") || word_is(word, length, "coproc") || word_is(word, length, "time")) {
        out->has[F_BASH_BUILTIN] = 1;
        snprintf(out->builtin_word, sizeof(out->builtin_word), "%.*s", (int)length, word);
      }
      // After these, the next word is a command again.
      command_start = word_is(word, length, "then") || word_is(word, length, "do") ||
                      word_is(word, length, "else") || word_is(word, length, "{") ||
                      word_is(word, length, "if") || word_is(word, length, "while") ||
                      word_is(word, length, "until") || word_is(word, length, "!") ||
                      word_is(word, length, "time") || word_is(word, length, "begin") ||
                      word_is(word, length, "elif");
    }
    else {
      // `for x in a b; do` -> `do` follows a ';' so it's handled above;
      // `if test; then` likewise.
      command_start = 0;
    }
    previous = word;
    previous_length = length;
  }
  free(code);
}

static const char *pick_language(const Scan *s) {
  int bash = 0, fish = 0, zsh = 0;
  bash += 3 * (s->has[F_THEN_DO] + s->has[F_DOUBLE_BRACKET] + s->has[F_FUNC_PAREN] + s->has[F_CASE] + s->has[F_FUNC_BRACE]);
  bash += 2 * (s->has[F_ARITH] + s->has[F_HERESTRING] + s->has[F_PROCSUB] + s->has[F_DECLARE] + s->has[F_BASH_BUILTIN]);
  bash += s->has[F_HEREDOC] + s->has[F_SUBSHELL] + s->has[F_GROUP] + s->has[F_BRACE];
  fish += 4 * s->has[F_END] + 3 * (s->has[F_BEGIN] + s->has[F_SWITCH] + s->has[F_FISH_SUBST]) + 2 * s->has[F_ELSE_IF];
  if (s->has[F_FUNC_KEYWORD] && !s->has[F_FUNC_BRACE] && !s->has[F_FUNC_PAREN]) fish += 3;
  zsh += 4 * (s->has[F_ZSH_FLAGS] + s->has[F_ZSH_OPTION]) + 3 * s->has[F_ZSH_QUALIFIER];

  const char *chosen = lang_current_name();
  if (fish > bash && fish >= zsh) chosen = "fish";
  else if (zsh > bash && zsh > fish) chosen = "zsh";
  else if (bash > 0) {
    // zsh runs bash syntax fine; respect someone who chose zsh.
    chosen = strcmp(lang_current_name(), "zsh") == 0 ? "zsh" : "bash";
  }
  if (strcmp(chosen, "sh") == 0 && bash > 0) chosen = "bash";
  return chosen;
}

static void describe(const Scan *s, const char *language, Foreign *out) {
  int fish = strcmp(language, "fish") == 0;
  const char *what = "", *tip = "", *kind = "generic";
  char buffer[256];

  if (s->has[F_FUNC_PAREN] || s->has[F_FUNC_KEYWORD]) {
    snprintf(buffer, sizeof(buffer),
             "defined a %s function%s%s%s and ran the line there. The function is gone now: functions can't come back from %s",
             language, s->function_name[0] ? " '" : "", s->function_name, s->function_name[0] ? "'" : "", language);
    what = buffer;
    tip = "keep functions in tarsh script instead: def name(): (and funcsave name to save it)";
    kind = "function";
  }
  else if (s->has[F_FOR]) {
    what = fish ? "a for-loop: ran the body once for each item, up to `end`"
                : "a for-loop: ran the commands between `do` and `done` once for each item";
    tip = "tarsh script: for x in [\"a\", \"b\"]:  (then indent the body)";
    kind = "for";
  }
  else if (s->has[F_WHILE] || s->has[F_UNTIL]) {
    what = s->has[F_UNTIL] ? "an until-loop: repeated the body until the condition succeeded"
                           : "a while-loop: repeated the body while the condition kept succeeding";
    tip = "tarsh script: while condition:";
    kind = "while";
  }
  else if (s->has[F_IF]) {
    what = fish ? "an if-statement: picked a branch by whether the test command succeeded, up to `end`"
                : "an if-statement: picked a branch by whether the test command succeeded, up to `fi`";
    tip = "tarsh script: if ?(test -f file):   or   if x > 3:";
    kind = "if";
  }
  else if (s->has[F_CASE] || s->has[F_SWITCH]) {
    what = "a case/switch: matched a value against patterns and ran the first match";
    tip = "tarsh script: if / elif / else";
    kind = "case";
  }
  else if (s->has[F_ARITH]) {
    what = "arithmetic: $(( )) worked out the sum";
    tip = "tarsh: math 2 + 3   or in tarsh script: x = 2 + 3";
    kind = "arith";
  }
  else if (s->has[F_HEREDOC]) {
    what = "a here-document: the lines up to the end marker were fed to the command as its input";
    tip = "";
    kind = "heredoc";
  }
  else if (s->has[F_HERESTRING]) {
    what = "a here-string: the text after <<< was fed to the command as its input";
    tip = "tarsh: echo \"text\" | command";
    kind = "herestring";
  }
  else if (s->has[F_PROCSUB]) {
    what = "process substitution: <( ) let a command read another command's output as if it were a file";
    tip = "";
    kind = "procsub";
  }
  else if (s->has[F_DOUBLE_BRACKET]) {
    what = "a [[ ]] test: checked a condition (bash's extended test)";
    tip = "tarsh: test / [ ] work as commands; in tarsh script: if x == \"y\":";
    kind = "dbracket";
  }
  else if (s->has[F_BRACE]) {
    what = "brace expansion: {a,b} / {1..5} was expanded into separate words";
    tip = "tarsh script: [\"a\", \"b\"] or range(1, 6)";
    kind = "brace";
  }
  else if (s->has[F_FISH_SUBST]) {
    what = "command substitution: (cmd) was replaced by that command's output";
    tip = "tarsh: $(cmd)";
    kind = "fishsubst";
  }
  else if (s->has[F_SUBSHELL]) {
    snprintf(buffer, sizeof(buffer),
             "a subshell: ( ) ran the commands in a separate copy of %s, so cd and variables inside it didn't change anything outside",
             language);
    what = buffer;
    kind = "subshell";
  }
  else if (s->has[F_BEGIN] || s->has[F_GROUP]) {
    what = "a command group: ran the commands together as one block";
    kind = "group";
  }
  else if (s->has[F_ZSH_FLAGS]) {
    what = "zsh parameter flags: ${(flags)var} transformed a variable";
    kind = "zshflags";
  }
  else if (s->has[F_ZSH_QUALIFIER]) {
    what = "zsh glob qualifiers: the (…) after the pattern filtered which files matched";
    kind = "zshglob";
  }
  else if (s->has[F_ZSH_OPTION] || s->has[F_BASH_BUILTIN] || s->has[F_DECLARE]) {
    snprintf(buffer, sizeof(buffer), "`%s` is a %s builtin; it only applied inside that one %s run",
             s->builtin_word, language, language);
    what = buffer;
    kind = s->builtin_word;
  }
  else {
    snprintf(buffer, sizeof(buffer), "%s syntax that tarsh doesn't run itself", language);
    what = buffer;
  }

  snprintf(out->language, sizeof(out->language), "%s", language);
  snprintf(out->what, sizeof(out->what), "%s", what);
  snprintf(out->tip, sizeof(out->tip), "%s", tip);
  snprintf(out->kind, sizeof(out->kind), "%.23s", kind);
}

void foreign_analyze(const char *line, Foreign *out) {
  Scan s;
  scan(line, &s);
  describe(&s, pick_language(&s), out);
}

int foreign_analyze_file(const char *path, Foreign *out) {
  FILE *file = fopen(path, "r");
  if (file == NULL) return 0;
  static char text[65536];
  size_t length = fread(text, 1, sizeof(text) - 1, file);
  fclose(file);
  text[length] = '\0';
  if (memchr(text, '\0', length) != NULL) return 0;   // a binary

  memset(out, 0, sizeof(*out));
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  size_t base_length = strlen(base);
  const char *language = NULL;
  const char *reason = NULL;

  if (strncmp(text, "#!", 2) == 0) {
    char first[256];
    snprintf(first, sizeof(first), "%.*s", (int)strcspn(text, "\n"), text);
    if (strstr(first, "tarsh")) return 0;
    if (strstr(first, "fish")) language = "fish";
    else if (strstr(first, "zsh")) language = "zsh";
    else if (strstr(first, "bash")) language = "bash";
    else if (strstr(first, "/sh") || strstr(first, " sh") || strstr(first, "dash")) language = "sh";
    else return 0;   // python, perl, ...: the kernel runs those directly
    reason = "its #! line";
  }
  else if (base_length > 4 && strcmp(base + base_length - 4, ".tsh") == 0) {
    return 0;
  }
  else if (strcmp(base, "tarshrc") == 0) {
    return 0;
  }
  else if (base_length > 5 && strcmp(base + base_length - 5, ".fish") == 0) { language = "fish"; reason = "its .fish name"; }
  else if (base_length > 4 && strcmp(base + base_length - 4, ".zsh") == 0) { language = "zsh"; reason = "its .zsh name"; }
  else if (base_length > 5 && strcmp(base + base_length - 5, ".bash") == 0) { language = "bash"; reason = "its .bash name"; }
  else {
    Scan s;
    scan(text, &s);
    language = pick_language(&s);
    reason = "its contents, since it has no #! line";
    if (base_length > 3 && strcmp(base + base_length - 3, ".sh") == 0 && strcmp(language, "fish") != 0) {
      language = "bash";
      reason = "its .sh name";
    }
  }

  snprintf(out->language, sizeof(out->language), "%s", language);
  snprintf(out->what, sizeof(out->what), "ran %s as a %s script (picked from %s)", base, language, reason);
  snprintf(out->kind, sizeof(out->kind), "script");
  snprintf(out->tip, sizeof(out->tip), "add a #! line (e.g. #!/usr/bin/env %s) so every shell knows how to run it",
           strcmp(language, "sh") == 0 ? "sh" : language);
  return 1;
}

static int tip_already_shown(const char *kind) {
  for (int i = 0;i < tip_count;i++) {
    if (strcmp(tip_kinds[i], kind) == 0) return tips_shown[i];
  }
  if (tip_count < 64) {
    snprintf(tip_kinds[tip_count], sizeof(tip_kinds[tip_count]), "%s", kind);
    tips_shown[tip_count++] = 1;
  }
  return 0;
}

void foreign_explain(const Foreign *info, const char *subject) {
  (void)subject;
  if (!enabled || !jobs_interactive() || exec_is_sourcing() || !isatty(STDERR_FILENO)) {
    return;
  }
  char changes[512] = "";
  const char *directory = lang_last_directory();
  ArgList *variables = lang_last_variables();
  if (directory[0]) {
    const char *home = getenv("HOME");
    size_t home_length = home ? strlen(home) : 0;
    if (home && home_length && strncmp(directory, home, home_length) == 0) {
      snprintf(changes, sizeof(changes), " It changed folder, so you're now in ~%s.", directory + home_length);
    }
    else {
      snprintf(changes, sizeof(changes), " It changed folder, so you're now in %s.", directory);
    }
  }
  if (variables->count > 0) {
    size_t used = strlen(changes);
    used += snprintf(changes + used, sizeof(changes) - used, " Kept %d variable%s it set:",
                     variables->count, variables->count == 1 ? "" : "s");
    for (int i = 0;i < variables->count && i < 4 && used < sizeof(changes) - 40;i++) {
      used += snprintf(changes + used, sizeof(changes) - used, "%s %s", i ? "," : "", variables->items[i]);
    }
    if (variables->count > 4) snprintf(changes + used, sizeof(changes) - used, ", ...");
    strncat(changes, ".", sizeof(changes) - strlen(changes) - 1);
  }

  fflush(stdout);
  fprintf(stderr, "\x1b[2m↳ ran in %s: %s.%s\x1b[0m\n", info->language, info->what, changes);
  if (info->tip[0] && !tip_already_shown(info->kind)) {
    fprintf(stderr, "\x1b[2m  %s   (hide these notes: explain off)\x1b[0m\n", info->tip);
  }
}

int foreign_run_line(const char *line) {
  Foreign info;
  foreign_analyze(line, &info);
  if (!lang_installed(info.language)) {
    int bash_family = strcmp(info.language, "fish") != 0;
    if (bash_family && lang_installed("bash")) {
      snprintf(info.language, sizeof(info.language), "bash");
    }
    else {
      fprintf(stderr, "tarsh: that looks like %s syntax, but %s isn't installed.\n", info.language, info.language);
      fprintf(stderr, "  install it with: %s\n", lang_install_hint(info.language));
      return 127;
    }
  }
  int status = lang_run_as(info.language, line);
  if (!lang_last_stopped()) {
    foreign_explain(&info, NULL);
  }
  return status;
}
