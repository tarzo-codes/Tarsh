// pipe2()
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#include "live.h"
#include "alias.h"
#include "exec.h"
#include "jobs.h"
#include "lang.h"
#include "parser.h"
#include "script.h"
#include "syntax.h"
#include "foreign.h"
#include "util.h"
#include <regex.h>
#include <limits.h>

#define DEFAULT_COMMANDS "grep,egrep,fgrep,rg,awk,sed,cut,sort,uniq,head,tail,wc,tr,jq,column,nl,rev,tac,cat,echo,printf,ls,seq,find,fold,paste"

static int enabled = 1;
static int hints = 1;
static int max_lines = 6;
static int delay_ms = 300;
static ArgList commands;
static int commands_ready = 0;

static void set_commands(const char *list) {
  if (commands_ready) arglist_free(&commands);
  arglist_init(&commands);
  commands_ready = 1;
  const char *start = list;
  while (*start) {
    while (*start == ',' || *start == ' ') start++;
    size_t length = strcspn(start, ", ");
    if (length > 0) {
      char *name = strndup(start, length);
      arglist_append(&commands, name);
      free(name);
    }
    start += length;
  }
}

void live_setup(void) {
  set_commands(DEFAULT_COMMANDS);
}

int live_configure(const char *key, const char *value) {
  if (!strcmp(key, "LIVE")) enabled = atoi(value) != 0;
  else if (!strcmp(key, "HINTS")) hints = atoi(value) != 0;
  else if (!strcmp(key, "LIVE_COMMANDS")) set_commands(value);
  else if (!strcmp(key, "LIVE_LINES")) { max_lines = atoi(value); if (max_lines < 1) max_lines = 1; if (max_lines > 30) max_lines = 30; }
  else if (!strcmp(key, "LIVE_DELAY")) { delay_ms = atoi(value); if (delay_ms < 50) delay_ms = 50; }
  else return 0;
  return 1;
}

void live_write_defaults(FILE *file) {
  fprintf(file, "# While typing a | > >> < pipeline, explain each step under the prompt\n");
  fprintf(file, "HINTS=1\n");
  fprintf(file, "# Live preview: when every command in the line is listed here, run it in a\n");
  fprintf(file, "# sandbox (no writing, deleting, network) and show its first output lines\n");
  fprintf(file, "LIVE=1\n");
  fprintf(file, "LIVE_COMMANDS=%s\n", DEFAULT_COMMANDS);
  fprintf(file, "LIVE_LINES=6\n");
  fprintf(file, "# how long to wait after you stop typing, in milliseconds\n");
  fprintf(file, "LIVE_DELAY=300\n");
}

int live_enabled(void) { return enabled; }
void live_set_enabled(int on) { enabled = on; }
int live_hints_enabled(void) { return hints; }
void live_set_hints(int on) { hints = on; }
int live_lines(void) { return max_lines; }
int live_delay_ms(void) { return delay_ms; }

static int listed(const char *name) {
  if (!commands_ready) live_setup();
  for (int i = 0;i < commands.count;i++) {
    if (strcmp(commands.items[i], name) == 0) return 1;
  }
  return 0;
}

static void say(char *why, size_t size, const char *text) {
  if (why && size) snprintf(why, size, "%s", text);
}

// ---------------------------------------------------------------------------
// Planning a preview: what to run in the sandbox, and what to tell you
// ---------------------------------------------------------------------------
//
// Lines that would write files are run with the writing taken out, so you
// see what would be written: `> pids.txt` drops the redirect, `sed -i`
// shows the edited text, `sort -o` shows the sorted text, `find -delete`
// lists what would be deleted. Each change becomes a note in the header.

#define MAX_PATTERNS 8
// /dev/null, opened before the sandbox starts (it can't open files for
// writing after that).
#define NULL_FD 9

typedef struct {
  char *source;        // the pattern as typed (unquoted)
  int extended;        // -E (or rg, or -P)
  int perl;            // -P or rg: \d and friends
  int ignore_case;
  int fixed;           // -F: plain text
  int words;           // -w
  int whole_line;      // -x
}Pattern;

typedef struct {
  char *line;          // what runs in the sandbox
  char shell[8];       // "" for tarsh itself, else bash/zsh/sh
  char notes[512];     // "would write to pids.txt · sed -i would change a.txt"
  Pattern patterns[MAX_PATTERNS];
  int pattern_count;
}Plan;

static void plan_free(Plan *plan) {
  free(plan->line);
  for (int i = 0;i < plan->pattern_count;i++) free(plan->patterns[i].source);
  memset(plan, 0, sizeof(*plan));
}

static void add_note(Plan *plan, const char *format, const char *subject) {
  char note[256];
  snprintf(note, sizeof(note), format, subject);
  if (strstr(plan->notes, note)) return;
  size_t used = strlen(plan->notes);
  snprintf(plan->notes + used, sizeof(plan->notes) - used, "%s%s", used ? " · " : "", note);
}

static int is_grep(const char *name) {
  return !strcmp(name, "grep") || !strcmp(name, "egrep") || !strcmp(name, "fgrep") || !strcmp(name, "rg");
}

