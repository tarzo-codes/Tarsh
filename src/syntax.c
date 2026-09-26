#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "syntax.h"

static void *grow(void *items, int count, int *capacity, size_t item_size) {
  if (count < *capacity) {
    return items;
  }
  int new_capacity = *capacity ? *capacity * 2 : 4;
  void *grown = realloc(items, new_capacity * item_size);
  if (grown == NULL) {
    perror("tarsh: out of memory");
    exit(EXIT_FAILURE);
  }
  *capacity = new_capacity;
  return grown;
}

static int is_blank(char c) {
  return c == ' ' || c == '\t';
}

static int is_operator_char(char c) {
  return c == '|' || c == '&' || c == ';' || c == '<' || c == '>';
}

// Skips a '...' string starting at text[i] == '\''.
static size_t skip_single(const char *text, size_t i, int *unterminated) {
  i++;
  while (text[i] != '\0' && text[i] != '\'') {
    i++;
  }
  if (text[i] == '\0') {
    *unterminated = 1;
    return i;
  }
  return i + 1;
}

static size_t skip_backtick(const char *text, size_t i, int *unterminated) {
  i++;
  while (text[i] != '\0' && text[i] != '`') {
    if (text[i] == '\\' && text[i + 1] != '\0') {
      i++;
    }
    i++;
  }
  if (text[i] == '\0') {
    *unterminated = 1;
    return i;
  }
  return i + 1;
}

static size_t skip_double(const char *text, size_t i, int *unterminated);

size_t syntax_subst_end(const char *text, size_t start, int *unterminated) {
  size_t i = start + 2;
  int depth = 1;
  while (text[i] != '\0' && depth > 0) {
    char c = text[i];
    if (c == '\\' && text[i + 1] != '\0') {
      i += 2;
    }
    else if (c == '\'') {
      i = skip_single(text, i, unterminated);
    }
    else if (c == '"') {
      i = skip_double(text, i, unterminated);
    }
    else if (c == '`') {
      i = skip_backtick(text, i, unterminated);
    }
    else {
      if (c == '(') depth++;
      if (c == ')') depth--;
      i++;
    }
  }
  if (depth > 0) {
    *unterminated = 1;
  }
  return i;
}

static size_t skip_braces(const char *text, size_t i, int *unterminated) {
  // text[i] == '$', text[i + 1] == '{'
  i += 2;
  while (text[i] != '\0' && text[i] != '}') {
    i++;
  }
  if (text[i] == '\0') {
    *unterminated = 1;
    return i;
  }
  return i + 1;
}

static size_t skip_double(const char *text, size_t i, int *unterminated) {
  i++;
  while (text[i] != '\0' && text[i] != '"') {
    if (text[i] == '\\' && text[i + 1] != '\0') {
      i += 2;
    }
    else if (text[i] == '$' && text[i + 1] == '(') {
      i = syntax_subst_end(text, i, unterminated);
    }
    else if (text[i] == '`') {
      i = skip_backtick(text, i, unterminated);
    }
    else {
      i++;
    }
  }
  if (text[i] == '\0') {
    *unterminated = 1;
    return i;
  }
  return i + 1;
}

size_t syntax_word_end(const char *text, size_t start, int *unterminated) {
  size_t i = start;
  *unterminated = 0;
  while (text[i] != '\0') {
    char c = text[i];
    if (is_blank(c) || c == '\n' || is_operator_char(c)) {
      break;
    }
    if (c == '\\') {
      i += (text[i + 1] != '\0') ? 2 : 1;
    }
    else if (c == '\'') {
      i = skip_single(text, i, unterminated);
    }
    else if (c == '"') {
      i = skip_double(text, i, unterminated);
    }
    else if (c == '`') {
      i = skip_backtick(text, i, unterminated);
    }
    else if (c == '$' && text[i + 1] == '(') {
      i = syntax_subst_end(text, i, unterminated);
    }
    else if (c == '$' && text[i + 1] == '{') {
      i = skip_braces(text, i, unterminated);
    }
    else {
      i++;
    }
  }
  return i;
}