// Remembers what a grep looks for, so its matches can be highlighted in
// whatever comes out at the end of the line.
static void collect_grep(Plan *plan, char **argv, int argc) {
  const char *name = argv[0];
  int extended = !strcmp(name, "egrep") || !strcmp(name, "rg");
  int perl = !strcmp(name, "rg");
  int fixed = !strcmp(name, "fgrep");
  int ignore_case = 0, words = 0, whole_line = 0, inverted = 0, given = 0;
  const char *patterns[MAX_PATTERNS];
  int count = 0;
  int options_done = 0;
  for (int i = 1;i < argc;i++) {
    const char *arg = argv[i];
    if (!options_done && !strcmp(arg, "--")) { options_done = 1; continue; }
    if (!options_done && arg[0] == '-' && arg[1]) {
      if (!strcmp(arg, "-e") || !strcmp(arg, "--regexp")) { if (i + 1 < argc && count < MAX_PATTERNS) patterns[count++] = argv[++i]; given = 1; continue; }
      if (!strncmp(arg, "--regexp=", 9)) { if (count < MAX_PATTERNS) patterns[count++] = arg + 9; given = 1; continue; }
      if (!strcmp(arg, "--extended-regexp")) { extended = 1; continue; }
      if (!strcmp(arg, "--perl-regexp")) { extended = perl = 1; continue; }
      if (!strcmp(arg, "--fixed-strings")) { fixed = 1; continue; }
      if (!strcmp(arg, "--ignore-case")) { ignore_case = 1; continue; }
      if (!strcmp(arg, "--invert-match")) { inverted = 1; continue; }
      if (!strcmp(arg, "--word-regexp")) { words = 1; continue; }
      if (!strcmp(arg, "--line-regexp")) { whole_line = 1; continue; }
      if (!strcmp(arg, "-f") || !strcmp(arg, "-m") || !strcmp(arg, "-A") || !strcmp(arg, "-B") || !strcmp(arg, "-C") ||
          !strcmp(arg, "-g") || !strcmp(arg, "-t") || !strcmp(arg, "-T") || !strcmp(arg, "--max-count")) { i++; continue; }
      if (arg[1] == '-') continue;
      for (const char *flag = arg + 1;*flag;flag++) {
        if (*flag == 'E') extended = 1;
        else if (*flag == 'P') extended = perl = 1;
        else if (*flag == 'F') fixed = 1;
        else if (*flag == 'i') ignore_case = 1;
        else if (*flag == 'v') inverted = 1;
        else if (*flag == 'w') words = 1;
        else if (*flag == 'x') whole_line = 1;
        else if (*flag == 'e') {
          const char *rest = flag[1] ? flag + 1 : (i + 1 < argc ? argv[++i] : NULL);
          if (rest && count < MAX_PATTERNS) patterns[count++] = rest;
          given = 1;
          break;
        }
        else if (strchr("mABCf", *flag)) { if (!flag[1]) i++; break; }
      }
      continue;
    }
    if (!given) {
      if (count < MAX_PATTERNS) patterns[count++] = arg;
      given = 1;
    }
    // The rest are files.
  }
  if (inverted) return;  // what's left is what didn't match: nothing to mark
  for (int i = 0;i < count && plan->pattern_count < MAX_PATTERNS;i++) {
    if (patterns[i][0] == '\0') continue;
    Pattern *p = &plan->patterns[plan->pattern_count++];
    p->source = strdup(patterns[i]);
    p->extended = extended;
    p->perl = perl;
    p->fixed = fixed;
    p->ignore_case = ignore_case;
    p->words = words;
    p->whole_line = whole_line;
  }
}

static int check_text(const char *text, char *why, size_t why_size, int depth);

// Every $(...) and `...` inside a word must hold only listed commands too.
static int check_substitutions(const char *word, char *why, size_t why_size, int depth) {
  char quote = 0;
  for (size_t i = 0;word[i];i++) {
    char c = word[i];
    if (quote == '\'') { if (c == '\'') quote = 0; continue; }
    if (c == '\\' && word[i + 1]) { i++; continue; }
    if (c == '\'' && !quote) { quote = '\''; continue; }
    if (c == '"') { quote = quote ? 0 : '"'; continue; }
    if (c == '$' && word[i + 1] == '(') {
      if (word[i + 2] == '(') {
        // $((math)) runs no commands: skip to its end.
        const char *close = strstr(word + i + 3, "))");
        if (!close) { say(why, why_size, "unfinished $(("); return 0; }
        i = (size_t)(close - word) + 1;
        continue;
      }
      int unterminated = 0;
      size_t end = syntax_subst_end(word, i, &unterminated);
      if (unterminated || end < i + 3) { say(why, why_size, "unfinished $("); return 0; }
      char *inner = strndup(word + i + 2, end - i - 3);
      int ok = check_text(inner, why, why_size, depth + 1);
      free(inner);
      if (!ok) return 0;
      i = end - 1;
    }
    else if (c == '`') {
      const char *close = strchr(word + i + 1, '`');
      if (!close) { say(why, why_size, "unfinished `"); return 0; }
      char *inner = strndup(word + i + 1, close - word - i - 1);
      int ok = check_text(inner, why, why_size, depth + 1);
      free(inner);
      if (!ok) return 0;
      i = close - word;
    }
  }
  return 1;
}

static int not_listed(const char *name, char *why, size_t why_size) {
  char message[200];
  snprintf(message, sizeof(message), "%s isn't in LIVE_COMMANDS", name);
  say(why, why_size, message);
  return 0;
}

// Commands that run another command (xargs grep ..., env LANG=C sort):
// the one they run must be listed as well. Returns the index of that
// command's name in argv, or -1 when there's none.
static int wrapped_command(char **argv, int argc) {
  const char *name = argv[0];
  int i = 1;
  if (!strcmp(name, "xargs")) {
    for (;i < argc && argv[i][0] == '-';i++) {
      const char *a = argv[i];
      if (!a[2] && strchr("nIdPLsEa", a[1])) i++;
    }
    return i < argc ? i : -1;
  }
  if (!strcmp(name, "env")) {
    for (;i < argc && (argv[i][0] == '-' || strchr(argv[i], '='));i++) {
      if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "-C")) i++;
    }
    return i < argc ? i : -1;
  }
  if (!strcmp(name, "timeout") || !strcmp(name, "nice") || !strcmp(name, "stdbuf") || !strcmp(name, "time") ||
      !strcmp(name, "command") || !strcmp(name, "nohup")) {
    for (;i < argc && argv[i][0] == '-';i++) {
      if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "-s") || !strcmp(argv[i], "-k")) i++;
    }
    if (!strcmp(name, "timeout") && i < argc) i++;  // the duration
    return i < argc ? i : -1;
  }
  return -1;
}

// Checks one command (unquoted words) and rewrites it so it writes no
// files. Fills `out` with the words to run. Returns 0 (with `why`) when
// it can't be previewed.
static int plan_command(Plan *plan, ArgList *words, ArgList *out, char *why, size_t why_size) {
  char **argv = words->items;
  int argc = words->count;
  const char *name = argv[0];
  // fish's `and` / `or` / `not` (and `!`) just lead into the real command.
  if ((!strcmp(name, "and") || !strcmp(name, "or") || !strcmp(name, "not") || !strcmp(name, "!")) && argc > 1) {
    arglist_append(out, name);
    ArgList rest = { argv + 1, argc - 1, argc - 1 };
    return plan_command(plan, &rest, out, why, why_size);
  }
  // A path like /usr/bin/grep counts as grep.
  const char *base = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
  if (!listed(name) && !listed(base)) return not_listed(name, why, why_size);

  for (int depth = 0, at = 0;depth < 4;depth++) {
    int next = wrapped_command(argv + at, argc - at);
    if (next < 0) break;
    at += next;
    if (!listed(argv[at])) return not_listed(argv[at], why, why_size);
  }

  if (is_grep(base)) collect_grep(plan, argv, argc);

  arglist_append(out, name);
  for (int i = 1;i < argc;i++) {
    const char *arg = argv[i];
    if (!strcmp(base, "sed") && (!strncmp(arg, "--in-place", 10) ||
                                 (arg[0] == '-' && arg[1] != '-' && strchr(arg, 'i') != NULL &&
                                  strcspn(arg, "ef") > (size_t)(strchr(arg, 'i') - arg)))) {
      // Show the edited text instead of saving it. In -Ei the E stays;
      // whatever follows the i is a backup suffix (-i.bak).
      add_note(plan, "sed -i would change %s (showing the new text)", argv[argc - 1]);
      if (arg[1] != '-' && strchr(arg, 'i') - arg > 1) {
        char kept[64];
        snprintf(kept, sizeof(kept), "%.*s", (int)(strchr(arg, 'i') - arg), arg);
        arglist_append(out, kept);
      }
      continue;
    }
    if (!strcmp(base, "sort") && (!strcmp(arg, "-o") || !strncmp(arg, "--output", 8) || (!strncmp(arg, "-o", 2) && arg[2]))) {
      const char *target = !strcmp(arg, "-o") ? (i + 1 < argc ? argv[i + 1] : "") :
                           !strncmp(arg, "--output=", 9) ? arg + 9 : (!strncmp(arg, "-o", 2) ? arg + 2 : "");
      if (!strcmp(arg, "-o") || !strcmp(arg, "--output")) i++;
      add_note(plan, "sort -o would write to %s", target);
      continue;
    }
    if ((!strcmp(base, "awk") || !strcmp(base, "gawk")) && !strcmp(arg, "-i") && i + 1 < argc && !strcmp(argv[i + 1], "inplace")) {
      i++;
      add_note(plan, "%s", "awk -i inplace would change the files (showing the new text)");
      continue;
    }
    if (!strcmp(base, "find")) {
      if (!strcmp(arg, "-delete")) { add_note(plan, "%s", "find -delete would delete these"); continue; }
      if (!strcmp(arg, "-ok") || !strcmp(arg, "-okdir")) { say(why, why_size, "find -ok asks questions"); return 0; }
      if (!strcmp(arg, "-fprint") || !strcmp(arg, "-fprint0") || !strcmp(arg, "-fls") || !strcmp(arg, "-fprintf")) {
        if (i + 1 < argc) add_note(plan, "find would write the list to %s", argv[i + 1]);
        arglist_append(out, !strcmp(arg, "-fprintf") ? "-printf" : !strcmp(arg, "-fls") ? "-ls" : !strcmp(arg, "-fprint0") ? "-print0" : "-print");
        i++;
        continue;
      }
      if ((!strcmp(arg, "-exec") || !strcmp(arg, "-execdir")) && i + 1 < argc) {
        if (!listed(argv[i + 1])) return not_listed(argv[i + 1], why, why_size);
        if (is_grep(argv[i + 1])) {
          // grep -exec ... {} ;  : collect up to the terminator
          int end = i + 1;
          while (end < argc && strcmp(argv[end], ";") && strcmp(argv[end], "+")) end++;
          collect_grep(plan, argv + i + 1, end - i - 1);
        }
      }
    }
    if (!strcmp(base, "tee")) {
      if (arg[0] != '-' || !arg[1]) { add_note(plan, "tee would also write to %s", arg); continue; }
      continue;  // -a and friends mean nothing without files
    }
    arglist_append(out, arg);
  }
  return 1;
}

// Builds the sandbox version of one parsed command list (tarsh syntax).
static int plan_native(Plan *plan, const char *line, char *why, size_t why_size) {
  CommandList list;
  char error[256];
  if (syntax_parse(line, &list, error, sizeof(error)) != 0) { say(why, why_size, "unfinished"); return 0; }

  size_t size = strlen(line) * 2 + 256;
  char *text = calloc(1, size);
  size_t used = 0;
  int ok = 1;
#define PUT(...) do { if (used < size) used += snprintf(text + used, size - used, __VA_ARGS__); } while (0)

  for (int p = 0;p < list.count && ok;p++) {
    Pipeline *pipeline = &list.pipelines[p];
    for (int c = 0;c < pipeline->count && ok;c++) {
      Command *command = &pipeline->commands[c];
      if (command->words.count == 0) { ok = 0; say(why, why_size, "empty command"); break; }
      for (int k = 0;k < command->words.count && ok;k++) {
        ok = check_substitutions(command->words.items[k], why, why_size, 0);
      }
      if (!ok) break;

      // The command name, unquoted; the other words stay as typed so
      // globs and variables expand in the sandbox like they would for real.
      ArgList unquoted, out;
      arglist_init(&unquoted);
      arglist_init(&out);
      for (int k = 0;k < command->words.count;k++) {
        ArgList one;
        arglist_init(&one);
        parser_split_words(command->words.items[k], &one);
        arglist_append(&unquoted, one.count ? one.items[0] : "");
        arglist_free(&one);
      }
      ok = plan_command(plan, &unquoted, &out, why, why_size);
      if (ok) {
        // Map the kept words back to what was typed (plan_command only
        // drops or swaps words), so quoting survives.
        int typed = 0;
        for (int k = 0;k < out.count;k++) {
          int found = -1;
          for (int t = typed;t < unquoted.count;t++) {
            if (!strcmp(unquoted.items[t], out.items[k])) { found = t; break; }
          }
          if (found >= 0) { PUT("%s ", command->words.items[found]); typed = found + 1; }
          else {
            char *quoted = shell_quote(out.items[k]);
            PUT("%s ", quoted);
            free(quoted);
          }
        }
      }
      arglist_free(&unquoted);
      arglist_free(&out);
      if (!ok) break;

      for (int r = 0;r < command->redir_count;r++) {
        Redirect *redirect = &command->redirs[r];
        const char *target = redirect->target ? redirect->target : "";
        ArgList t;
        arglist_init(&t);
        parser_split_words(target, &t);
        const char *shown = t.count ? t.items[0] : target;
        int to_null = !strcmp(shown, "/dev/null");
        switch (redirect->kind) {
          case REDIR_IN:
            if (redirect->fd == 0) PUT("<%s ", target);
            else PUT("%d<%s ", redirect->fd, target);
            break;
          case REDIR_OUT:
          case REDIR_APPEND:
            if (redirect->fd == 1) {
              // The output comes to the preview instead of the file.
              if (!to_null) add_note(plan, redirect->kind == REDIR_APPEND ? "would add this to the end of %s" : "would write this to %s", shown);
              else PUT(">&%d ", NULL_FD);
            }
            else {
              // Errors (2> file) stay out of the preview, like they would
              // stay off the screen.
              PUT("%d>&%d ", redirect->fd, NULL_FD);
              if (!to_null && redirect->fd == 2) add_note(plan, "errors would go to %s", shown);
            }
            break;
          case REDIR_BOTH:
          case REDIR_BOTH_APPEND:
            if (!to_null) add_note(plan, "would write this (and errors) to %s", shown);
            if (to_null) PUT(">&%d 2>&%d ", NULL_FD, NULL_FD);
            else PUT("2>&1 ");
            break;
          case REDIR_DUP:
            PUT("%d>&%d ", redirect->fd, redirect->target_fd);
            break;
          case REDIR_CLOSE:
            PUT("%d>&- ", redirect->fd);
            break;
        }
        arglist_free(&t);
      }
      if (c + 1 < pipeline->count) PUT("| ");
    }
    if (!ok) break;
    if (p + 1 < list.count) {
      Connector connector = list.connectors[p];
      // & runs in the foreground here: the preview waits for everything.
      PUT("%s ", connector == CONNECT_AND ? "&&" : connector == CONNECT_OR ? "||" : ";");
    }
  }
#undef PUT
  syntax_free(&list);
  if (!ok) { free(text); return 0; }
  plan->line = text;
  return 1;
}