static int all_digits(const char *text, size_t length) {
  if (length == 0) {
    return 0;
  }
  for (size_t i = 0;i < length;i++) {
    if (text[i] < '0' || text[i] > '9') {
      return 0;
    }
  }
  return 1;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

typedef struct {
  CommandList *list;
  int pipeline_capacity;
  Pipeline current;
  int command_capacity;
  Command command;
  int redir_capacity;
  size_t pipeline_start;
}ParseState;

static void command_reset(ParseState *state) {
  arglist_init(&state->command.words);
  state->command.redirs = NULL;
  state->command.redir_count = 0;
  state->redir_capacity = 0;
}

static void command_free(Command *command) {
  arglist_free(&command->words);
  for (int i = 0;i < command->redir_count;i++) {
    free(command->redirs[i].target);
  }
  free(command->redirs);
}

static int command_empty(const Command *command) {
  return command->words.count == 0 && command->redir_count == 0;
}

static void pipeline_reset(ParseState *state, size_t start) {
  state->current.commands = NULL;
  state->current.count = 0;
  state->current.text = NULL;
  state->command_capacity = 0;
  state->pipeline_start = start;
}

static void finish_command(ParseState *state) {
  state->current.commands = grow(state->current.commands, state->current.count,
                                 &state->command_capacity, sizeof(Command));
  state->current.commands[state->current.count++] = state->command;
  command_reset(state);
}

static void finish_pipeline(ParseState *state, const char *text, size_t end, Connector connector) {
  // Trim the source text for display.
  size_t start = state->pipeline_start;
  while (start < end && (is_blank(text[start]) || text[start] == '\n')) start++;
  while (end > start && (is_blank(text[end - 1]) || text[end - 1] == '\n')) end--;
  state->current.text = strndup(text + start, end - start);

  CommandList *list = state->list;
  int capacity = state->pipeline_capacity;
  list->pipelines = grow(list->pipelines, list->count, &capacity, sizeof(Pipeline));
  int connector_capacity = state->pipeline_capacity;
  list->connectors = grow(list->connectors, list->count, &connector_capacity, sizeof(Connector));
  state->pipeline_capacity = capacity;

  list->pipelines[list->count] = state->current;
  list->connectors[list->count] = connector;
  list->count++;
}

static void add_redirect(ParseState *state, RedirKind kind, int fd, int target_fd, char *target) {
  state->command.redirs = grow(state->command.redirs, state->command.redir_count,
                               &state->redir_capacity, sizeof(Redirect));
  Redirect *redirect = &state->command.redirs[state->command.redir_count++];
  redirect->kind = kind;
  redirect->fd = fd;
  redirect->target_fd = target_fd;
  redirect->target = target;
}

// Reads the word after a redirection operator. Returns NULL (and fills
// `error`) if there isn't one.
static char *read_target(const char *text, size_t *i, const char *operator_text,
                         char *error, size_t error_size) {
  while (is_blank(text[*i])) {
    (*i)++;
  }
  if (text[*i] == '\0' || text[*i] == '\n' || is_operator_char(text[*i])) {
    snprintf(error, error_size, "syntax error: '%s' needs a file name after it", operator_text);
    return NULL;
  }
  int unterminated = 0;
  size_t end = syntax_word_end(text, *i, &unterminated);
  char *word = strndup(text + *i, end - *i);
  *i = end;
  return word;
}

// Parses a redirection at text[*i] ('<', '>' or '&>'). `fd` is an
// explicit descriptor like the 2 in 2>, or -1.
static int parse_redirect(ParseState *state, const char *text, size_t *i, int fd,
                          char *error, size_t error_size) {
  const char *op = text + *i;
  RedirKind kind;
  int default_fd;
  size_t length;

  if (strncmp(op, "&>>", 3) == 0) {
    kind = REDIR_BOTH_APPEND; default_fd = 1; length = 3;
  }
  else if (strncmp(op, "&>", 2) == 0) {
    kind = REDIR_BOTH; default_fd = 1; length = 2;
  }
  else if (strncmp(op, ">>", 2) == 0) {
    kind = REDIR_APPEND; default_fd = 1; length = 2;
  }
  else if (strncmp(op, ">&", 2) == 0 || strncmp(op, "<&", 2) == 0) {
    default_fd = (op[0] == '>') ? 1 : 0;
    char operator_text[3] = { op[0], '&', '\0' };
    *i += 2;
    char *target = read_target(text, i, operator_text, error, error_size);
    if (target == NULL) {
      return -1;
    }
    int use_fd = (fd >= 0) ? fd : default_fd;
    if (strcmp(target, "-") == 0) {
      add_redirect(state, REDIR_CLOSE, use_fd, -1, NULL);
      free(target);
    }
    else if (all_digits(target, strlen(target))) {
      add_redirect(state, REDIR_DUP, use_fd, atoi(target), NULL);
      free(target);
    }
    else if (op[0] == '>' && fd < 0) {
      // bash's >&file means &>file.
      add_redirect(state, REDIR_BOTH, 1, -1, target);
    }
    else {
      snprintf(error, error_size, "syntax error: '%s' needs a descriptor number like 1 or 2",
               operator_text);
      free(target);
      return -1;
    }
    return 0;
  }
  else if (strncmp(op, "<<", 2) == 0) {
    snprintf(error, error_size, "here-documents (<<) need a shell language: they run in bash/zsh");
    return -1;
  }
  else if (op[0] == '>') {
    kind = REDIR_OUT; default_fd = 1; length = (op[1] == '|') ? 2 : 1;
  }
  else {
    kind = REDIR_IN; default_fd = 0; length = 1;
  }

  if ((kind == REDIR_BOTH || kind == REDIR_BOTH_APPEND) && fd >= 0) {
    snprintf(error, error_size, "syntax error near '&>'");
    return -1;
  }

  char operator_text[4];
  snprintf(operator_text, sizeof(operator_text), "%.*s", (int)length, op);
  *i += length;
  char *target = read_target(text, i, operator_text, error, error_size);
  if (target == NULL) {
    return -1;
  }
  add_redirect(state, kind, (fd >= 0) ? fd : default_fd, -1, target);
  return 0;
}

int syntax_parse(const char *text, CommandList *list, char *error, size_t error_size) {
  list->pipelines = NULL;
  list->connectors = NULL;
  list->count = 0;

  ParseState state;
  memset(&state, 0, sizeof(state));
  state.list = list;
  command_reset(&state);
  pipeline_reset(&state, 0);

  size_t i = 0;
  while (1) {
    while (is_blank(text[i])) {
      i++;
    }
    char c = text[i];

    // A newline ends a pipeline, except right after | && || where the
    // command obviously continues.
    if (c == '\n' && command_empty(&state.command) && state.current.count > 0) {
      i++;
      continue;
    }

    if (c == '\0' || c == '\n' || c == ';' ||
        (c == '&' && text[i + 1] == '&') || (c == '|' && text[i + 1] == '|') ||
        (c == '&' && text[i + 1] != '>')) {
      Connector connector = CONNECT_SEQ;
      const char *operator_text = ";";
      size_t length = 1;
      if (c == '&' && text[i + 1] == '&') { connector = CONNECT_AND; operator_text = "&&"; length = 2; }
      else if (c == '|') { connector = CONNECT_OR; operator_text = "||"; length = 2; }
      else if (c == '&') { connector = CONNECT_BG; operator_text = "&"; }
      else if (c == '\0') { length = 0; }

      int has_command = !command_empty(&state.command);
      if (has_command) {
        finish_command(&state);
      }
      if (state.current.count > 0) {
        finish_pipeline(&state, text, i, connector);
        pipeline_reset(&state, i + length);
      }
      else if (c != '\0' && c != '\n' && c != ';') {
        snprintf(error, error_size, "syntax error: '%s' needs a command before it", operator_text);
        goto fail;
      }
      else if (c == ';' && list->count == 0) {
        snprintf(error, error_size, "syntax error: unexpected ';'");
        goto fail;
      }
      else {
        state.pipeline_start = i + length;
      }

      if (c == '\0') {
        break;
      }
      i += length;
      // A trailing && or || with nothing after it can't run.
      if ((connector == CONNECT_AND || connector == CONNECT_OR)) {
        size_t look = i;
        while (is_blank(text[look]) || text[look] == '\n') look++;
        if (text[look] == '\0') {
          snprintf(error, error_size, "syntax error: '%s' needs a command after it", operator_text);
          goto fail;
        }
      }
      continue;
    }

    if (c == '|') {
      if (command_empty(&state.command)) {
        snprintf(error, error_size, "syntax error: '|' needs a command before it");
        goto fail;
      }
      finish_command(&state);
      i++;
      size_t look = i;
      while (is_blank(text[look]) || text[look] == '\n') look++;
      if (text[look] == '\0') {
        snprintf(error, error_size, "syntax error: '|' needs a command after it");
        goto fail;
      }
      continue;
    }

    if (c == '#') {
      // Comment: skip to the end of the line.
      while (text[i] != '\0' && text[i] != '\n') {
        i++;
      }
      continue;
    }

    if (c == '<' || c == '>' || (c == '&' && text[i + 1] == '>')) {
      if (parse_redirect(&state, text, &i, -1, error, error_size) != 0) {
        goto fail;
      }
      continue;
    }

    int unterminated = 0;
    size_t end = syntax_word_end(text, i, &unterminated);
    if (unterminated) {
      snprintf(error, error_size, "syntax error: a quote or $( was opened but never closed");
      goto fail;
    }

    // 2>file, 2>&1: digits glued to a redirection name a descriptor.
    if ((text[end] == '<' || text[end] == '>') && all_digits(text + i, end - i) && end - i < 4) {
      int fd = atoi(text + i);
      i = end;
      if (parse_redirect(&state, text, &i, fd, error, error_size) != 0) {
        goto fail;
      }
      continue;
    }

    char *word = strndup(text + i, end - i);
    arglist_append(&state.command.words, word);
    free(word);
    i = end;
  }

  command_free(&state.command);
  return 0;

fail:
  command_free(&state.command);
  for (int j = 0;j < state.current.count;j++) {
    command_free(&state.current.commands[j]);
  }
  free(state.current.commands);
  syntax_free(list);
  return -1;
}

void syntax_free(CommandList *list) {
  for (int i = 0;i < list->count;i++) {
    Pipeline *pipeline = &list->pipelines[i];
    for (int j = 0;j < pipeline->count;j++) {
      command_free(&pipeline->commands[j]);
    }
    free(pipeline->commands);
    free(pipeline->text);
  }
  free(list->pipelines);
  free(list->connectors);
  list->pipelines = NULL;
  list->connectors = NULL;
  list->count = 0;
}

// ---------------------------------------------------------------------------
// Is more input needed?
// ---------------------------------------------------------------------------

static int word_is(const char *word, size_t length, const char *const *set) {
  for (int i = 0;set[i] != NULL;i++) {
    if (strlen(set[i]) == length && strncmp(word, set[i], length) == 0) {
      return 1;
    }
  }
  return 0;
}

int syntax_incomplete(const char *text, int fish) {
  static const char *const openers[] = { "if", "for", "while", "until", "case", "select", "{", NULL };
  static const char *const fish_openers[] = { "function", "begin", "switch", NULL };
  static const char *const closers[] = { "fi", "done", "esac", "}", NULL };
  static const char *const fish_closers[] = { "end", NULL };
  // After these words the next word is a command again.
  static const char *const leads_command[] = {
    "if", "while", "until", "do", "then", "else", "elif", "!", "{", "time", "begin", "and", "or", "not", NULL
  };

  // An odd number of trailing backslashes escapes the newline.
  size_t length = strlen(text);
  size_t slashes = 0;
  while (slashes < length && text[length - 1 - slashes] == '\\') {
    slashes++;
  }
  if (slashes % 2 == 1) {
    return 1;
  }

  int depth = 0;
  int command_start = 1;
  int pending_operator = 0;
  size_t i = 0;

  while (text[i] != '\0') {
    char c = text[i];
    if (is_blank(c)) {
      i++;
      continue;
    }
    if (c == '\n') {
      command_start = 1;
      i++;
      continue;
    }
    if (c == '#') {
      while (text[i] != '\0' && text[i] != '\n') i++;
      continue;
    }
    if ((c == '|' && text[i + 1] == '|') || (c == '&' && text[i + 1] == '&')) {
      pending_operator = 1;
      command_start = 1;
      i += 2;
      continue;
    }
    if (c == '|') {
      pending_operator = 1;
      command_start = 1;
      i++;
      continue;
    }
    if (c == ';' || (c == '&' && text[i + 1] != '>')) {
      pending_operator = 0;
      command_start = 1;
      i += (text[i + 1] == ';') ? 2 : 1;
      continue;
    }
    if (c == '<' || c == '>' || c == '&') {
      pending_operator = 0;
      while (text[i] == '<' || text[i] == '>' || text[i] == '&') i++;
      continue;
    }

    int unterminated = 0;
    size_t end = syntax_word_end(text, i, &unterminated);
    if (unterminated) {
      return 1;
    }
    const char *word = text + i;
    size_t word_length = end - i;
    pending_operator = 0;

    if (command_start) {
      if (word_is(word, word_length, openers) || (fish && word_is(word, word_length, fish_openers))) {
        depth++;
      }
      else if (word_is(word, word_length, closers) || (fish && word_is(word, word_length, fish_closers))) {
        depth--;
      }
      command_start = word_is(word, word_length, leads_command);
    }
    // `name()` is followed by the function body.
    if (word_length > 2 && strncmp(word + word_length - 2, "()", 2) == 0) {
      command_start = 1;
    }
    i = end;
  }

  return depth > 0 || pending_operator;
}