static int is_word_char(char c) {
  return c && !strchr(" \t\n;|&<>()`", c);
}

// One redirection in a bash/zsh line, at line[*i] (an optional number,
// then < > >> &> <<< or <<). Reads stay; writes to files are taken out
// (the output comes to the preview, with a note), /dev/null and error
// redirects go to NULL_FD. Moves *i past it.
static int redirect_backend(Plan *plan, const char *line, size_t *i, size_t *copied, char *rewritten,
                            size_t *written, size_t size, char *why, size_t why_size) {
  size_t start = *i;
  int fd = -1;
  if (line[*i] >= '0' && line[*i] <= '9') {
    fd = atoi(line + *i);
    while (line[*i] >= '0' && line[*i] <= '9') (*i)++;
  }
  int both = line[*i] == '&';
  if (both) (*i)++;
  int is_input = line[*i] == '<';
  int append = !is_input && line[*i + 1] == '>';
  int heredoc = is_input && line[*i + 1] == '<' && line[*i + 2] != '<';
  while (line[*i] == '<' || line[*i] == '>') (*i)++;
  if (line[*i] == '|') (*i)++;      // >|
  if (line[*i] == '&') {
    // 2>&1, >&2, <&0, 2>&-: descriptors only.
    (*i)++;
    while ((line[*i] >= '0' && line[*i] <= '9') || line[*i] == '-') (*i)++;
    return 1;
  }
  while (line[*i] == ' ' || line[*i] == '\t') (*i)++;
  size_t word_start = *i;
  int unterminated = 0;
  size_t end = syntax_word_end(line, *i, &unterminated);
  if (end <= word_start) { say(why, why_size, "unfinished redirect"); return 0; }
  *i = end;
  if (is_input) {
    if (heredoc) {
      // The body follows on the next lines, up to the delimiter.
      char delimiter[128];
      size_t kept = 0;
      for (size_t k = word_start;k < end && kept < sizeof(delimiter) - 1;k++) {
        if (line[k] != '\'' && line[k] != '"' && line[k] != '-') delimiter[kept++] = line[k];
      }
      delimiter[kept] = '\0';
      const char *body = strchr(line + *i, '\n');
      while (body) {
        body++;
        size_t length = strcspn(body, "\n");
        const char *trimmed = body + strspn(body, "\t");
        size_t tabs = (size_t)(trimmed - body);
        if (tabs <= length && length - tabs == kept && !strncmp(trimmed, delimiter, kept)) {
          *i = (size_t)(body - line) + length;
          break;
        }
        body = strchr(body, '\n');
        if (!body) *i = strlen(line);
      }
    }
    return 1;
  }

  char *raw = strndup(line + word_start, end - word_start);
  ArgList unquoted;
  arglist_init(&unquoted);
  parser_split_words(raw, &unquoted);
  const char *target = unquoted.count ? unquoted.items[0] : raw;
  int to_null = !strcmp(target, "/dev/null");
  char replacement[64];
  replacement[0] = '\0';
  if (both) {
    if (to_null) snprintf(replacement, sizeof(replacement), " >&%d 2>&%d ", NULL_FD, NULL_FD);
    else { snprintf(replacement, sizeof(replacement), " 2>&1 "); add_note(plan, "would write this (and errors) to %s", target); }
  }
  else if (fd == -1 || fd == 1) {
    if (to_null) snprintf(replacement, sizeof(replacement), " >&%d ", NULL_FD);
    else { snprintf(replacement, sizeof(replacement), " "); add_note(plan, append ? "would add this to the end of %s" : "would write this to %s", target); }
  }
  else {
    snprintf(replacement, sizeof(replacement), " %d>&%d ", fd, NULL_FD);
    if (!to_null && fd == 2) add_note(plan, "errors would go to %s", target);
  }
  arglist_free(&unquoted);
  free(raw);
  // Swap the redirect for its replacement.
  size_t n = start - *copied;
  if (*written + n + strlen(replacement) < size) {
    memcpy(rewritten + *written, line + *copied, n);
    *written += n;
    memcpy(rewritten + *written, replacement, strlen(replacement));
    *written += strlen(replacement);
  }
  *copied = end;
  return 1;
}

// Lines in bash/zsh syntax (here-strings, loops, process substitution):
// find every command they'd run and check each one is listed, and take
// out writing to files the same way as for tarsh lines. Sets plan->line.
static int check_backend(Plan *plan, const char *line, char *why, size_t why_size) {
  size_t size = strlen(line) * 2 + 64;
  char *rewritten = malloc(size);
  size_t written = 0;
  size_t copied = 0;   // line[0..copied) is already in `rewritten`
#define COPY_TO(position) do { size_t _n = (position) - copied; memcpy(rewritten + written, line + copied, _n); written += _n; copied = (position); } while (0)
#define PUT_TEXT(text) do { size_t _n = strlen(text); if (written + _n < size) { memcpy(rewritten + written, text, _n); written += _n; } } while (0)
  // bash/zsh and fish keywords: a command follows them (or nothing does).
  static const char *keep_start[] = { "if", "then", "else", "elif", "do", "while", "until", "!", "{", "time",
                                       "begin", "and", "or", "not", NULL };
  static const char *ends[] = { "fi", "done", "}", "esac", "end", NULL };
  int command_start = 1;
  int skip_to_separator = 0;
  for (size_t i = 0;line[i];) {
    char c = line[i];
    if (c == ' ' || c == '\t') { i++; continue; }
    // 2>file: the number belongs to the redirect.
    if (c >= '0' && c <= '9') {
      size_t k = i;
      while (line[k] >= '0' && line[k] <= '9') k++;
      if (line[k] == '>' || line[k] == '<') {
        if (!redirect_backend(plan, line, &i, &copied, rewritten, &written, size, why, why_size)) { free(rewritten); return 0; }
        continue;
      }
    }
    if (c == '&' && line[i + 1] == '>') {
      if (!redirect_backend(plan, line, &i, &copied, rewritten, &written, size, why, why_size)) { free(rewritten); return 0; }
      continue;
    }
    if (c == '\n' || c == ';' || c == '|' || c == '&' || c == '(' || c == ')' || c == '`') {
      command_start = c != ')';
      if (c == '\n' || c == ';') skip_to_separator = 0;
      i += ((c == '&' || c == '|' || c == ';') && line[i + 1] == c) ? 2 : 1;
      continue;
    }
    if (c == '#') { while (line[i] && line[i] != '\n') i++; continue; }
    if (c == '<' || c == '>') {
      if ((line[i + 1] == '(') || (c == '>' && line[i + 1] == '>' && line[i + 2] == '(')) {
        while (line[i] == '<' || line[i] == '>') i++;
        command_start = 1;  // <(cmd) and >(cmd)
        i++;
        continue;
      }
      if (!redirect_backend(plan, line, &i, &copied, rewritten, &written, size, why, why_size)) { free(rewritten); return 0; }
      continue;
    }
    // A word.
    int unterminated = 0;
    size_t end = syntax_word_end(line, i, &unterminated);
    if (end <= i) end = i + 1;
    char *raw = strndup(line + i, end - i);
    if (!check_substitutions(raw, why, why_size, 0)) { free(raw); { free(rewritten); return 0; } }
    if (command_start && !skip_to_separator) {
      ArgList one;
      arglist_init(&one);
      parser_split_words(raw, &one);
      const char *word = one.count ? one.items[0] : raw;
      int keyword = 0;
      for (int k = 0;keep_start[k];k++) if (!strcmp(word, keep_start[k])) keyword = 1;
      for (int k = 0;ends[k];k++) if (!strcmp(word, ends[k])) { keyword = 2; }
      if (!strcmp(word, "for") || !strcmp(word, "select")) { skip_to_separator = 1; command_start = 0; }
      else if (!strcmp(word, "case") || !strcmp(word, "function") || !strcmp(word, "coproc") || !strcmp(word, "switch")) {
        say(why, why_size, "case and functions aren't previewed");
        arglist_free(&one); free(raw); { free(rewritten); return 0; }
      }
      else if (keyword == 1) command_start = 1;
      else if (keyword == 2) command_start = 0;
      else if (strchr(word, '=') && word[0] != '=' && is_word_char(word[0])) command_start = 1;  // VAR=value cmd
      else if (!listed(word)) {
        not_listed(word, why, why_size);
        arglist_free(&one); free(raw); { free(rewritten); return 0; }
      }
      else {
        command_start = 0;
        if (is_grep(word)) {
          // Gather this command's words for its pattern.
          ArgList argv;
          arglist_init(&argv);
          size_t k = i;
          while (line[k] && !strchr("\n;|&<>()", line[k])) {
            while (line[k] == ' ' || line[k] == '\t') k++;
            if (!line[k] || strchr("\n;|&<>()", line[k])) break;
            int open = 0;
            size_t e = syntax_word_end(line, k, &open);
            if (e <= k) break;
            char *w = strndup(line + k, e - k);
            ArgList u;
            arglist_init(&u);
            parser_split_words(w, &u);
            arglist_append(&argv, u.count ? u.items[0] : w);
            arglist_free(&u);
            free(w);
            k = e;
          }
          if (argv.count) collect_grep(plan, argv.items, argv.count);
          arglist_free(&argv);
        }
      }
      arglist_free(&one);
    }
    free(raw);
    i = end;
  }
  COPY_TO(strlen(line));
  rewritten[written] = '\0';
  free(plan->line);
  plan->line = rewritten;
  return 1;
#undef COPY_TO
#undef PUT_TEXT
}

// Works out how to preview `text`. Returns 1 and fills `plan`, or 0 with
// the reason in `why`.
static int plan_line(const char *text, Plan *plan, char *why, size_t why_size) {
  memset(plan, 0, sizeof(*plan));
  if (!enabled) { say(why, why_size, "live preview is off"); return 0; }
  if (text[strspn(text, " \t\n")] == '\0') { say(why, why_size, "nothing typed"); return 0; }
  char *line = alias_expand_line(text);
  int ok = 0;
  if (script_claims(line)) {
    say(why, why_size, "tarsh script isn't previewed");
  }
  else if (lang_needs_backend(line)) {
    Foreign info;
    foreign_analyze(line, &info);
    if (!lang_installed(info.language)) say(why, why_size, "that shell isn't installed");
    else if (syntax_incomplete(line, !strcmp(info.language, "fish"))) say(why, why_size, "unfinished");
    else if (check_backend(plan, line, why, why_size)) {
      snprintf(plan->shell, sizeof(plan->shell), "%s", info.language);
      ok = 1;
    }
  }
  else {
    ok = plan_native(plan, line, why, why_size);
  }
  free(line);
  if (!ok) plan_free(plan);
  return ok;
}

// Checks a $(...) body: the same rules, recursively, without running it.
static int check_text(const char *text, char *why, size_t why_size, int depth) {
  if (depth > 4) { say(why, why_size, "too many nested $(...)"); return 0; }
  Plan inner;
  if (!plan_line(text, &inner, why, why_size)) return 0;
  // Writing inside $(...) can't be simulated without changing the result.
  int writes = inner.notes[0] != '\0';
  if (writes) say(why, why_size, "a $(...) that writes files isn't previewed");
  plan_free(&inner);
  return !writes;
}

int live_eligible(const char *text, char *why, size_t why_size) {
  Plan plan;
  if (!plan_line(text, &plan, why, why_size)) return 0;
  plan_free(&plan);
  return 1;
}

static char last_note[512];

const char *live_last_note(void) {
  return last_note;
}

// ---------------------------------------------------------------------------
// The sandbox
// ---------------------------------------------------------------------------

#if defined(__x86_64__)
#define AUDIT_ARCH_CURRENT AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define AUDIT_ARCH_CURRENT AUDIT_ARCH_AARCH64
#elif defined(__i386__)
#define AUDIT_ARCH_CURRENT AUDIT_ARCH_I386
#elif defined(__riscv) && __riscv_xlen == 64
#define AUDIT_ARCH_CURRENT AUDIT_ARCH_RISCV64
#endif

typedef struct {
  struct sock_filter items[160];
  int count;
}Program;

static void emit(Program *p, struct sock_filter instruction) {
  if (p->count < (int)(sizeof(p->items) / sizeof(p->items[0]))) p->items[p->count++] = instruction;
}

#define DENY SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)
#define LOAD_NR BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr))
#define LOAD_ARG(n) BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[n]))

static void deny_syscall(Program *p, long nr) {
  emit(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)nr, 0, 1));
  emit(p, (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, DENY));
}

// Deny `nr` when argument `arg` has any of the bits in `mask` set.
static void deny_with_flags(Program *p, long nr, int arg, unsigned mask) {
  emit(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)nr, 0, 4));
  emit(p, (struct sock_filter)LOAD_ARG(arg));
  emit(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, mask, 0, 1));
  emit(p, (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, DENY));
  emit(p, (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
}

// Deny `nr` when argument `arg` equals `value`.
static void deny_with_value(Program *p, long nr, int arg, unsigned value) {
  emit(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)nr, 0, 4));
  emit(p, (struct sock_filter)LOAD_ARG(arg));
  emit(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, value, 0, 1));
  emit(p, (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, DENY));
  emit(p, (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
}

static int install_sandbox(void) {
#ifndef AUDIT_ARCH_CURRENT
  return -1;
#else
  Program p = { .count = 0 };
  emit(&p, (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)));
  emit(&p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_CURRENT, 1, 0));
  emit(&p, (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
  emit(&p, (struct sock_filter)LOAD_NR);

  // Changing files, in every form.
  long denied[] = {
#ifdef SYS_creat
    SYS_creat,
#endif
#ifdef SYS_unlink
    SYS_unlink,
#endif
#ifdef SYS_rename
    SYS_rename,
#endif
#ifdef SYS_mkdir
    SYS_mkdir,
#endif
#ifdef SYS_rmdir
    SYS_rmdir,
#endif
#ifdef SYS_link
    SYS_link,
#endif
#ifdef SYS_symlink
    SYS_symlink,
#endif
#ifdef SYS_chmod
    SYS_chmod,
#endif
#ifdef SYS_chown
    SYS_chown,
#endif
#ifdef SYS_lchown
    SYS_lchown,
#endif
#ifdef SYS_mknod
    SYS_mknod,
#endif
#ifdef SYS_utime
    SYS_utime,
#endif
#ifdef SYS_utimes
    SYS_utimes,
#endif
#ifdef SYS_futimesat
    SYS_futimesat,
#endif
#ifdef SYS_renameat
    SYS_renameat,
#endif
    SYS_unlinkat, SYS_renameat2, SYS_mkdirat, SYS_linkat, SYS_symlinkat,
    SYS_fchmod, SYS_fchmodat, SYS_fchown, SYS_fchownat, SYS_truncate, SYS_mknodat, SYS_utimensat,
    SYS_setxattr, SYS_lsetxattr, SYS_fsetxattr, SYS_removexattr, SYS_lremovexattr, SYS_fremovexattr,
    SYS_mount, SYS_umount2, SYS_chroot, SYS_pivot_root, SYS_openat2,
    // network, other processes, kernel interfaces that bypass the rules
    SYS_socket, SYS_socketpair, SYS_connect, SYS_kill, SYS_tkill, SYS_tgkill, SYS_ptrace,
    SYS_process_vm_writev, SYS_io_uring_setup, SYS_bpf, SYS_perf_event_open, SYS_keyctl,
    SYS_add_key, SYS_request_key, SYS_setns, SYS_unshare, SYS_name_to_handle_at, SYS_open_by_handle_at,
  };
  for (size_t i = 0;i < sizeof(denied) / sizeof(denied[0]);i++) deny_syscall(&p, denied[i]);

  // Opening for writing (reading is fine).
  unsigned write_flags = O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND;
#ifdef SYS_open
  deny_with_flags(&p, SYS_open, 1, write_flags);
  emit(&p, (struct sock_filter)LOAD_NR);
#endif
  deny_with_flags(&p, SYS_openat, 2, write_flags);
  emit(&p, (struct sock_filter)LOAD_NR);
  // Typing into the terminal.
  deny_with_value(&p, SYS_ioctl, 1, TIOCSTI);
  emit(&p, (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));

  struct sock_fprog program = { (unsigned short)p.count, p.items };
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
  if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) return -1;
  return 0;
#endif
}

static void limit(int resource, rlim_t value) {
  struct rlimit l = { value, value };
  setrlimit(resource, &l);
}

// Keeps text the terminal can show safely: printable characters, tabs,
// UTF-8, and color codes. Everything else (cursor movement, titles,
// bells, raw control bytes) becomes a dot.
static void sanitize(char *text) {
  size_t out = 0;
  for (size_t i = 0;text[i];i++) {
    unsigned char c = (unsigned char)text[i];
    if (c == 0x1b && text[i + 1] == '[') {
      size_t j = i + 2;
      while (text[j] && ((text[j] >= '0' && text[j] <= '9') || text[j] == ';')) j++;
      if (text[j] == 'm' || text[j] == 'K') {
        if (text[j] == 'm') { memmove(text + out, text + i, j - i + 1); out += j - i + 1; }
        i = j;
        continue;
      }
      text[out++] = '.';
      continue;
    }
    if (c == '\n' || c == '\t' || c >= 0x20) { if (c != 0x7f) { text[out++] = (char)c; continue; } }
    text[out++] = '.';
  }
  text[out] = '\0';
}

// ---------------------------------------------------------------------------
// Highlighting what the greps matched
// ---------------------------------------------------------------------------

#define MATCH_ON "\x1b[1;33m"
#define MATCH_OFF "\x1b[22;39m"

// Turns a grep pattern into a regex the C library understands.
static int compile_pattern(const Pattern *pattern, regex_t *regex) {
  size_t size = strlen(pattern->source) * 2 + 32;
  char *text = malloc(size);
  size_t used = 0;
  int extended = pattern->extended;
  if (pattern->fixed) {
    // Plain text: escape everything special (basic syntax).
    extended = 0;
    for (const char *c = pattern->source;*c;c++) {
      if (strchr("\\.[]*^$", *c)) text[used++] = '\\';
      text[used++] = *c;
    }
    text[used] = '\0';
  }
  else {
    snprintf(text, size, "%s", pattern->source);
    used = strlen(text);
  }
  if (pattern->words || pattern->whole_line) {
    char *wrapped = malloc(used + 32);
    const char *open = pattern->whole_line ? "^" : "\\<";
    const char *close = pattern->whole_line ? "$" : "\\>";
    if (extended) sprintf(wrapped, "%s(%s)%s", open, text, close);
    else sprintf(wrapped, "%s\\(%s\\)%s", open, text, close);
    free(text);
    text = wrapped;
  }
  int flags = (extended ? REG_EXTENDED : 0) | (pattern->ignore_case ? REG_ICASE : 0) | REG_NEWLINE;
  int result = -1;
  if (!pattern->perl) {
    result = regcomp(regex, text, flags);
  }
  else {
    // Perl and ripgrep extras (\d, lazy +?) that plain ERE lacks, translated.
    char *translated = malloc(strlen(text) * 6 + 1);
    size_t t = 0;
    for (const char *c = text;*c;c++) {
      if (c[0] == '\\' && c[1] == 'd') { strcpy(translated + t, "[0-9]"); t += 5; c++; }
      else if (c[0] == '\\' && c[1] == 'D') { strcpy(translated + t, "[^0-9]"); t += 6; c++; }
      else if ((c[0] == '+' || c[0] == '*' || c[0] == '?') && c[1] == '?') { translated[t++] = *c; c++; }
      else translated[t++] = *c;
    }
    translated[t] = '\0';
    result = regcomp(regex, translated, flags);
    free(translated);
  }
  free(text);
  return result == 0;
}

// Marks every match of the line's grep patterns in `output`. Returns a
// new string, or NULL when there was nothing to look for.
static char *highlight_matches(const Plan *plan, const char *output) {
  if (plan->pattern_count == 0 || output[0] == '\0') return NULL;
  regex_t regexes[MAX_PATTERNS];
  int compiled = 0;
  for (int i = 0;i < plan->pattern_count;i++) {
    if (compile_pattern(&plan->patterns[i], &regexes[compiled])) compiled++;
  }
  if (compiled == 0) return NULL;

  size_t length = strlen(output);
  // Marks per byte: which bytes are inside a match.
  char *marked = calloc(length + 1, 1);
  const char *line = output;
  while (*line) {
    size_t line_length = strcspn(line, "\n");
    char *copy = strndup(line, line_length);
    for (int r = 0;r < compiled;r++) {
      size_t offset = 0;
      while (offset <= line_length) {
        regmatch_t match;
        if (regexec(&regexes[r], copy + offset, 1, &match, offset ? REG_NOTBOL : 0) != 0) break;
        size_t from = offset + (size_t)match.rm_so, to = offset + (size_t)match.rm_eo;
        for (size_t k = from;k < to;k++) marked[(line - output) + k] = 1;
        offset = to > from ? to : to + 1;
      }
    }
    free(copy);
    line += line_length;
    if (*line == '\n') line++;
  }
  for (int r = 0;r < compiled;r++) regfree(&regexes[r]);

  size_t size = length * 2 + 64;
  char *result = malloc(size);
  size_t used = 0;
  int on = 0;
  for (size_t i = 0;i < length;i++) {
    int want = marked[i] && output[i] != '\n' && output[i] != '\x1b';
    if (output[i] == '\x1b') {
      // Keep the program's own colors as they are.
      size_t skip = escape_length(output + i, length - i);
      if (on) { used += sprintf(result + used, MATCH_OFF); on = 0; }
      if (used + skip + 32 >= size) { size = size * 2 + skip; result = realloc(result, size); }
      memcpy(result + used, output + i, skip);
      used += skip;
      i += skip - 1;
      continue;
    }
    if (want != on) {
      used += sprintf(result + used, "%s", want ? MATCH_ON : MATCH_OFF);
      on = want;
    }
    if (used + 32 >= size) { size *= 2; result = realloc(result, size); }
    result[used++] = output[i];
  }
  if (on) used += sprintf(result + used, MATCH_OFF);
  result[used] = '\0';
  free(marked);
  return result;
}

static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

char *live_run(const char *line, int wanted_lines, int *total_lines, int *failed) {
  *total_lines = 0;
  *failed = 0;
  int out_pipe[2], err_pipe[2];
  if (pipe2(out_pipe, O_CLOEXEC) != 0) return NULL;
  if (pipe2(err_pipe, O_CLOEXEC) != 0) { close(out_pipe[0]); close(out_pipe[1]); return NULL; }

  Plan plan;
  char why[200];
  last_note[0] = '\0';
  if (!plan_line(line, &plan, why, sizeof(why))) {
    close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
    *failed = 1;
    return strdup(why);
  }
  snprintf(last_note, sizeof(last_note), "%s", plan.notes);

  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
    plan_free(&plan);
    return NULL;
  }
  if (pid == 0) {
    setsid();                                   // no terminal at all
    jobs_become_subshell();
    signal(SIGPIPE, SIG_DFL);
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0) { dup2(null, STDIN_FILENO); close(null); }
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    limit(RLIMIT_FSIZE, 0);
    limit(RLIMIT_CPU, 2);
#ifndef __SANITIZE_ADDRESS__
    // AddressSanitizer reserves terabytes of shadow memory, so debug builds skip this.
    limit(RLIMIT_AS, (rlim_t)2048 * 1024 * 1024);
#endif
    limit(RLIMIT_CORE, 0);
    setenv("TMPDIR", "/nonexistent-tarsh-preview", 1);
    // Redirects to /dev/null become this descriptor.
    int sink = open("/dev/null", O_WRONLY);
    if (sink >= 0 && sink != NULL_FD) { dup2(sink, NULL_FD); close(sink); }
    if (install_sandbox() != 0) {
      fprintf(stderr, "live preview needs seccomp, which this system doesn't offer\n");
      _exit(125);
    }
    // Inside the sandbox now: run the line with tarsh itself, or with the
    // shell its syntax belongs to.
    if (plan.shell[0]) {
      char program[PATH_MAX];
      if (path_lookup(plan.shell, program, sizeof(program))) {
        if (!strcmp(plan.shell, "fish")) {
          // No config (it could do anything) and no history.
          execl(program, "fish", "--no-config", "--private", "-c", plan.line, (char *)NULL);
        }
        execl(program, plan.shell, "-c", plan.line, (char *)NULL);
      }
      fprintf(stderr, "%s isn't installed\n", plan.shell);
      _exit(127);
    }
    int status = exec_run_line(plan.line);
    fflush(stdout);
    fflush(stderr);
    _exit(status);
  }
  close(out_pipe[1]);
  close(err_pipe[1]);

  char *output = malloc(65536);
  char *errors = malloc(4096);
  size_t output_length = 0, error_length = 0;
  int lines = 0;
  long deadline = now_ms() + 1000;
  int out_open = 1, err_open = 1;
  int enough = 0;
  while ((out_open || err_open) && !enough) {
    long remaining = deadline - now_ms();
    if (remaining <= 0) break;
    struct pollfd fds[2] = { { out_pipe[0], POLLIN, 0 }, { err_pipe[0], POLLIN, 0 } };
    if (poll(fds, 2, (int)remaining) <= 0) continue;
    if (out_open && (fds[0].revents & (POLLIN | POLLHUP))) {
      ssize_t got = read(out_pipe[0], output + output_length, 65535 - output_length);
      if (got <= 0) out_open = 0;
      else {
        for (ssize_t k = 0;k < got;k++) if (output[output_length + k] == '\n') lines++;
        output_length += got;
        // A few lines past what we show, to know there's more.
        if (lines > wanted_lines + 50 || output_length >= 65535) enough = 1;
      }
    }
    if (err_open && (fds[1].revents & (POLLIN | POLLHUP))) {
      ssize_t got = read(err_pipe[0], errors + error_length, 4095 - error_length);
      if (got <= 0) err_open = 0;
      else error_length += got;
      if (error_length >= 4095) err_open = 0;
    }
  }
  // Done or out of time: stop the whole preview.
  kill(-pid, SIGKILL);
  kill(pid, SIGKILL);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  close(out_pipe[0]);
  close(err_pipe[0]);
  output[output_length] = '\0';
  errors[error_length] = '\0';

  if (output_length > 0 && output[output_length - 1] != '\n') lines++;
  *total_lines = lines;
  int timed_out = now_ms() >= deadline;

  // Keep the first `wanted_lines` lines.
  char *cut = output;
  for (int k = 0;k < wanted_lines && cut;k++) {
    cut = strchr(cut, '\n');
    if (cut) cut++;
  }
  if (cut && *cut) *cut = '\0';
  if (output_length == 0 && error_length > 0) {
    // Show the first error line instead.
    errors[strcspn(errors, "\n")] = '\0';
    free(output);
    sanitize(errors);
    *failed = 1;
    plan_free(&plan);
    return errors;
  }
  free(errors);
  if (output_length == 0 && timed_out) {
    free(output);
    *failed = 1;
    plan_free(&plan);
    return strdup("(still running after a second; preview stopped)");
  }
  sanitize(output);
  char *marked = highlight_matches(&plan, output);
  plan_free(&plan);
  if (marked != NULL) {
    free(output);
    return marked;
  }
  return output;
}

// ---------------------------------------------------------------------------
// live / hints builtin
// ---------------------------------------------------------------------------

int live_builtin(int argc, char **argv) {
  if (argc >= 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
    enabled = !strcmp(argv[1], "on");
    printf("live preview %s (LIVE in config.t keeps it)\n", enabled ? "on" : "off");
    return 0;
  }
  if (argc >= 3 && (!strcmp(argv[1], "add") || !strcmp(argv[1], "remove"))) {
    if (!commands_ready) live_setup();
    for (int i = 2;i < argc;i++) {
      int index = -1;
      for (int k = 0;k < commands.count;k++) if (!strcmp(commands.items[k], argv[i])) index = k;
      if (!strcmp(argv[1], "add") && index < 0) arglist_append(&commands, argv[i]);
      if (!strcmp(argv[1], "remove") && index >= 0) {
        free(commands.items[index]);
        for (int k = index;k < commands.count - 1;k++) commands.items[k] = commands.items[k + 1];
        commands.count--;
        commands.items[commands.count] = NULL;
      }
    }
    printf("live commands for this session updated (LIVE_COMMANDS in config.t keeps them)\n");
    return 0;
  }
  if (argc >= 2 && !strcmp(argv[1], "test")) {
    if (argc < 3) { fprintf(stderr, "usage: live test 'COMMAND LINE'\n"); return 2; }
    char why[200];
    if (!live_eligible(argv[2], why, sizeof(why))) { printf("not previewed: %s\n", why); return 1; }
    int total = 0, failed = 0;
    char *output = live_run(argv[2], max_lines, &total, &failed);
    if (!output) { printf("couldn't run the preview\n"); return 1; }
    if (last_note[0]) printf("(%s)\n", last_note);
    if (!isatty(STDOUT_FILENO)) {
      // Matches are marked with colors; leave them out of files and pipes.
      size_t kept = 0;
      for (size_t i = 0;output[i];) {
        if (output[i] == '\x1b') { i += escape_length(output + i, strlen(output + i)); continue; }
        output[kept++] = output[i++];
      }
      output[kept] = '\0';
    }
    fputs(output, stdout);
    if (output[0] && output[strlen(output) - 1] != '\n') putchar('\n');
    if (total > max_lines) printf("... %d lines in all\n", total);
    free(output);
    return failed;
  }
  if (!commands_ready) live_setup();
  printf("live preview: %s   hints: %s   lines: %d   delay: %dms\n", enabled ? "on" : "off", hints ? "on" : "off", max_lines, delay_ms);
  printf("previewed commands:");
  for (int i = 0;i < commands.count;i++) printf(" %s", commands.items[i]);
  printf("\n\nusage: live on|off   live add CMD...   live remove CMD...   live test 'LINE'\n");
  printf("       hints on|off  (the explanation under the prompt)\n");
  printf("\nthe preview runs in a sandbox that can't write, delete or rename files, use the\n");
  printf("network or signal other programs, and is stopped after a second.\n");
  return 0;
}
