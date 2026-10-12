#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <signal.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <sys/stat.h>

#include "script.h"
#include "exec.h"
#include "expand.h"
#include "jobs.h"
#include "syntax.h"
#include "lang.h"
#include "util.h"

// ===========================================================================
// Values
// ===========================================================================

typedef enum { T_NONE, T_BOOL, T_INT, T_FLOAT, T_STR, T_LIST, T_DICT, T_FUNC }Type;

typedef struct Value Value;
typedef struct Expr Expr;
typedef struct Block Block;

typedef struct {
  int refs;
  size_t length;
  char text[];
}Str;

typedef struct {
  int refs;
  int count, capacity;
  Value *items;
}List;

typedef struct {
  int refs;
  int count, capacity;
  Value *keys;
  Value *values;
}Dict;

typedef struct {
  int refs;
  char *name;
  int param_count;
  char **params;
  Expr **defaults;   // owned by a kept program, may be NULL per param
  Block *body;       // owned by a kept program
  char *source;      // the def's text, for funcsave
}Func;

struct Value {
  Type type;
  union {
    int boolean;
    long long integer;
    double number;
    Str *str;
    List *list;
    Dict *dict;
    Func *func;
  }as;
};

static Value v_none(void) { Value v; v.type = T_NONE; v.as.integer = 0; return v; }
static Value v_bool(int b) { Value v; v.type = T_BOOL; v.as.boolean = b != 0; return v; }
static Value v_int(long long i) { Value v; v.type = T_INT; v.as.integer = i; return v; }
static Value v_float(double d) { Value v; v.type = T_FLOAT; v.as.number = d; return v; }

static void *checked(void *pointer) {
  if (pointer == NULL) {
    perror("tarsh: out of memory");
    exit(EXIT_FAILURE);
  }
  return pointer;
}

static Value v_strn(const char *text, size_t length) {
  Str *s = checked(malloc(sizeof(Str) + length + 1));
  s->refs = 1;
  s->length = length;
  memcpy(s->text, text, length);
  s->text[length] = '\0';
  Value v;
  v.type = T_STR;
  v.as.str = s;
  return v;
}

static Value v_str(const char *text) { return v_strn(text, strlen(text)); }

// Takes ownership of a malloc'd string.
static Value v_str_take(char *text) {
  Value v = v_str(text);
  free(text);
  return v;
}

static Value v_list(void) {
  List *l = checked(calloc(1, sizeof(List)));
  l->refs = 1;
  Value v;
  v.type = T_LIST;
  v.as.list = l;
  return v;
}

static Value v_dict(void) {
  Dict *d = checked(calloc(1, sizeof(Dict)));
  d->refs = 1;
  Value v;
  v.type = T_DICT;
  v.as.dict = d;
  return v;
}

static Value incref(Value v) {
  switch (v.type) {
    case T_STR: v.as.str->refs++; break;
    case T_LIST: v.as.list->refs++; break;
    case T_DICT: v.as.dict->refs++; break;
    case T_FUNC: v.as.func->refs++; break;
    default: break;
  }
  return v;
}

static void decref(Value v) {
  switch (v.type) {
    case T_STR:
      if (--v.as.str->refs == 0) free(v.as.str);
      break;
    case T_LIST:
      if (--v.as.list->refs == 0) {
        for (int i = 0;i < v.as.list->count;i++) decref(v.as.list->items[i]);
        free(v.as.list->items);
        free(v.as.list);
      }
      break;
    case T_DICT:
      if (--v.as.dict->refs == 0) {
        for (int i = 0;i < v.as.dict->count;i++) {
          decref(v.as.dict->keys[i]);
          decref(v.as.dict->values[i]);
        }
        free(v.as.dict->keys);
        free(v.as.dict->values);
        free(v.as.dict);
      }
      break;
    case T_FUNC:
      if (--v.as.func->refs == 0) {
        Func *f = v.as.func;
        free(f->name);
        for (int i = 0;i < f->param_count;i++) free(f->params[i]);
        free(f->params);
        free(f->defaults);
        free(f->source);
        free(f);
      }
      break;
    default:
      break;
  }
}

// Appends (taking ownership of) `item`.
static void list_push(List *l, Value item) {
  if (l->count == l->capacity) {
    l->capacity = l->capacity ? l->capacity * 2 : 8;
    l->items = checked(realloc(l->items, l->capacity * sizeof(Value)));
  }
  l->items[l->count++] = item;
}

// ===========================================================================
// String building
// ===========================================================================

typedef struct {
  char *text;
  size_t length, capacity;
}SB;

static void sb_addn(SB *sb, const char *text, size_t length) {
  if (sb->length + length + 1 > sb->capacity) {
    size_t capacity = sb->capacity ? sb->capacity : 64;
    while (sb->length + length + 1 > capacity) capacity *= 2;
    sb->text = checked(realloc(sb->text, capacity));
    sb->capacity = capacity;
  }
  memcpy(sb->text + sb->length, text, length);
  sb->length += length;
  sb->text[sb->length] = '\0';
}
static void sb_add(SB *sb, const char *text) { sb_addn(sb, text, strlen(text)); }
static void sb_addc(SB *sb, char c) { sb_addn(sb, &c, 1); }
static char *sb_take(SB *sb) { return sb->text ? sb->text : strdup(""); }

// ===========================================================================
// Errors
// ===========================================================================

static jmp_buf *error_jump = NULL;
static const char *current_origin = "script";
static int line_offset = 0;
static volatile sig_atomic_t script_interrupted = 0;
static int last_run_failed = 0;

static void fail(int line, const char *format, ...) {
  char message[512];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  fflush(stdout);
  if (line > 0) {
    fprintf(stderr, "tarsh: %s, line %d: %s\n", current_origin, line + line_offset, message);
  }
  else {
    fprintf(stderr, "tarsh: %s: %s\n", current_origin, message);
  }
  if (error_jump != NULL) {
    longjmp(*error_jump, 1);
  }
  exit(1);
}

static const char *type_name(Value v);

// "an int", "a string": for messages.
static const char *a_type(Value v) {
  switch (v.type) {
    case T_INT: return "an int";
    case T_NONE: return "none";
    default: {
      static char buffer[32];
      snprintf(buffer, sizeof(buffer), "a %s", type_name(v));
      return buffer;
    }
  }
}

static const char *type_name(Value v) {
  switch (v.type) {
    case T_NONE: return "none";
    case T_BOOL: return "bool";
    case T_INT: return "int";
    case T_FLOAT: return "float";
    case T_STR: return "string";
    case T_LIST: return "list";
    case T_DICT: return "dict";
    case T_FUNC: return "function";
  }
  return "?";
}

// ===========================================================================
// Conversions
// ===========================================================================

static void format_float(SB *sb, double d) {
  char buffer[64];
  if (isnan(d)) { sb_add(sb, "nan"); return; }
  if (isinf(d)) { sb_add(sb, d > 0 ? "inf" : "-inf"); return; }
  snprintf(buffer, sizeof(buffer), "%.15g", d);
  sb_add(sb, buffer);
  if (strpbrk(buffer, ".e") == NULL) sb_add(sb, ".0");
}

static void append_value(SB *sb, Value v, int quote_strings) {
  char buffer[64];
  switch (v.type) {
    case T_NONE: sb_add(sb, quote_strings ? "none" : ""); break;
    case T_BOOL: sb_add(sb, v.as.boolean ? "true" : "false"); break;
    case T_INT: snprintf(buffer, sizeof(buffer), "%lld", v.as.integer); sb_add(sb, buffer); break;
    case T_FLOAT: format_float(sb, v.as.number); break;
    case T_STR:
      if (!quote_strings) {
        sb_addn(sb, v.as.str->text, v.as.str->length);
      }
      else {
        sb_addc(sb, '"');
        for (size_t i = 0;i < v.as.str->length;i++) {
          char c = v.as.str->text[i];
          if (c == '"' || c == '\\') { sb_addc(sb, '\\'); sb_addc(sb, c); }
          else if (c == '\n') sb_add(sb, "\\n");
          else if (c == '\t') sb_add(sb, "\\t");
          else sb_addc(sb, c);
        }
        sb_addc(sb, '"');
      }
      break;
    case T_LIST:
      sb_addc(sb, '[');
      for (int i = 0;i < v.as.list->count;i++) {
        if (i) sb_add(sb, ", ");
        append_value(sb, v.as.list->items[i], 1);
      }
      sb_addc(sb, ']');
      break;
    case T_DICT:
      sb_addc(sb, '{');
      for (int i = 0;i < v.as.dict->count;i++) {
        if (i) sb_add(sb, ", ");
        append_value(sb, v.as.dict->keys[i], 1);
        sb_add(sb, ": ");
        append_value(sb, v.as.dict->values[i], 1);
      }
      sb_addc(sb, '}');
      break;
    case T_FUNC:
      sb_add(sb, "<function ");
      sb_add(sb, v.as.func->name);
      sb_addc(sb, '>');
      break;
  }
}

// str(v): what the value looks like as text.
static char *to_text(Value v) {
  SB sb = { 0 };
  append_value(&sb, v, 0);
  return sb_take(&sb);
}

static char *to_repr(Value v) {
  SB sb = { 0 };
  append_value(&sb, v, 1);
  return sb_take(&sb);
}

static int truthy(Value v) {
  switch (v.type) {
    case T_NONE: return 0;
    case T_BOOL: return v.as.boolean;
    case T_INT: return v.as.integer != 0;
    case T_FLOAT: return v.as.number != 0.0;
    case T_STR: return v.as.str->length > 0;
    case T_LIST: return v.as.list->count > 0;
    case T_DICT: return v.as.dict->count > 0;
    case T_FUNC: return 1;
  }
  return 0;
}

// Shell values are text; a string that looks like a number can be used
// as one ("5" + 1 == 6).
static int numeric_string(Value v, double *out, int *is_int, long long *integer) {
  if (v.type != T_STR || v.as.str->length == 0) return 0;
  const char *text = v.as.str->text;
  char *end;
  errno = 0;
  long long i = strtoll(text, &end, 10);
  if (*end == '\0' && errno == 0) {
    *is_int = 1; *integer = i; *out = (double)i;
    return 1;
  }
  double d = strtod(text, &end);
  if (*end == '\0') {
    *is_int = 0; *out = d;
    return 1;
  }
  return 0;
}

// Reads v as a number. Returns 0 if it isn't one.
static int as_number(Value v, double *number, int *is_int, long long *integer) {
  switch (v.type) {
    case T_BOOL: *is_int = 1; *integer = v.as.boolean; *number = v.as.boolean; return 1;
    case T_INT: *is_int = 1; *integer = v.as.integer; *number = (double)v.as.integer; return 1;
    case T_FLOAT: *is_int = 0; *number = v.as.number; return 1;
    case T_STR: return numeric_string(v, number, is_int, integer);
    default: return 0;
  }
}

// ===========================================================================
// Tokens
// ===========================================================================

typedef enum {
  TK_END, TK_NUMBER, TK_STRING, TK_ISTRING, TK_FSTRING, TK_NAME, TK_OP,
  TK_CAPTURE, TK_SUCCESS, TK_VAR
}TokenKind;

typedef struct {
  TokenKind kind;
  char *text;
  int is_float;
  long long integer;
  double number;
}Token;

typedef struct {
  Token *items;
  int count, capacity;
  int position;
  int line;
}Tokens;

static void token_push(Tokens *tokens, Token token) {
  if (tokens->count == tokens->capacity) {
    tokens->capacity = tokens->capacity ? tokens->capacity * 2 : 16;
    tokens->items = checked(realloc(tokens->items, tokens->capacity * sizeof(Token)));
  }
  tokens->items[tokens->count++] = token;
}

static void tokens_free(Tokens *tokens) {
  for (int i = 0;i < tokens->count;i++) free(tokens->items[i].text);
  free(tokens->items);
  tokens->items = NULL;
  tokens->count = tokens->capacity = 0;
}

static int is_name_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_name_char(char c) { return is_name_start(c) || (c >= '0' && c <= '9'); }

static void tokenize(const char *s, Tokens *out, int line) {
  out->line = line;
  out->position = 0;
  size_t i = 0;
  while (s[i] != '\0') {
    char c = s[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
    if (c == '#') break;

    Token token;
    memset(&token, 0, sizeof(token));

    if ((c >= '0' && c <= '9') || (c == '.' && s[i + 1] >= '0' && s[i + 1] <= '9')) {
      char *end;
      if (c == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        token.integer = strtoll(s + i, &end, 16);
        token.kind = TK_NUMBER;
      }
      else {
        size_t j = i;
        int is_float = 0;
        while ((s[j] >= '0' && s[j] <= '9') || s[j] == '_') j++;
        if (s[j] == '.' && s[j + 1] != '.') { is_float = 1; j++; while (s[j] >= '0' && s[j] <= '9') j++; }
        if (s[j] == 'e' || s[j] == 'E') { is_float = 1; j++; if (s[j] == '+' || s[j] == '-') j++; while (s[j] >= '0' && s[j] <= '9') j++; }
        char *digits = strndup(s + i, j - i);
        size_t used = 0;
        for (char *d = digits;*d;d++) if (*d != '_') digits[used++] = *d;
        digits[used] = '\0';
        token.kind = TK_NUMBER;
        token.is_float = is_float;
        if (is_float) token.number = strtod(digits, NULL);
        else token.integer = strtoll(digits, NULL, 10);
        free(digits);
        end = (char *)s + j;
      }
      i = end - s;
      token_push(out, token);
      continue;
    }

    if ((c == 'f' || c == 'F') && (s[i + 1] == '"' || s[i + 1] == '\'')) {
      token.kind = TK_FSTRING;
      i++;
      c = s[i];
    }
    if (c == '"' || c == '\'') {
      char quote = c;
      if (token.kind != TK_FSTRING) token.kind = (quote == '"') ? TK_ISTRING : TK_STRING;
      SB sb = { 0 };
      i++;
      while (s[i] != '\0' && s[i] != quote) {
        if (s[i] == '\\' && s[i + 1] != '\0') {
          char next = s[i + 1];
          if (token.kind == TK_ISTRING) {
            // Kept raw; escapes are handled with $-interpolation later.
            sb_addc(&sb, '\\');
            sb_addc(&sb, next);
          }
          else {
            switch (next) {
              case 'n': sb_addc(&sb, '\n'); break;
              case 't': sb_addc(&sb, '\t'); break;
              case 'r': sb_addc(&sb, '\r'); break;
              case 'e': sb_addc(&sb, '\033'); break;
              case '0': sb_addc(&sb, '\0'); break;
              default: sb_addc(&sb, next); break;
            }
          }
          i += 2;
          continue;
        }
        sb_addc(&sb, s[i]);
        i++;
      }
      if (s[i] != quote) {
        free(sb.text);
        fail(line, "a string was opened with %c but never closed", quote);
      }
      i++;
      token.text = sb_take(&sb);
      token_push(out, token);
      continue;
    }

    if ((c == '$' || c == '?') && s[i + 1] == '(') {
      int unterminated = 0;
      size_t end = syntax_subst_end(s, i, &unterminated);
      if (unterminated) fail(line, "%c( was opened but never closed", c);
      token.kind = (c == '$') ? TK_CAPTURE : TK_SUCCESS;
      token.text = strndup(s + i + 2, end - i - 3);
      i = end;
      token_push(out, token);
      continue;
    }

    if (c == '$' && (s[i + 1] >= '0' && s[i + 1] <= '9')) {
      // $1 ... $9: the script's arguments.
      token.kind = TK_VAR;
      token.text = strndup(s + i + 1, 1);
      i += 2;
      token_push(out, token);
      continue;
    }
    if (c == '$' && (is_name_start(s[i + 1]) || s[i + 1] == '{')) {
      size_t start = i + 1;
      size_t end;
      if (s[start] == '{') {
        start++;
        end = start;
        while (is_name_char(s[end])) end++;
        if (s[end] != '}') fail(line, "${ needs a closing }");
        token.text = strndup(s + start, end - start);
        i = end + 1;
      }
      else {
        end = start;
        while (is_name_char(s[end])) end++;
        token.text = strndup(s + start, end - start);
        i = end;
      }
      token.kind = TK_VAR;
      token_push(out, token);
      continue;
    }

    if (is_name_start(c)) {
      size_t j = i;
      while (is_name_char(s[j])) j++;
      token.kind = TK_NAME;
      token.text = strndup(s + i, j - i);
      i = j;
      token_push(out, token);
      continue;
    }

    static const char *operators[] = {
      "**", "//", "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=",
      "(", ")", "[", "]", "{", "}", ",", ":", ".", "+", "-", "*", "/", "%", "<", ">", "=", NULL
    };
    int matched = 0;
    for (int k = 0;operators[k] != NULL;k++) {
      size_t length = strlen(operators[k]);
      if (strncmp(s + i, operators[k], length) == 0) {
        token.kind = TK_OP;
        token.text = strdup(operators[k]);
        i += length;
        matched = 1;
        break;
      }
    }
    if (!matched) {
      fail(line, "didn't expect '%c' here", c);
    }
    token_push(out, token);
  }
  Token end;
  memset(&end, 0, sizeof(end));
  end.kind = TK_END;
  end.text = strdup("");
  token_push(out, end);
}

// ===========================================================================
// Syntax tree
// ===========================================================================

typedef enum {
  EX_CONST, EX_ISTRING, EX_FSTRING, EX_NAME, EX_LIST, EX_DICT, EX_INDEX, EX_SLICE,
  EX_CALL, EX_METHOD, EX_BINARY, EX_NEGATE, EX_NOT, EX_AND, EX_OR, EX_COMPARE,
  EX_CAPTURE, EX_SUCCESS, EX_TERNARY
}ExprKind;

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_FLOORDIV, OP_MOD, OP_POW };
enum { CMP_EQ, CMP_NE, CMP_LT, CMP_LE, CMP_GT, CMP_GE, CMP_IN, CMP_NOT_IN };

struct Expr {
  ExprKind kind;
  int line;
  Value constant;
  char *text;
  int op;
  Expr *left, *right, *third;
  Expr **items;
  int count;
  char **names;      // keyword argument names (NULL for positional)
  int *ops;          // comparison chain operators
};

typedef enum {
  S_EXPR, S_ASSIGN, S_AUGMENT, S_IF, S_WHILE, S_FOR, S_DEF, S_RETURN,
  S_BREAK, S_CONTINUE, S_PASS, S_GLOBAL, S_COMMAND
}StmtKind;

typedef struct Stmt Stmt;

struct Block {
  Stmt **items;
  int count;
};

struct Stmt {
  StmtKind kind;
  int line;
  Expr **targets;    // assignment / for targets
  int target_count;
  int op;            // augmented assignment operator
  Expr *value;       // value / condition / iterable
  Block *body;
  Block *orelse;
  char *name;        // def name
  char **params;     // def params, global names
  Expr **defaults;
  int param_count;
  char *text;        // command text, def source
};

static Expr *new_expr(ExprKind kind, int line) {
  Expr *e = checked(calloc(1, sizeof(Expr)));
  e->kind = kind;
  e->line = line;
  e->constant = v_none();
  return e;
}

static void free_expr(Expr *e) {
  if (e == NULL) return;
  decref(e->constant);
  free(e->text);
  free_expr(e->left);
  free_expr(e->right);
  free_expr(e->third);
  for (int i = 0;i < e->count;i++) {
    free_expr(e->items[i]);
    if (e->names) free(e->names[i]);
  }
  free(e->items);
  free(e->names);
  free(e->ops);
  free(e);
}

static void free_block(Block *b);

static void free_stmt(Stmt *s) {
  if (s == NULL) return;
  for (int i = 0;i < s->target_count;i++) free_expr(s->targets[i]);
  free(s->targets);
  free_expr(s->value);
  free_block(s->body);
  free_block(s->orelse);
  free(s->name);
  for (int i = 0;i < s->param_count;i++) {
    if (s->params) free(s->params[i]);
    if (s->defaults) free_expr(s->defaults[i]);
  }
  free(s->params);
  free(s->defaults);
  free(s->text);
  free(s);
}

static void free_block(Block *b) {
  if (b == NULL) return;
  for (int i = 0;i < b->count;i++) free_stmt(b->items[i]);
  free(b->items);
  free(b);
}

static void expr_push(Expr *e, Expr *item, char *name) {
  e->items = checked(realloc(e->items, (e->count + 1) * sizeof(Expr *)));
  e->names = checked(realloc(e->names, (e->count + 1) * sizeof(char *)));
  e->items[e->count] = item;
  e->names[e->count] = name;
  e->count++;
}

// ===========================================================================
// Expression parser
// ===========================================================================

static Token *peek(Tokens *t) { return &t->items[t->position]; }
static Token *advance(Tokens *t) { Token *token = &t->items[t->position]; if (token->kind != TK_END) t->position++; return token; }

static int is_op(Tokens *t, const char *op) {
  Token *token = peek(t);
  return token->kind == TK_OP && strcmp(token->text, op) == 0;
}
static int is_word(Tokens *t, const char *word) {
  Token *token = peek(t);
  return token->kind == TK_NAME && strcmp(token->text, word) == 0;
}
static void expect_op(Tokens *t, const char *op) {
  if (!is_op(t, op)) {
    Token *token = peek(t);
    fail(t->line, "expected '%s' but found %s%s%s", op,
         token->kind == TK_END ? "the end of the line" : "'",
         token->kind == TK_END ? "" : token->text,
         token->kind == TK_END ? "" : "'");
  }
  advance(t);
}

static Expr *parse_expression(Tokens *t);
static Expr *parse_fstring(const char *text, int line);

static Expr *parse_atom(Tokens *t) {
  Token *token = advance(t);
  Expr *e;
  switch (token->kind) {
    case TK_NUMBER:
      e = new_expr(EX_CONST, t->line);
      e->constant = token->is_float ? v_float(token->number) : v_int(token->integer);
      return e;
    case TK_STRING:
      e = new_expr(EX_CONST, t->line);
      e->constant = v_str(token->text);
      return e;
    case TK_ISTRING:
      // Only strings mentioning $ or \ need work at run time.
      if (strchr(token->text, '$') == NULL && strchr(token->text, '\\') == NULL) {
        e = new_expr(EX_CONST, t->line);
        e->constant = v_str(token->text);
        return e;
      }
      e = new_expr(EX_ISTRING, t->line);
      e->text = strdup(token->text);
      return e;
    case TK_FSTRING:
      return parse_fstring(token->text, t->line);
    case TK_CAPTURE:
      e = new_expr(EX_CAPTURE, t->line);
      e->text = strdup(token->text);
      return e;
    case TK_SUCCESS:
      e = new_expr(EX_SUCCESS, t->line);
      e->text = strdup(token->text);
      return e;
    case TK_VAR:
      e = new_expr(EX_NAME, t->line);
      e->text = strdup(token->text);
      return e;
    case TK_NAME:
      if (strcmp(token->text, "True") == 0 || strcmp(token->text, "true") == 0) {
        e = new_expr(EX_CONST, t->line); e->constant = v_bool(1); return e;
      }
      if (strcmp(token->text, "False") == 0 || strcmp(token->text, "false") == 0) {
        e = new_expr(EX_CONST, t->line); e->constant = v_bool(0); return e;
      }
      if (strcmp(token->text, "None") == 0 || strcmp(token->text, "none") == 0) {
        return new_expr(EX_CONST, t->line);
      }
      e = new_expr(EX_NAME, t->line);
      e->text = strdup(token->text);
      return e;
    case TK_OP:
      if (strcmp(token->text, "(") == 0) {
        e = parse_expression(t);
        expect_op(t, ")");
        return e;
      }
      if (strcmp(token->text, "[") == 0) {
        e = new_expr(EX_LIST, t->line);
        while (!is_op(t, "]")) {
          expr_push(e, parse_expression(t), NULL);
          if (!is_op(t, ",")) break;
          advance(t);
        }
        expect_op(t, "]");
        return e;
      }
      if (strcmp(token->text, "{") == 0) {
        e = new_expr(EX_DICT, t->line);
        while (!is_op(t, "}")) {
          expr_push(e, parse_expression(t), NULL);
          expect_op(t, ":");
          expr_push(e, parse_expression(t), NULL);
          if (!is_op(t, ",")) break;
          advance(t);
        }
        expect_op(t, "}");
        return e;
      }
      fail(t->line, "didn't expect '%s' here", token->text);
      break;
    case TK_END:
      fail(t->line, "the line ended where a value was expected");
      break;
  }
  return NULL;
}

static void parse_call_arguments(Tokens *t, Expr *call) {
  expect_op(t, "(");
  while (!is_op(t, ")")) {
    // name=value keyword argument?
    Token *token = peek(t);
    if (token->kind == TK_NAME && t->items[t->position + 1].kind == TK_OP &&
        strcmp(t->items[t->position + 1].text, "=") == 0) {
      char *name = strdup(token->text);
      advance(t);
      advance(t);
      expr_push(call, parse_expression(t), name);
    }
    else {
      expr_push(call, parse_expression(t), NULL);
    }
    if (!is_op(t, ",")) break;
    advance(t);
  }
  expect_op(t, ")");
}

static Expr *parse_postfix(Tokens *t) {
  Expr *e = parse_atom(t);
  while (1) {
    if (is_op(t, "(")) {
      if (e->kind != EX_NAME) fail(t->line, "only named functions can be called");
      Expr *call = new_expr(EX_CALL, t->line);
      call->text = e->text;
      e->text = NULL;
      free_expr(e);
      parse_call_arguments(t, call);
      e = call;
    }
    else if (is_op(t, "[")) {
      advance(t);
      Expr *start = NULL;
      if (!is_op(t, ":")) start = parse_expression(t);
      if (is_op(t, ":")) {
        advance(t);
        Expr *slice = new_expr(EX_SLICE, t->line);
        slice->left = e;
        slice->right = start;
        if (!is_op(t, "]")) slice->third = parse_expression(t);
        expect_op(t, "]");
        e = slice;
      }
      else {
        Expr *index = new_expr(EX_INDEX, t->line);
        index->left = e;
        index->right = start;
        expect_op(t, "]");
        e = index;
      }
    }
    else if (is_op(t, ".")) {
      advance(t);
      Token *name = advance(t);
      if (name->kind != TK_NAME) fail(t->line, "expected a method name after '.'");
      Expr *method = new_expr(EX_METHOD, t->line);
      method->text = strdup(name->text);
      method->left = e;
      if (is_op(t, "(")) parse_call_arguments(t, method);
      e = method;
    }
    else {
      return e;
    }
  }
}

static Expr *parse_unary(Tokens *t);

static Expr *parse_power(Tokens *t) {
  Expr *base = parse_postfix(t);
  if (is_op(t, "**")) {
    advance(t);
    Expr *e = new_expr(EX_BINARY, t->line);
    e->op = OP_POW;
    e->left = base;
    e->right = parse_unary(t);
    return e;
  }
  return base;
}

static Expr *parse_unary(Tokens *t) {
  if (is_op(t, "-")) {
    advance(t);
    Expr *e = new_expr(EX_NEGATE, t->line);
    e->left = parse_unary(t);
    return e;
  }
  if (is_op(t, "+")) {
    advance(t);
    return parse_unary(t);
  }
  return parse_power(t);
}

static Expr *parse_term(Tokens *t) {
  Expr *left = parse_unary(t);
  while (is_op(t, "*") || is_op(t, "/") || is_op(t, "//") || is_op(t, "%")) {
    Token *op = advance(t);
    Expr *e = new_expr(EX_BINARY, t->line);
    e->op = strcmp(op->text, "*") == 0 ? OP_MUL : strcmp(op->text, "/") == 0 ? OP_DIV :
            strcmp(op->text, "//") == 0 ? OP_FLOORDIV : OP_MOD;
    e->left = left;
    e->right = parse_unary(t);
    left = e;
  }
  return left;
}

static Expr *parse_sum(Tokens *t) {
  Expr *left = parse_term(t);
  while (is_op(t, "+") || is_op(t, "-")) {
    Token *op = advance(t);
    Expr *e = new_expr(EX_BINARY, t->line);
    e->op = strcmp(op->text, "+") == 0 ? OP_ADD : OP_SUB;
    e->left = left;
    e->right = parse_term(t);
    left = e;
  }
  return left;
}

static int comparison_op(Tokens *t) {
  if (is_op(t, "==")) return CMP_EQ;
  if (is_op(t, "!=")) return CMP_NE;
  if (is_op(t, "<")) return CMP_LT;
  if (is_op(t, "<=")) return CMP_LE;
  if (is_op(t, ">")) return CMP_GT;
  if (is_op(t, ">=")) return CMP_GE;
  if (is_word(t, "in")) return CMP_IN;
  if (is_word(t, "not") && t->items[t->position + 1].kind == TK_NAME &&
      strcmp(t->items[t->position + 1].text, "in") == 0) return CMP_NOT_IN;
  return -1;
}

static Expr *parse_comparison(Tokens *t) {
  Expr *first = parse_sum(t);
  int op = comparison_op(t);
  if (op < 0) return first;
  Expr *e = new_expr(EX_COMPARE, t->line);
  expr_push(e, first, NULL);
  while ((op = comparison_op(t)) >= 0) {
    advance(t);
    if (op == CMP_NOT_IN) advance(t);
    e->ops = checked(realloc(e->ops, e->count * sizeof(int)));
    e->ops[e->count - 1] = op;
    expr_push(e, parse_sum(t), NULL);
  }
  return e;
}

static Expr *parse_not(Tokens *t) {
  if (is_word(t, "not")) {
    advance(t);
    Expr *e = new_expr(EX_NOT, t->line);
    e->left = parse_not(t);
    return e;
  }
  return parse_comparison(t);
}

static Expr *parse_and(Tokens *t) {
  Expr *left = parse_not(t);
  while (is_word(t, "and")) {
    advance(t);
    Expr *e = new_expr(EX_AND, t->line);
    e->left = left;
    e->right = parse_not(t);
    left = e;
  }
  return left;
}

static Expr *parse_or(Tokens *t) {
  Expr *left = parse_and(t);
  while (is_word(t, "or")) {
    advance(t);
    Expr *e = new_expr(EX_OR, t->line);
    e->left = left;
    e->right = parse_and(t);
    left = e;
  }
  return left;
}

// value if condition else other
static Expr *parse_expression(Tokens *t) {
  Expr *value = parse_or(t);
  if (is_word(t, "if")) {
    advance(t);
    Expr *e = new_expr(EX_TERNARY, t->line);
    e->left = value;
    e->right = parse_or(t);
    if (!is_word(t, "else")) fail(t->line, "expected 'else' in 'x if condition else y'");
    advance(t);
    e->third = parse_expression(t);
    return e;
  }
  return value;
}

// Parses a whole string as one expression.
static Expr *parse_expression_text(const char *text, int line) {
  Tokens t = { 0 };
  tokenize(text, &t, line);
  Expr *e = parse_expression(&t);
  if (peek(&t)->kind != TK_END) {
    char *rest = strdup(peek(&t)->text);
    tokens_free(&t);
    free_expr(e);
    fail(line, "didn't expect '%s' after the expression", rest);
  }
  tokens_free(&t);
  return e;
}

// f"text {expr} more {other:.2f}" -> parts
static Expr *parse_fstring(const char *text, int line) {
  Expr *e = new_expr(EX_FSTRING, line);
  SB literal = { 0 };
  size_t i = 0;
  while (text[i] != '\0') {
    if (text[i] == '{' && text[i + 1] == '{') { sb_addc(&literal, '{'); i += 2; continue; }
    if (text[i] == '}' && text[i + 1] == '}') { sb_addc(&literal, '}'); i += 2; continue; }
    if (text[i] != '{') { sb_addc(&literal, text[i]); i++; continue; }

    if (literal.length > 0) {
      Expr *part = new_expr(EX_CONST, line);
      part->constant = v_str(literal.text);
      expr_push(e, part, NULL);
      literal.length = 0;
      literal.text[0] = '\0';
    }
    size_t start = ++i;
    int depth = 0;
    while (text[i] != '\0' && !(depth == 0 && (text[i] == '}' || text[i] == ':'))) {
      if (text[i] == '(' || text[i] == '[' || text[i] == '{') depth++;
      if (text[i] == ')' || text[i] == ']' || text[i] == '}') depth--;
      i++;
    }
    char *inside = strndup(text + start, i - start);
    char *spec = NULL;
    if (text[i] == ':') {
      size_t spec_start = ++i;
      while (text[i] != '\0' && text[i] != '}') i++;
      spec = strndup(text + spec_start, i - spec_start);
    }
    if (text[i] != '}') {
      free(inside); free(spec); free(literal.text);
      free_expr(e);
      fail(line, "f-string has a { without a matching }");
    }
    i++;
    Expr *part = parse_expression_text(inside, line);
    free(inside);
    expr_push(e, part, spec);
  }
  if (literal.length > 0) {
    Expr *part = new_expr(EX_CONST, line);
    part->constant = v_str(literal.text);
    expr_push(e, part, NULL);
  }
  free(literal.text);
  return e;
}

// ===========================================================================
// Lines and statements
// ===========================================================================

typedef struct {
  int indent;
  int line;
  char *text;     // without indentation
  int is_script;  // statement (1) or command (0)
}Line;

static const char *block_keywords[] = { "if", "elif", "else", "for", "while", "def", NULL };
static const char *simple_keywords[] = { "return", "break", "continue", "pass", "global", NULL };

static int word_in(const char *word, size_t length, const char **set) {
  for (int i = 0;set[i] != NULL;i++) {
    if (strlen(set[i]) == length && strncmp(set[i], word, length) == 0) return 1;
  }
  return 0;
}

int script_is_keyword(const char *word) {
  return word_in(word, strlen(word), block_keywords) || word_in(word, strlen(word), simple_keywords) ||
         strcmp(word, "in") == 0 || strcmp(word, "and") == 0 || strcmp(word, "or") == 0 ||
         strcmp(word, "not") == 0;
}

// Last character that isn't a space or part of a # comment.
static char last_code_char(const char *text) {
  char quote = '\0';
  char last = '\0';
  for (size_t i = 0;text[i];i++) {
    char c = text[i];
    if (quote) {
      if (c == '\\' && text[i + 1]) { i++; continue; }
      if (c == quote) quote = '\0';
      last = c;
      continue;
    }
    if (c == '"' || c == '\'') { quote = c; last = c; continue; }
    if (c == '#' && (i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t')) break;
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') last = c;
  }
  return last;
}

static int builtin_function_index_n(const char *name, size_t length);
static int lookup_global_func(const char *name, size_t length);
static int global_exists(const char *name, size_t length);

// What kind of line is this? 0 = shell command, 1 = block header,
// 2 = other script statement.
static int classify(const char *text) {
  while (*text == ' ' || *text == '\t') text++;
  size_t word = 0;
  while (is_name_char(text[word])) word++;

  if (word > 0 && word_in(text, word, block_keywords)) {
    char next = text[word];
    if ((next == ' ' || next == ':' || next == '(' || next == '\t') && last_code_char(text) == ':') {
      return 1;
    }
    return 0;
  }
  if (word > 0 && word_in(text, word, simple_keywords)) {
    // `pass` (the password manager) and `global` (GNU global) are also
    // programs: only their script forms count as statements.
    const char *rest = text + word;
    while (*rest == ' ' || *rest == '\t') rest++;
    int alone = (*rest == '\0' || *rest == '#');
    if (strncmp(text, "return", word) == 0 && word == 6 && (alone || text[word] == ' ')) return 2;
    if ((word == 4 || word == 5 || word == 8) && alone) return 2;   // pass, break, continue
    if (word == 6 && strncmp(text, "global", 6) == 0 && is_name_start(*rest) && strchr(rest, '-') == NULL) return 2;
  }

  // Assignment: name = ..., name[i] = ..., a, b = ..., x += ...
  if (word > 0 && is_name_start(text[0])) {
    size_t i = word;
    int depth = 0;
    int shaped = 1;
    while (text[i]) {
      char c = text[i];
      if (c == '[') depth++;
      else if (c == ']') depth--;
      else if (depth == 0 && (c == ' ' || c == '\t')) { i++; continue; }
      else if (depth == 0 && c == ',') { i++; while (text[i] == ' ') i++; if (!is_name_start(text[i])) { shaped = 0; break; } while (is_name_char(text[i])) i++; continue; }
      else if (depth == 0) break;
      i++;
    }
    if (shaped && depth == 0) {
      int spaced = (i > 0 && (text[i - 1] == ' ' || text[i - 1] == '\t'));
      if (text[i] == '=' && text[i + 1] != '=') {
        int spaced_after = text[i + 1] == ' ' || text[i + 1] == '\t';
        // NAME=value (no spaces) stays a shell assignment.
        if (spaced || spaced_after || text[word] == '[' || text[word] == ',') return 2;
      }
      if ((text[i] == '+' || text[i] == '-' || text[i] == '*' || text[i] == '/') && text[i + 1] == '=') {
        return 2;
      }
    }
  }

  // A call to a script or builtin function: print(...), name.method(...)
  if (word > 0 && text[word] == '(') {
    if (builtin_function_index_n(text, word) >= 0 || lookup_global_func(text, word)) return 2;
  }
  if (word > 0 && text[word] == '.' && is_name_start(text[word + 1]) && global_exists(text, word)) {
    return 2;
  }
  // Values typed at the prompt: 2 + 3, "text", [1, 2]
  if ((text[0] >= '0' && text[0] <= '9') || text[0] == '"' || text[0] == '\'' ||
      (text[0] == '[' && text[1] != ' ' && text[1] != '\0' && text[1] != '[')) {
    return 2;
  }
  return 0;
}

int script_claims(const char *text) {
  // Only the first non-blank line decides.
  while (*text == ' ' || *text == '\t' || *text == '\n') text++;
  size_t length = strcspn(text, "\n");
  char *first = strndup(text, length);
  int kind = classify(first);
  free(first);
  return kind != 0;
}

int script_opens_block(const char *line) {
  return classify(line) == 1;
}

// Splits source into logical lines: joins bracketed expressions and
// unfinished shell commands, records indentation.
static Line *split_lines(const char *source, int *count_out) {
  Line *lines = NULL;
  int count = 0, capacity = 0;
  char *copy = strdup(source);
  char *cursor = copy;
  int line_number = 0;
  int fish = strcmp(lang_current_name(), "fish") == 0;

  while (cursor && *cursor) {
    char *raw = cursor;
    char *newline = strchr(cursor, '\n');
    if (newline) { *newline = '\0'; cursor = newline + 1; }
    else cursor = raw + strlen(raw);
    line_number++;

    int indent = 0;
    char *text = raw;
    while (*text == ' ' || *text == '\t') {
      indent += (*text == '\t') ? 4 - (indent % 4) : 1;
      text++;
    }
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\r' || text[length - 1] == ' ' || text[length - 1] == '\t')) {
      text[--length] = '\0';
    }
    if (text[0] == '\0' || text[0] == '#') continue;

    int start_line = line_number;
    int kind = classify(text);
    SB joined = { 0 };
    sb_add(&joined, text);

    if (kind != 0) {
      // Keep reading while brackets are open.
      while (1) {
        int depth = 0;
        char quote = '\0';
        for (size_t i = 0;i < joined.length;i++) {
          char c = joined.text[i];
          if (quote) { if (c == '\\') { i++; continue; } if (c == quote) quote = '\0'; continue; }
          if (c == '"' || c == '\'') quote = c;
          else if (c == '#') break;
          else if (c == '(' || c == '[' || c == '{') depth++;
          else if (c == ')' || c == ']' || c == '}') depth--;
        }
        if (depth <= 0 || !*cursor) break;
        char *next = cursor;
        char *next_newline = strchr(cursor, '\n');
        if (next_newline) { *next_newline = '\0'; cursor = next_newline + 1; }
        else cursor = next + strlen(next);
        line_number++;
        sb_addc(&joined, ' ');
        while (*next == ' ' || *next == '\t') next++;
        sb_add(&joined, next);
      }
    }
    else {
      // A shell command continues while the shell says it's unfinished.
      while (*cursor && syntax_incomplete(joined.text, fish)) {
        char *next = cursor;
        char *next_newline = strchr(cursor, '\n');
        if (next_newline) { *next_newline = '\0'; cursor = next_newline + 1; }
        else cursor = next + strlen(next);
        line_number++;
        if (joined.length > 0 && joined.text[joined.length - 1] == '\\') {
          joined.text[--joined.length] = '\0';
        }
        else {
          sb_addc(&joined, '\n');
        }
        sb_add(&joined, next);
      }
    }

    if (count == capacity) {
      capacity = capacity ? capacity * 2 : 32;
      lines = checked(realloc(lines, capacity * sizeof(Line)));
    }
    lines[count].indent = indent;
    lines[count].line = start_line;
    lines[count].text = sb_take(&joined);
    lines[count].is_script = kind != 0;
    count++;
  }
  free(copy);
  *count_out = count;
  return lines;
}

static Stmt *new_stmt(StmtKind kind, int line) {
  Stmt *s = checked(calloc(1, sizeof(Stmt)));
  s->kind = kind;
  s->line = line;
  return s;
}

static void block_push(Block *b, Stmt *s) {
  b->items = checked(realloc(b->items, (b->count + 1) * sizeof(Stmt *)));
  b->items[b->count++] = s;
}

static Block *parse_block(Line *lines, int *index, int count, int indent);

// The text between a block keyword and its trailing ':'.
static char *header_body(const char *text, size_t keyword_length) {
  const char *start = text + keyword_length;
  while (*start == ' ' || *start == '\t') start++;
  // Drop a trailing comment, then the colon.
  char *copy = strdup(start);
  char quote = '\0';
  for (size_t i = 0;copy[i];i++) {
    if (quote) { if (copy[i] == '\\' && copy[i + 1]) { i++; continue; } if (copy[i] == quote) quote = '\0'; continue; }
    if (copy[i] == '"' || copy[i] == '\'') quote = copy[i];
    else if (copy[i] == '#' && (i == 0 || copy[i - 1] == ' ')) { copy[i] = '\0'; break; }
  }
  size_t length = strlen(copy);
  while (length > 0 && (copy[length - 1] == ' ' || copy[length - 1] == '\t')) copy[--length] = '\0';
  if (length > 0 && copy[length - 1] == ':') copy[--length] = '\0';
  while (length > 0 && (copy[length - 1] == ' ' || copy[length - 1] == '\t')) copy[--length] = '\0';
  return copy;
}

static Block *parse_body(Line *lines, int *index, int count, int indent, const char *what, int line) {
  if (*index >= count || lines[*index].indent <= indent) {
    fail(line, "'%s' needs an indented block under it", what);
  }
  return parse_block(lines, index, count, lines[*index].indent);
}

static char *capture_source(Line *lines, int from, int to, int base_indent) {
  SB sb = { 0 };
  for (int i = from;i < to;i++) {
    for (int k = base_indent;k < lines[i].indent;k++) sb_addc(&sb, ' ');
    sb_add(&sb, lines[i].text);
    sb_addc(&sb, '\n');
  }
  return sb_take(&sb);
}

static Stmt *parse_if(Line *lines, int *index, int count, int indent, size_t keyword_length) {
  Line *line = &lines[*index];
  char *condition = header_body(line->text, keyword_length);
  Stmt *s = new_stmt(S_IF, line->line);
  s->value = parse_expression_text(condition, line->line);
  free(condition);
  (*index)++;
  s->body = parse_body(lines, index, count, indent, keyword_length == 2 ? "if" : "elif", line->line);

  if (*index < count && lines[*index].indent == indent) {
    const char *text = lines[*index].text;
    if (strncmp(text, "elif", 4) == 0 && (text[4] == ' ' || text[4] == '(')) {
      Block *orelse = checked(calloc(1, sizeof(Block)));
      block_push(orelse, parse_if(lines, index, count, indent, 4));
      s->orelse = orelse;
    }
    else if (strncmp(text, "else", 4) == 0 && (text[4] == ':' || text[4] == ' ')) {
      int else_line = lines[*index].line;
      (*index)++;
      s->orelse = parse_body(lines, index, count, indent, "else", else_line);
    }
  }
  return s;
}

static Expr *parse_target(Tokens *t) {
  Expr *e = parse_postfix(t);
  if (e->kind != EX_NAME && e->kind != EX_INDEX) {
    free_expr(e);
    fail(t->line, "can only assign to a name or an item like x[0]");
  }
  return e;
}

static Stmt *parse_statement(Line *lines, int *index, int count, int indent) {
  Line *line = &lines[*index];
  const char *text = line->text;

  if (!line->is_script) {
    Stmt *s = new_stmt(S_COMMAND, line->line);
    s->text = strdup(text);
    (*index)++;
    return s;
  }

  size_t word = 0;
  while (is_name_char(text[word])) word++;

  if (word == 2 && strncmp(text, "if", 2) == 0) {
    return parse_if(lines, index, count, indent, 2);
  }
  if ((word == 4 && strncmp(text, "elif", 4) == 0) || (word == 4 && strncmp(text, "else", 4) == 0)) {
    fail(line->line, "'%.4s' without an 'if' before it", text);
  }
  if (word == 5 && strncmp(text, "while", 5) == 0) {
    char *condition = header_body(text, 5);
    Stmt *s = new_stmt(S_WHILE, line->line);
    s->value = parse_expression_text(condition, line->line);
    free(condition);
    (*index)++;
    s->body = parse_body(lines, index, count, indent, "while", line->line);
    return s;
  }
  if (word == 3 && strncmp(text, "for", 3) == 0) {
    char *header = header_body(text, 3);
    Stmt *s = new_stmt(S_FOR, line->line);
    Tokens t = { 0 };
    tokenize(header, &t, line->line);
    free(header);
    do {
      Token *name = advance(&t);
      if (name->kind != TK_NAME && name->kind != TK_VAR) {
        tokens_free(&t);
        free_stmt(s);
        fail(line->line, "expected a variable name after 'for'");
      }
      Expr *target = new_expr(EX_NAME, line->line);
      target->text = strdup(name->text);
      s->targets = checked(realloc(s->targets, (s->target_count + 1) * sizeof(Expr *)));
      s->targets[s->target_count++] = target;
    } while (is_op(&t, ",") && advance(&t));
    if (!is_word(&t, "in")) {
      tokens_free(&t);
      free_stmt(s);
      fail(line->line, "expected 'in' in 'for x in ...:'");
    }
    advance(&t);
    s->value = parse_expression(&t);
    tokens_free(&t);
    (*index)++;
    s->body = parse_body(lines, index, count, indent, "for", line->line);
    return s;
  }
  if (word == 3 && strncmp(text, "def", 3) == 0) {
    int first = *index;
    char *header = header_body(text, 3);
    Stmt *s = new_stmt(S_DEF, line->line);
    Tokens t = { 0 };
    tokenize(header, &t, line->line);
    free(header);
    Token *name = advance(&t);
    if (name->kind != TK_NAME) {
      tokens_free(&t);
      free_stmt(s);
      fail(line->line, "expected a function name after 'def'");
    }
    s->name = strdup(name->text);
    if (is_op(&t, "(")) {
      advance(&t);
      while (!is_op(&t, ")")) {
        Token *param = advance(&t);
        if (param->kind != TK_NAME) {
          tokens_free(&t);
          free_stmt(s);
          fail(line->line, "expected a parameter name");
        }
        s->params = checked(realloc(s->params, (s->param_count + 1) * sizeof(char *)));
        s->defaults = checked(realloc(s->defaults, (s->param_count + 1) * sizeof(Expr *)));
        s->params[s->param_count] = strdup(param->text);
        s->defaults[s->param_count] = NULL;
        if (is_op(&t, "=")) {
          advance(&t);
          s->defaults[s->param_count] = parse_expression(&t);
        }
        s->param_count++;
        if (!is_op(&t, ",")) break;
        advance(&t);
      }
      expect_op(&t, ")");
    }
    tokens_free(&t);
    (*index)++;
    s->body = parse_body(lines, index, count, indent, "def", line->line);
    s->text = capture_source(lines, first, *index, indent);
    return s;
  }

  (*index)++;
  if (word == 6 && strncmp(text, "return", 6) == 0) {
    Stmt *s = new_stmt(S_RETURN, line->line);
    const char *rest = text + 6;
    while (*rest == ' ') rest++;
    if (*rest && *rest != '#') s->value = parse_expression_text(rest, line->line);
    return s;
  }
  if (word == 5 && strncmp(text, "break", 5) == 0) return new_stmt(S_BREAK, line->line);
  if (word == 8 && strncmp(text, "continue", 8) == 0) return new_stmt(S_CONTINUE, line->line);
  if (word == 4 && strncmp(text, "pass", 4) == 0) return new_stmt(S_PASS, line->line);
  if (word == 6 && strncmp(text, "global", 6) == 0) {
    Stmt *s = new_stmt(S_GLOBAL, line->line);
    Tokens t = { 0 };
    tokenize(text + 6, &t, line->line);
    while (peek(&t)->kind == TK_NAME) {
      s->params = checked(realloc(s->params, (s->param_count + 1) * sizeof(char *)));
      s->params[s->param_count++] = strdup(advance(&t)->text);
      if (!is_op(&t, ",")) break;
      advance(&t);
    }
    tokens_free(&t);
    return s;
  }

  // Assignment or expression.
  Tokens t = { 0 };
  tokenize(text, &t, line->line);
  // Find a top-level = or op= to decide.
  int depth = 0;
  int assign_at = -1;
  for (int i = 0;i < t.count;i++) {
    Token *token = &t.items[i];
    if (token->kind != TK_OP) continue;
    if (strchr("([{", token->text[0])) depth++;
    else if (strchr(")]}", token->text[0])) depth--;
    else if (depth == 0 && (strcmp(token->text, "=") == 0 || strcmp(token->text, "+=") == 0 ||
             strcmp(token->text, "-=") == 0 || strcmp(token->text, "*=") == 0 ||
             strcmp(token->text, "/=") == 0)) {
      assign_at = i;
      break;
    }
  }
  if (assign_at < 0) {
    Stmt *s = new_stmt(S_EXPR, line->line);
    s->value = parse_expression(&t);
    if (peek(&t)->kind != TK_END) {
      char *rest = strdup(peek(&t)->text);
      tokens_free(&t);
      free_stmt(s);
      fail(line->line, "didn't expect '%s' here", rest);
    }
    tokens_free(&t);
    return s;
  }

  const char *op = t.items[assign_at].text;
  Stmt *s = new_stmt(strcmp(op, "=") == 0 ? S_ASSIGN : S_AUGMENT, line->line);
  s->op = op[0] == '+' ? OP_ADD : op[0] == '-' ? OP_SUB : op[0] == '*' ? OP_MUL : OP_DIV;
  while (t.position < assign_at) {
    s->targets = checked(realloc(s->targets, (s->target_count + 1) * sizeof(Expr *)));
    s->targets[s->target_count++] = parse_target(&t);
    if (is_op(&t, ",")) advance(&t);
  }
  if (s->kind == S_AUGMENT && s->target_count != 1) {
    tokens_free(&t);
    free_stmt(s);
    fail(line->line, "'%s' works on one variable at a time", op);
  }
  advance(&t);  // the = itself
  // a, b = 1, 2  -> the right side is a list
  Expr *value = parse_expression(&t);
  if (is_op(&t, ",")) {
    Expr *list = new_expr(EX_LIST, line->line);
    expr_push(list, value, NULL);
    while (is_op(&t, ",")) {
      advance(&t);
      expr_push(list, parse_expression(&t), NULL);
    }
    value = list;
  }
  s->value = value;
  if (peek(&t)->kind != TK_END) {
    char *rest = strdup(peek(&t)->text);
    tokens_free(&t);
    free_stmt(s);
    fail(line->line, "didn't expect '%s' here", rest);
  }
  tokens_free(&t);
  return s;
}

static Block *parse_block(Line *lines, int *index, int count, int indent) {
  Block *block = checked(calloc(1, sizeof(Block)));
  while (*index < count) {
    Line *line = &lines[*index];
    if (line->indent < indent) break;
    if (line->indent > indent) {
      free_block(block);
      fail(line->line, "this line's indentation doesn't line up with the lines above it");
    }
    block_push(block, parse_statement(lines, index, count, indent));
  }
  return block;
}

// ===========================================================================
// Scopes
// ===========================================================================

typedef struct {
  int count, capacity;
  char **names;
  Value *values;
}Scope;

typedef struct Frame {
  Scope locals;
  struct Frame *parent;
  char **global_names;
  int global_count;
  Value return_value;
  int depth;
}Frame;

static Scope globals;
static Frame *frame = NULL;
static int keep_program = 0;
static int last_statement_status = 0;
static int repl_mode = 0;

static int scope_find(Scope *scope, const char *name, size_t length) {
  for (int i = 0;i < scope->count;i++) {
    if (strlen(scope->names[i]) == length && strncmp(scope->names[i], name, length) == 0) return i;
  }
  return -1;
}

// Takes ownership of `value`.
static void scope_set(Scope *scope, const char *name, Value value) {
  int index = scope_find(scope, name, strlen(name));
  if (index >= 0) {
    decref(scope->values[index]);
    scope->values[index] = value;
    return;
  }
  if (scope->count == scope->capacity) {
    scope->capacity = scope->capacity ? scope->capacity * 2 : 16;
    scope->names = checked(realloc(scope->names, scope->capacity * sizeof(char *)));
    scope->values = checked(realloc(scope->values, scope->capacity * sizeof(Value)));
  }
  scope->names[scope->count] = strdup(name);
  scope->values[scope->count] = value;
  scope->count++;
}

static int scope_remove(Scope *scope, const char *name) {
  int index = scope_find(scope, name, strlen(name));
  if (index < 0) return 0;
  free(scope->names[index]);
  decref(scope->values[index]);
  scope->names[index] = scope->names[scope->count - 1];
  scope->values[index] = scope->values[scope->count - 1];
  scope->count--;
  return 1;
}

static void scope_clear(Scope *scope) {
  for (int i = 0;i < scope->count;i++) {
    free(scope->names[i]);
    decref(scope->values[i]);
  }
  free(scope->names);
  free(scope->values);
  memset(scope, 0, sizeof(*scope));
}

static int lookup_global_func(const char *name, size_t length) {
  int index = scope_find(&globals, name, length);
  return index >= 0 && globals.values[index].type == T_FUNC;
}

static int global_exists(const char *name, size_t length) {
  return scope_find(&globals, name, length) >= 0;
}

static int declared_global(const char *name) {
  if (frame == NULL) return 1;
  for (int i = 0;i < frame->global_count;i++) {
    if (strcmp(frame->global_names[i], name) == 0) return 1;
  }
  return 0;
}

static Value positional_list(void) {
  Value list = v_list();
  int count = 0;
  char **args = expand_positional(&count);
  for (int i = 1;i < count;i++) list_push(list.as.list, v_str(args[i]));
  return list;
}

// Reads a variable; returns 0 if it doesn't exist.
static int read_variable(const char *name, Value *out) {
  size_t length = strlen(name);
  if (frame != NULL && !declared_global(name)) {
    int index = scope_find(&frame->locals, name, length);
    if (index >= 0) { *out = incref(frame->locals.values[index]); return 1; }
  }
  int index = scope_find(&globals, name, length);
  if (index >= 0) { *out = incref(globals.values[index]); return 1; }
  if (strcmp(name, "status") == 0) { *out = v_int(expand_last_status()); return 1; }
  if (name[0] >= '0' && name[0] <= '9' && name[1] == '\0') {
    int count = 0;
    char **args = expand_positional(&count);
    int index = name[0] - '0';
    *out = index < count ? v_str(args[index]) : (index == 0 ? v_str("tarsh") : v_str(""));
    return 1;
  }
  if (strcmp(name, "argv") == 0) { *out = positional_list(); return 1; }
  const char *environment = getenv(name);
  if (environment != NULL) { *out = v_str(environment); return 1; }
  return 0;
}

// Takes ownership of `value`.
static void write_variable(const char *name, Value value) {
  if (frame != NULL && !declared_global(name)) {
    scope_set(&frame->locals, name, value);
    return;
  }
  // Exported variables stay exported, like in other shells.
  if (frame == NULL && value.type != T_LIST && value.type != T_DICT && value.type != T_FUNC &&
      getenv(name) != NULL && scope_find(&globals, name, strlen(name)) < 0) {
    char *text = to_text(value);
    setenv(name, text, 1);
    free(text);
    decref(value);
    return;
  }
  scope_set(&globals, name, value);
}

// ===========================================================================
// Operators
// ===========================================================================

static int values_equal(Value a, Value b);

static int contains(Value container, Value item, int line) {
  switch (container.type) {
    case T_STR: {
      char *needle = to_text(item);
      int found = strstr(container.as.str->text, needle) != NULL;
      free(needle);
      return found;
    }
    case T_LIST:
      for (int i = 0;i < container.as.list->count;i++) {
        if (values_equal(container.as.list->items[i], item)) return 1;
      }
      return 0;
    case T_DICT:
      for (int i = 0;i < container.as.dict->count;i++) {
        if (values_equal(container.as.dict->keys[i], item)) return 1;
      }
      return 0;
    default:
      fail(line, "can't use 'in' with a %s", type_name(container));
  }
  return 0;
}

static int values_equal(Value a, Value b) {
  double x, y; int xi, yi; long long xn, yn;
  int a_number = a.type == T_INT || a.type == T_FLOAT || a.type == T_BOOL;
  int b_number = b.type == T_INT || b.type == T_FLOAT || b.type == T_BOOL;
  if ((a_number || b_number) && as_number(a, &x, &xi, &xn) && as_number(b, &y, &yi, &yn)) {
    if (xi && yi) return xn == yn;
    return x == y;
  }
  if (a.type != b.type) return 0;
  switch (a.type) {
    case T_NONE: return 1;
    case T_STR: return a.as.str->length == b.as.str->length && memcmp(a.as.str->text, b.as.str->text, a.as.str->length) == 0;
    case T_LIST:
      if (a.as.list->count != b.as.list->count) return 0;
      for (int i = 0;i < a.as.list->count;i++) if (!values_equal(a.as.list->items[i], b.as.list->items[i])) return 0;
      return 1;
    case T_DICT:
      if (a.as.dict->count != b.as.dict->count) return 0;
      for (int i = 0;i < a.as.dict->count;i++) {
        int found = 0;
        for (int j = 0;j < b.as.dict->count;j++) {
          if (values_equal(a.as.dict->keys[i], b.as.dict->keys[j])) {
            if (!values_equal(a.as.dict->values[i], b.as.dict->values[j])) return 0;
            found = 1;
            break;
          }
        }
        if (!found) return 0;
      }
      return 1;
    case T_FUNC: return a.as.func == b.as.func;
    default: return 0;
  }
}

// <0, 0, >0
static int compare_order(Value a, Value b, int line) {
  double x, y; int xi, yi; long long xn, yn;
  int a_number = a.type == T_INT || a.type == T_FLOAT || a.type == T_BOOL;
  int b_number = b.type == T_INT || b.type == T_FLOAT || b.type == T_BOOL;
  if ((a_number || b_number) && as_number(a, &x, &xi, &xn) && as_number(b, &y, &yi, &yn)) {
    if (xi && yi) return (xn > yn) - (xn < yn);
    return (x > y) - (x < y);
  }
  if (a.type == T_STR && b.type == T_STR) return strcmp(a.as.str->text, b.as.str->text);
  if (a.type == T_LIST && b.type == T_LIST) {
    for (int i = 0;i < a.as.list->count && i < b.as.list->count;i++) {
      int c = compare_order(a.as.list->items[i], b.as.list->items[i], line);
      if (c) return c;
    }
    return (a.as.list->count > b.as.list->count) - (a.as.list->count < b.as.list->count);
  }
  char left[32];
  snprintf(left, sizeof(left), "%s", a_type(a));
  fail(line, "can't compare %s with %s", left, a_type(b));
  return 0;
}

static Value repeat(Value v, long long times) {
  if (times < 0) times = 0;
  if (v.type == T_STR) {
    SB sb = { 0 };
    sb_add(&sb, "");
    for (long long i = 0;i < times;i++) sb_addn(&sb, v.as.str->text, v.as.str->length);
    return v_str_take(sb_take(&sb));
  }
  Value list = v_list();
  for (long long i = 0;i < times;i++) {
    for (int j = 0;j < v.as.list->count;j++) list_push(list.as.list, incref(v.as.list->items[j]));
  }
  return list;
}

static Value binary(int op, Value a, Value b, int line) {
  double x, y; int xi, yi; long long xn, yn;
  if (op == OP_ADD) {
    if (a.type == T_STR && b.type == T_STR) {
      Value result = v_strn(a.as.str->text, a.as.str->length + b.as.str->length);
      memcpy(result.as.str->text + a.as.str->length, b.as.str->text, b.as.str->length);
      result.as.str->text[result.as.str->length] = '\0';
      return result;
    }
    if (a.type == T_LIST && b.type == T_LIST) {
      Value list = v_list();
      for (int i = 0;i < a.as.list->count;i++) list_push(list.as.list, incref(a.as.list->items[i]));
      for (int i = 0;i < b.as.list->count;i++) list_push(list.as.list, incref(b.as.list->items[i]));
      return list;
    }
    if (!as_number(a, &x, &xi, &xn) || !as_number(b, &y, &yi, &yn)) {
      if (a.type == T_STR || b.type == T_STR) {
        char *left = to_text(a), *right = to_text(b);
        SB sb = { 0 };
        sb_add(&sb, left);
        sb_add(&sb, right);
        free(left);
        free(right);
        return v_str_take(sb_take(&sb));
      }
      { char left[32]; snprintf(left, sizeof(left), "%s", a_type(a)); fail(line, "can't add %s and %s", left, a_type(b)); }
    }
    return (xi && yi) ? v_int(xn + yn) : v_float(x + y);
  }
  if (op == OP_MUL) {
    if ((a.type == T_STR || a.type == T_LIST) && (b.type == T_INT || b.type == T_BOOL)) return repeat(a, b.as.integer);
    if ((b.type == T_STR || b.type == T_LIST) && (a.type == T_INT || a.type == T_BOOL)) return repeat(b, a.as.integer);
  }
  if (!as_number(a, &x, &xi, &xn) || !as_number(b, &y, &yi, &yn)) {
    static const char *names[] = { "+", "-", "*", "/", "//", "%", "**" };
    char left[32];
    snprintf(left, sizeof(left), "%s", a_type(a));
    fail(line, "'%s' needs numbers, not %s and %s", names[op], left, a_type(b));
  }
  switch (op) {
    case OP_SUB: return (xi && yi) ? v_int(xn - yn) : v_float(x - y);
    case OP_MUL: return (xi && yi) ? v_int(xn * yn) : v_float(x * y);
    case OP_DIV:
      if (y == 0) fail(line, "division by zero");
      return v_float(x / y);
    case OP_FLOORDIV:
      if (y == 0) fail(line, "division by zero");
      if (xi && yi) {
        long long q = xn / yn;
        if ((xn % yn != 0) && ((xn < 0) != (yn < 0))) q--;
        return v_int(q);
      }
      return v_float(floor(x / y));
    case OP_MOD:
      if (y == 0) fail(line, "division by zero");
      if (xi && yi) {
        long long r = xn % yn;
        if (r != 0 && ((r < 0) != (yn < 0))) r += yn;
        return v_int(r);
      }
      return v_float(x - y * floor(x / y));
    case OP_POW:
      if (xi && yi && yn >= 0) {
        long long result = 1;
        for (long long i = 0;i < yn;i++) result *= xn;
        return v_int(result);
      }
      return v_float(pow(x, y));
  }
  return v_none();
}

static long long as_index(Value v, int line) {
  double d; int is_int; long long i;
  if (!as_number(v, &d, &is_int, &i) || !is_int) fail(line, "an index must be a whole number, not %s", a_type(v));
  return i;
}

static Value get_item(Value container, Value key, int line) {
  if (container.type == T_LIST || container.type == T_STR) {
    long long length = container.type == T_LIST ? container.as.list->count : (long long)container.as.str->length;
    long long i = as_index(key, line);
    if (i < 0) i += length;
    if (i < 0 || i >= length) fail(line, "index %lld is out of range (length %lld)", as_index(key, line), length);
    if (container.type == T_LIST) return incref(container.as.list->items[i]);
    return v_strn(container.as.str->text + i, 1);
  }
  if (container.type == T_DICT) {
    for (int i = 0;i < container.as.dict->count;i++) {
      if (values_equal(container.as.dict->keys[i], key)) return incref(container.as.dict->values[i]);
    }
    char *shown = to_repr(key);
    char message[256];
    snprintf(message, sizeof(message), "no key %s in the dict (use get(d, key, default) to allow missing keys)", shown);
    free(shown);
    fail(line, "%s", message);
  }
  fail(line, "can't use [ ] on a %s", type_name(container));
  return v_none();
}

// Takes ownership of `value`.
static void set_item(Value container, Value key, Value value, int line) {
  if (container.type == T_LIST) {
    long long i = as_index(key, line);
    if (i < 0) i += container.as.list->count;
    if (i < 0 || i >= container.as.list->count) {
      decref(value);
      fail(line, "index out of range (use append to add items)");
    }
    decref(container.as.list->items[i]);
    container.as.list->items[i] = value;
    return;
  }
  if (container.type == T_DICT) {
    Dict *d = container.as.dict;
    for (int i = 0;i < d->count;i++) {
      if (values_equal(d->keys[i], key)) {
        decref(d->values[i]);
        d->values[i] = value;
        return;
      }
    }
    if (d->count == d->capacity) {
      d->capacity = d->capacity ? d->capacity * 2 : 8;
      d->keys = checked(realloc(d->keys, d->capacity * sizeof(Value)));
      d->values = checked(realloc(d->values, d->capacity * sizeof(Value)));
    }
    d->keys[d->count] = incref(key);
    d->values[d->count] = value;
    d->count++;
    return;
  }
  decref(value);
  fail(line, "can't set items on a %s", type_name(container));
}

static Value slice(Value container, Value start, Value end, int line) {
  long long length;
  if (container.type == T_LIST) length = container.as.list->count;
  else if (container.type == T_STR) length = (long long)container.as.str->length;
  else { fail(line, "can't slice a %s", type_name(container)); return v_none(); }
  long long from = start.type == T_NONE ? 0 : as_index(start, line);
  long long to = end.type == T_NONE ? length : as_index(end, line);
  if (from < 0) from += length;
  if (to < 0) to += length;
  if (from < 0) from = 0;
  if (to > length) to = length;
  if (to < from) to = from;
  if (container.type == T_STR) return v_strn(container.as.str->text + from, to - from);
  Value list = v_list();
  for (long long i = from;i < to;i++) list_push(list.as.list, incref(container.as.list->items[i]));
  return list;
}

// Splits text into whitespace-separated words.
static Value words_of(const char *text) {
  Value list = v_list();
  const char *c = text;
  while (*c) {
    while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') c++;
    if (!*c) break;
    const char *start = c;
    while (*c && *c != ' ' && *c != '\t' && *c != '\n' && *c != '\r') c++;
    list_push(list.as.list, v_strn(start, c - start));
  }
  return list;
}

// What `for x in value:` walks over: lists, dict keys, words of a string.
static Value iteration_list(Value v, int line) {
  switch (v.type) {
    case T_LIST: return incref(v);
    case T_STR: return words_of(v.as.str->text);
    case T_DICT: {
      Value list = v_list();
      for (int i = 0;i < v.as.dict->count;i++) list_push(list.as.list, incref(v.as.dict->keys[i]));
      return list;
    }
    case T_NONE: return v_list();
    default:
      fail(line, "can't loop over %s", a_type(v));
  }
  return v_none();
}

// ===========================================================================
// Evaluation
// ===========================================================================

static Value eval(Expr *e);
typedef enum { FLOW_NORMAL, FLOW_BREAK, FLOW_CONTINUE, FLOW_RETURN, FLOW_EXIT }Flow;
static Flow exec_block(Block *block);
static Value call_builtin(int index, Value *args, int count, char **kw_names, Value *kw_values, int kw_count, int line);
static Value call_user(Func *f, Value *args, int count, char **kw_names, Value *kw_values, int kw_count, int line, int as_command);

// "text with $name, ${name} and $(command)" plus \n-style escapes.
static Value interpolate(const char *raw, int line) {
  (void)line;
  SB sb = { 0 };
  sb_add(&sb, "");
  for (size_t i = 0;raw[i];i++) {
    char c = raw[i];
    if (c == '\\' && raw[i + 1]) {
      char next = raw[++i];
      switch (next) {
        case 'n': sb_addc(&sb, '\n'); break;
        case 't': sb_addc(&sb, '\t'); break;
        case 'r': sb_addc(&sb, '\r'); break;
        case 'e': sb_addc(&sb, '\033'); break;
        default: sb_addc(&sb, next); break;
      }
      continue;
    }
    if (c != '$') { sb_addc(&sb, c); continue; }
    if (raw[i + 1] == '(') {
      int unterminated = 0;
      size_t end = syntax_subst_end(raw, i, &unterminated);
      char *command = strndup(raw + i + 2, end - i - 3);
      char *output = expand_capture(command);
      sb_add(&sb, output);
      free(command);
      free(output);
      i = end - 1;
      continue;
    }
    size_t start = i + 1, stop;
    int braced = raw[start] == '{';
    if (braced) start++;
    stop = start;
    while (is_name_char(raw[stop])) stop++;
    if (stop == start || (braced && raw[stop] != '}')) { sb_addc(&sb, '$'); continue; }
    char *name = strndup(raw + start, stop - start);
    Value value;
    if (read_variable(name, &value)) {
      if (value.type == T_LIST) {
        for (int k = 0;k < value.as.list->count;k++) {
          char *text = to_text(value.as.list->items[k]);
          if (k) sb_addc(&sb, ' ');
          sb_add(&sb, text);
          free(text);
        }
      }
      else {
        char *text = to_text(value);
        sb_add(&sb, text);
        free(text);
      }
      decref(value);
    }
    free(name);
    i = braced ? stop : stop - 1;
  }
  return v_str_take(sb_take(&sb));
}

static void format_part(SB *sb, Value v, const char *spec) {
  if (spec != NULL && spec[0] == '.' && spec[strlen(spec) - 1] == 'f') {
    double d; int is_int; long long i;
    if (as_number(v, &d, &is_int, &i)) {
      char buffer[64];
      snprintf(buffer, sizeof(buffer), "%.*f", atoi(spec + 1), d);
      sb_add(sb, buffer);
      return;
    }
  }
  char *text = to_text(v);
  // :>8 / :<8 pad to a width
  if (spec != NULL && (spec[0] == '>' || spec[0] == '<')) {
    int width = atoi(spec + 1);
    int length = (int)strlen(text);
    if (spec[0] == '>') for (int k = length;k < width;k++) sb_addc(sb, ' ');
    sb_add(sb, text);
    if (spec[0] == '<') for (int k = length;k < width;k++) sb_addc(sb, ' ');
  }
  else {
    sb_add(sb, text);
  }
  free(text);
}

static int builtin_function_index(const char *name) {
  return builtin_function_index_n(name, strlen(name));
}

static Value eval_call(Expr *e, Value *receiver) {
  int total = e->count + (receiver ? 1 : 0);
  Value *args = checked(calloc(total > 0 ? total : 1, sizeof(Value)));
  char **kw_names = checked(calloc(e->count > 0 ? e->count : 1, sizeof(char *)));
  Value *kw_values = checked(calloc(e->count > 0 ? e->count : 1, sizeof(Value)));
  int count = 0, kw_count = 0;
  if (receiver) args[count++] = incref(*receiver);
  for (int i = 0;i < e->count;i++) {
    if (e->names && e->names[i]) {
      kw_names[kw_count] = e->names[i];
      kw_values[kw_count++] = eval(e->items[i]);
    }
    else {
      args[count++] = eval(e->items[i]);
    }
  }

  Value result = v_none();
  Value callee;
  int found_variable = receiver == NULL && read_variable(e->text, &callee);
  if (found_variable && callee.type == T_FUNC) {
    result = call_user(callee.as.func, args, count, kw_names, kw_values, kw_count, e->line, 0);
    decref(callee);
  }
  else {
    if (found_variable) decref(callee);
    int index = builtin_function_index(e->text);
    if (index < 0) {
      for (int i = 0;i < count;i++) decref(args[i]);
      for (int i = 0;i < kw_count;i++) decref(kw_values[i]);
      free(args); free(kw_names); free(kw_values);
      if (receiver) fail(e->line, "a %s has no method '%s'", type_name(*receiver), e->text);
      fail(e->line, "there's no function called '%s'", e->text);
    }
    result = call_builtin(index, args, count, kw_names, kw_values, kw_count, e->line);
  }
  for (int i = 0;i < count;i++) decref(args[i]);
  for (int i = 0;i < kw_count;i++) decref(kw_values[i]);
  free(args);
  free(kw_names);
  free(kw_values);
  return result;
}

static Value eval(Expr *e) {
  switch (e->kind) {
    case EX_CONST:
      return incref(e->constant);
    case EX_ISTRING:
      return interpolate(e->text, e->line);
    case EX_FSTRING: {
      SB sb = { 0 };
      sb_add(&sb, "");
      for (int i = 0;i < e->count;i++) {
        Value part = eval(e->items[i]);
        format_part(&sb, part, e->names ? e->names[i] : NULL);
        decref(part);
      }
      return v_str_take(sb_take(&sb));
    }
    case EX_NAME: {
      Value v;
      if (!read_variable(e->text, &v)) {
        if (builtin_function_index(e->text) >= 0) fail(e->line, "'%s' is a function: call it with %s(...)", e->text, e->text);
        fail(e->line, "'%s' isn't set (assign it first, e.g. %s = \"\")", e->text, e->text);
      }
      return v;
    }
    case EX_LIST: {
      Value list = v_list();
      for (int i = 0;i < e->count;i++) list_push(list.as.list, eval(e->items[i]));
      return list;
    }
    case EX_DICT: {
      Value dict = v_dict();
      for (int i = 0;i + 1 < e->count;i += 2) {
        Value key = eval(e->items[i]);
        set_item(dict, key, eval(e->items[i + 1]), e->line);
        decref(key);
      }
      return dict;
    }
    case EX_INDEX: {
      Value container = eval(e->left);
      Value key = eval(e->right);
      Value result = get_item(container, key, e->line);
      decref(container);
      decref(key);
      return result;
    }
    case EX_SLICE: {
      Value container = eval(e->left);
      Value start = e->right ? eval(e->right) : v_none();
      Value end = e->third ? eval(e->third) : v_none();
      Value result = slice(container, start, end, e->line);
      decref(container); decref(start); decref(end);
      return result;
    }
    case EX_CALL:
      return eval_call(e, NULL);
    case EX_METHOD: {
      Value receiver = eval(e->left);
      Value result = eval_call(e, &receiver);
      decref(receiver);
      return result;
    }
    case EX_BINARY: {
      Value a = eval(e->left);
      Value b = eval(e->right);
      Value result = binary(e->op, a, b, e->line);
      decref(a);
      decref(b);
      return result;
    }
    case EX_NEGATE: {
      Value a = eval(e->left);
      double d; int is_int; long long i;
      if (!as_number(a, &d, &is_int, &i)) { decref(a); fail(e->line, "can't make a %s negative", type_name(a)); }
      decref(a);
      return is_int ? v_int(-i) : v_float(-d);
    }
    case EX_NOT: {
      Value a = eval(e->left);
      int result = !truthy(a);
      decref(a);
      return v_bool(result);
    }
    case EX_AND: {
      Value a = eval(e->left);
      if (!truthy(a)) return a;
      decref(a);
      return eval(e->right);
    }
    case EX_OR: {
      Value a = eval(e->left);
      if (truthy(a)) return a;
      decref(a);
      return eval(e->right);
    }
    case EX_TERNARY: {
      Value condition = eval(e->right);
      int yes = truthy(condition);
      decref(condition);
      return eval(yes ? e->left : e->third);
    }
    case EX_COMPARE: {
      Value left = eval(e->items[0]);
      for (int i = 1;i < e->count;i++) {
        Value right = eval(e->items[i]);
        int op = e->ops[i - 1];
        int ok;
        switch (op) {
          case CMP_EQ: ok = values_equal(left, right); break;
          case CMP_NE: ok = !values_equal(left, right); break;
          case CMP_LT: ok = compare_order(left, right, e->line) < 0; break;
          case CMP_LE: ok = compare_order(left, right, e->line) <= 0; break;
          case CMP_GT: ok = compare_order(left, right, e->line) > 0; break;
          case CMP_GE: ok = compare_order(left, right, e->line) >= 0; break;
          case CMP_IN: ok = contains(right, left, e->line); break;
          default: ok = !contains(right, left, e->line); break;
        }
        decref(left);
        if (!ok) { decref(right); return v_bool(0); }
        left = right;
      }
      decref(left);
      return v_bool(1);
    }
    case EX_CAPTURE:
      return v_str_take(expand_capture(e->text));
    case EX_SUCCESS: {
      int status = exec_run_line(e->text);
      return v_bool(status == 0);
    }
  }
  return v_none();
}

// ===========================================================================
// Statements
// ===========================================================================

static void assign_to(Expr *target, Value value) {
  if (target->kind == EX_NAME) {
    write_variable(target->text, value);
    return;
  }
  Value container = eval(target->left);
  Value key = eval(target->right);
  set_item(container, key, value, target->line);
  decref(container);
  decref(key);
}

static void unpack_into(Expr **targets, int count, Value value, int line) {
  if (count == 1) {
    assign_to(targets[0], value);
    return;
  }
  if (value.type != T_LIST || value.as.list->count != count) {
    int got = value.type == T_LIST ? value.as.list->count : 1;
    decref(value);
    fail(line, "expected %d values to unpack but got %d", count, got);
  }
  for (int i = 0;i < count;i++) assign_to(targets[i], incref(value.as.list->items[i]));
  decref(value);
}

static Flow exec_statement(Stmt *s) {
  if (script_interrupted) {
    script_interrupted = 0;
    fail(s->line, "interrupted");
  }
  switch (s->kind) {
    case S_COMMAND: {
      int status = exec_run_line(s->text);
      last_statement_status = status;
      return exec_should_exit() ? FLOW_EXIT : FLOW_NORMAL;
    }
    case S_EXPR: {
      Value v = eval(s->value);
      if (repl_mode && frame == NULL && v.type != T_NONE) {
        char *text = (v.type == T_STR) ? to_text(v) : to_repr(v);
        printf("%s\n", text);
        free(text);
      }
      decref(v);
      last_statement_status = 0;
      return exec_should_exit() ? FLOW_EXIT : FLOW_NORMAL;
    }
    case S_ASSIGN:
      unpack_into(s->targets, s->target_count, eval(s->value), s->line);
      last_statement_status = 0;
      return FLOW_NORMAL;
    case S_AUGMENT: {
      Value current = eval(s->targets[0]);
      Value amount = eval(s->value);
      Value result = binary(s->op, current, amount, s->line);
      decref(current);
      decref(amount);
      assign_to(s->targets[0], result);
      last_statement_status = 0;
      return FLOW_NORMAL;
    }
    case S_IF: {
      Value condition = eval(s->value);
      int yes = truthy(condition);
      decref(condition);
      if (yes) return exec_block(s->body);
      if (s->orelse) return exec_block(s->orelse);
      return FLOW_NORMAL;
    }
    case S_WHILE:
      while (1) {
        Value condition = eval(s->value);
        int yes = truthy(condition);
        decref(condition);
        if (!yes) break;
        Flow flow = exec_block(s->body);
        if (flow == FLOW_BREAK) break;
        if (flow == FLOW_RETURN || flow == FLOW_EXIT) return flow;
      }
      return FLOW_NORMAL;
    case S_FOR: {
      Value iterable = eval(s->value);
      Value items = iteration_list(iterable, s->line);
      decref(iterable);
      for (int i = 0;i < items.as.list->count;i++) {
        unpack_into(s->targets, s->target_count, incref(items.as.list->items[i]), s->line);
        Flow flow = exec_block(s->body);
        if (flow == FLOW_BREAK) break;
        if (flow == FLOW_RETURN || flow == FLOW_EXIT) { decref(items); return flow; }
      }
      decref(items);
      return FLOW_NORMAL;
    }
    case S_DEF: {
      Func *f = checked(calloc(1, sizeof(Func)));
      f->refs = 1;
      f->name = strdup(s->name);
      f->param_count = s->param_count;
      f->params = checked(calloc(s->param_count > 0 ? s->param_count : 1, sizeof(char *)));
      f->defaults = checked(calloc(s->param_count > 0 ? s->param_count : 1, sizeof(Expr *)));
      for (int i = 0;i < s->param_count;i++) {
        f->params[i] = strdup(s->params[i]);
        f->defaults[i] = s->defaults[i];
      }
      f->body = s->body;
      f->source = strdup(s->text ? s->text : "");
      Value v;
      v.type = T_FUNC;
      v.as.func = f;
      write_variable(s->name, v);
      keep_program = 1;
      last_statement_status = 0;
      return FLOW_NORMAL;
    }
    case S_RETURN:
      if (frame != NULL) {
        decref(frame->return_value);
        frame->return_value = s->value ? eval(s->value) : v_none();
      }
      else if (s->value) {
        // `return n` at the top of a script ends it with that status.
        Value v = eval(s->value);
        double d; int is_int; long long i;
        last_statement_status = (v.type == T_BOOL) ? !v.as.boolean : as_number(v, &d, &is_int, &i) ? (int)d : 0;
        decref(v);
      }
      return FLOW_RETURN;
    case S_BREAK:
      return FLOW_BREAK;
    case S_CONTINUE:
      return FLOW_CONTINUE;
    case S_PASS:
      return FLOW_NORMAL;
    case S_GLOBAL:
      if (frame != NULL) {
        for (int i = 0;i < s->param_count;i++) {
          frame->global_names = checked(realloc(frame->global_names, (frame->global_count + 1) * sizeof(char *)));
          frame->global_names[frame->global_count++] = s->params[i];
        }
      }
      return FLOW_NORMAL;
  }
  return FLOW_NORMAL;
}

static Flow exec_block(Block *block) {
  for (int i = 0;i < block->count;i++) {
    Flow flow = exec_statement(block->items[i]);
    if (flow != FLOW_NORMAL) return flow;
  }
  return FLOW_NORMAL;
}

static Value call_user(Func *f, Value *args, int count, char **kw_names, Value *kw_values, int kw_count, int line, int as_command) {
  int depth = frame ? frame->depth + 1 : 1;
  if (depth > 400) fail(line, "too much recursion in %s()", f->name);
  if (!as_command && count > f->param_count) {
    fail(line, "%s() takes %d argument%s but got %d", f->name, f->param_count, f->param_count == 1 ? "" : "s", count);
  }

  Frame call;
  memset(&call, 0, sizeof(call));
  call.parent = frame;
  call.depth = depth;
  call.return_value = v_none();

  // Defaults are evaluated in the caller's scope, then bound.
  for (int i = 0;i < f->param_count;i++) {
    Value value;
    int bound = 0;
    if (i < count) { value = incref(args[i]); bound = 1; }
    for (int k = 0;k < kw_count;k++) {
      if (strcmp(kw_names[k], f->params[i]) == 0) {
        if (bound) decref(value);
        value = incref(kw_values[k]);
        bound = 1;
      }
    }
    if (!bound) {
      if (f->defaults[i] != NULL) value = eval(f->defaults[i]);
      else if (as_command) value = v_str("");
      else { scope_clear(&call.locals); fail(line, "%s() is missing the '%s' argument", f->name, f->params[i]); }
    }
    scope_set(&call.locals, f->params[i], value);
  }
  for (int k = 0;k < kw_count;k++) {
    int known = 0;
    for (int i = 0;i < f->param_count;i++) if (strcmp(kw_names[k], f->params[i]) == 0) known = 1;
    if (!known) { scope_clear(&call.locals); fail(line, "%s() has no parameter called '%s'", f->name, kw_names[k]); }
  }
  Value argv = v_list();
  for (int i = 0;i < count;i++) list_push(argv.as.list, incref(args[i]));
  scope_set(&call.locals, "argv", argv);

  Frame *previous = frame;
  frame = &call;
  Flow flow = exec_block(f->body);
  frame = previous;

  Value result = call.return_value;
  if (flow == FLOW_EXIT) {
    // exit() inside a function ends everything.
  }
  scope_clear(&call.locals);
  free(call.global_names);
  return result;
}

// ===========================================================================
// Builtin functions
// ===========================================================================

typedef Value (*BuiltinFn)(Value *args, int count, char **kw_names, Value *kw_values, int kw_count, int line);

static void need_args(const char *name, int count, int min, int max, int line) {
  if (count < min || (max >= 0 && count > max)) {
    if (min == max) fail(line, "%s() takes %d argument%s, got %d", name, min, min == 1 ? "" : "s", count);
    fail(line, "%s() got %d arguments", name, count);
  }
}

static const char *need_str(const char *name, Value v, int line) {
  if (v.type != T_STR) fail(line, "%s() needs a string, not a %s", name, type_name(v));
  return v.as.str->text;
}

static List *need_list(const char *name, Value v, int line) {
  if (v.type != T_LIST) fail(line, "%s() needs a list, not a %s", name, type_name(v));
  return v.as.list;
}

static Value keyword(const char *name, char **kw_names, Value *kw_values, int kw_count, Value fallback) {
  for (int i = 0;i < kw_count;i++) if (strcmp(kw_names[i], name) == 0) return kw_values[i];
  return fallback;
}

#define BUILTIN(fn) static Value fn(Value *args, int count, char **kw_names, Value *kw_values, int kw_count, int line)
#define UNUSED_KW (void)kw_names; (void)kw_values; (void)kw_count

BUILTIN(fn_print) {
  Value sep_value = keyword("sep", kw_names, kw_values, kw_count, v_none());
  Value end_value = keyword("end", kw_names, kw_values, kw_count, v_none());
  char *sep = sep_value.type == T_NONE ? strdup(" ") : to_text(sep_value);
  char *end = end_value.type == T_NONE ? strdup("\n") : to_text(end_value);
  (void)line;
  for (int i = 0;i < count;i++) {
    if (i) fputs(sep, stdout);
    char *text = to_text(args[i]);
    fputs(text, stdout);
    free(text);
  }
  fputs(end, stdout);
  fflush(stdout);
  free(sep);
  free(end);
  return v_none();
}

BUILTIN(fn_input) {
  UNUSED_KW;
  need_args("input", count, 0, 1, line);
  if (count == 1) {
    char *prompt = to_text(args[0]);
    fputs(prompt, stdout);
    fflush(stdout);
    free(prompt);
  }
  SB sb = { 0 };
  sb_add(&sb, "");
  char c;
  while (read(STDIN_FILENO, &c, 1) == 1 && c != '\n') sb_addc(&sb, c);
  return v_str_take(sb_take(&sb));
}

BUILTIN(fn_len) {
  UNUSED_KW;
  need_args("len", count, 1, 1, line);
  switch (args[0].type) {
    case T_STR: return v_int((long long)args[0].as.str->length);
    case T_LIST: return v_int(args[0].as.list->count);
    case T_DICT: return v_int(args[0].as.dict->count);
    default: fail(line, "len() doesn't work on %s", a_type(args[0]));
  }
  return v_none();
}

BUILTIN(fn_str) { UNUSED_KW; need_args("str", count, 0, 1, line); return count ? v_str_take(to_text(args[0])) : v_str(""); }

BUILTIN(fn_int) {
  UNUSED_KW;
  need_args("int", count, 1, 1, line);
  double d; int is_int; long long i;
  if (args[0].type == T_STR) {
    char *end;
    const char *text = args[0].as.str->text;
    while (*text == ' ') text++;
    long long value = strtoll(text, &end, 10);
    while (*end == ' ' || *end == '\n') end++;
    if (*end == '\0' && end != text) return v_int(value);
    double real = strtod(text, &end);
    while (*end == ' ' || *end == '\n') end++;
    if (*end == '\0' && end != text) return v_int((long long)real);
    fail(line, "int() can't read \"%s\" as a number", args[0].as.str->text);
  }
  if (!as_number(args[0], &d, &is_int, &i)) fail(line, "int() can't convert a %s", type_name(args[0]));
  return v_int(is_int ? i : (long long)d);
}

BUILTIN(fn_float) {
  UNUSED_KW;
  need_args("float", count, 1, 1, line);
  double d; int is_int; long long i;
  if (args[0].type == T_STR) {
    char *end;
    double value = strtod(args[0].as.str->text, &end);
    while (*end == ' ' || *end == '\n') end++;
    if (*end != '\0' || end == args[0].as.str->text) fail(line, "float() can't read \"%s\" as a number", args[0].as.str->text);
    return v_float(value);
  }
  if (!as_number(args[0], &d, &is_int, &i)) fail(line, "float() can't convert a %s", type_name(args[0]));
  return v_float(d);
}

BUILTIN(fn_bool) { UNUSED_KW; need_args("bool", count, 1, 1, line); return v_bool(truthy(args[0])); }
BUILTIN(fn_type) { UNUSED_KW; need_args("type", count, 1, 1, line); return v_str(type_name(args[0])); }

BUILTIN(fn_range) {
  UNUSED_KW;
  need_args("range", count, 1, 3, line);
  long long start = 0, stop, step = 1;
  if (count == 1) stop = as_index(args[0], line);
  else { start = as_index(args[0], line); stop = as_index(args[1], line); }
  if (count == 3) step = as_index(args[2], line);
  if (step == 0) fail(line, "range() step can't be 0");
  long long total = step > 0 ? (stop - start + step - 1) / step : (start - stop - step - 1) / -step;
  if (total > 10000000) fail(line, "range() is too big (over 10 million numbers)");
  Value list = v_list();
  for (long long i = start;step > 0 ? i < stop : i > stop;i += step) list_push(list.as.list, v_int(i));
  return list;
}

BUILTIN(fn_words) { UNUSED_KW; need_args("words", count, 1, 1, line); char *t = to_text(args[0]); Value v = words_of(t); free(t); return v; }

BUILTIN(fn_list) {
  UNUSED_KW;
  need_args("list", count, 0, 1, line);
  if (count == 0) return v_list();
  Value items = iteration_list(args[0], line);
  if (args[0].type == T_LIST) {
    Value copy = v_list();
    for (int i = 0;i < items.as.list->count;i++) list_push(copy.as.list, incref(items.as.list->items[i]));
    decref(items);
    return copy;
  }
  return items;
}

BUILTIN(fn_chars) {
  UNUSED_KW;
  need_args("chars", count, 1, 1, line);
  const char *text = need_str("chars", args[0], line);
  Value list = v_list();
  for (size_t i = 0;text[i];) {
    size_t length = 1;
    while (((unsigned char)text[i + length] & 0xC0) == 0x80) length++;
    list_push(list.as.list, v_strn(text + i, length));
    i += length;
  }
  return list;
}

BUILTIN(fn_lines) {
  UNUSED_KW;
  need_args("lines", count, 1, 1, line);
  char *text = to_text(args[0]);
  Value list = v_list();
  char *cursor = text;
  while (*cursor) {
    char *newline = strchr(cursor, '\n');
    size_t length = newline ? (size_t)(newline - cursor) : strlen(cursor);
    list_push(list.as.list, v_strn(cursor, length));
    if (!newline) break;
    cursor = newline + 1;
  }
  free(text);
  return list;
}

BUILTIN(fn_split) {
  need_args("split", count, 1, 2, line);
  Value sep_value = count > 1 ? args[1] : keyword("sep", kw_names, kw_values, kw_count, v_none());
  const char *text = need_str("split", args[0], line);
  if (sep_value.type == T_NONE) return words_of(text);
  const char *sep = need_str("split", sep_value, line);
  size_t sep_length = strlen(sep);
  if (sep_length == 0) fail(line, "split() separator can't be empty");
  Value list = v_list();
  const char *cursor = text;
  const char *found;
  while ((found = strstr(cursor, sep)) != NULL) {
    list_push(list.as.list, v_strn(cursor, found - cursor));
    cursor = found + sep_length;
  }
  list_push(list.as.list, v_str(cursor));
  return list;
}

BUILTIN(fn_join) {
  UNUSED_KW;
  need_args("join", count, 1, 2, line);
  // join(list, sep), join(sep, list) or ", ".join(list)
  Value list = args[0], sep = count > 1 ? args[1] : v_none();
  if (args[0].type != T_LIST && count > 1 && args[1].type == T_LIST) { list = args[1]; sep = args[0]; }
  need_list("join", list, line);
  char *separator = sep.type == T_NONE ? strdup(" ") : to_text(sep);
  SB sb = { 0 };
  sb_add(&sb, "");
  for (int i = 0;i < list.as.list->count;i++) {
    if (i) sb_add(&sb, separator);
    char *text = to_text(list.as.list->items[i]);
    sb_add(&sb, text);
    free(text);
  }
  free(separator);
  return v_str_take(sb_take(&sb));
}

static Value strip_impl(const char *name, Value *args, int count, int line, int left, int right) {
  need_args(name, count, 1, 2, line);
  const char *text = need_str(name, args[0], line);
  const char *chars = count > 1 ? need_str(name, args[1], line) : " \t\n\r";
  size_t start = 0, end = strlen(text);
  if (left) while (start < end && strchr(chars, text[start])) start++;
  if (right) while (end > start && strchr(chars, text[end - 1])) end--;
  return v_strn(text + start, end - start);
}
BUILTIN(fn_strip) { UNUSED_KW; return strip_impl("strip", args, count, line, 1, 1); }
BUILTIN(fn_lstrip) { UNUSED_KW; return strip_impl("lstrip", args, count, line, 1, 0); }
BUILTIN(fn_rstrip) { UNUSED_KW; return strip_impl("rstrip", args, count, line, 0, 1); }

static Value case_impl(const char *name, Value *args, int count, int line, int upper) {
  need_args(name, count, 1, 1, line);
  Value result = v_str(need_str(name, args[0], line));
  for (size_t i = 0;i < result.as.str->length;i++) {
    char c = result.as.str->text[i];
    if (upper && c >= 'a' && c <= 'z') result.as.str->text[i] = c - 32;
    if (!upper && c >= 'A' && c <= 'Z') result.as.str->text[i] = c + 32;
  }
  return result;
}
BUILTIN(fn_upper) { UNUSED_KW; return case_impl("upper", args, count, line, 1); }
BUILTIN(fn_lower) { UNUSED_KW; return case_impl("lower", args, count, line, 0); }

BUILTIN(fn_replace) {
  UNUSED_KW;
  need_args("replace", count, 3, 3, line);
  const char *text = need_str("replace", args[0], line);
  char *from = to_text(args[1]);
  char *to = to_text(args[2]);
  size_t from_length = strlen(from);
  SB sb = { 0 };
  sb_add(&sb, "");
  if (from_length == 0) {
    sb_add(&sb, text);
  }
  else {
    const char *cursor = text, *found;
    while ((found = strstr(cursor, from)) != NULL) {
      sb_addn(&sb, cursor, found - cursor);
      sb_add(&sb, to);
      cursor = found + from_length;
    }
    sb_add(&sb, cursor);
  }
  free(from);
  free(to);
  return v_str_take(sb_take(&sb));
}

BUILTIN(fn_startswith) {
  UNUSED_KW;
  need_args("startswith", count, 2, 2, line);
  const char *text = need_str("startswith", args[0], line);
  char *prefix = to_text(args[1]);
  int result = strncmp(text, prefix, strlen(prefix)) == 0;
  free(prefix);
  return v_bool(result);
}

BUILTIN(fn_endswith) {
  UNUSED_KW;
  need_args("endswith", count, 2, 2, line);
  const char *text = need_str("endswith", args[0], line);
  char *suffix = to_text(args[1]);
  size_t a = strlen(text), b = strlen(suffix);
  int result = a >= b && strcmp(text + a - b, suffix) == 0;
  free(suffix);
  return v_bool(result);
}

BUILTIN(fn_find) {
  UNUSED_KW;
  need_args("find", count, 2, 2, line);
  if (args[0].type == T_LIST) {
    for (int i = 0;i < args[0].as.list->count;i++) if (values_equal(args[0].as.list->items[i], args[1])) return v_int(i);
    return v_int(-1);
  }
  const char *text = need_str("find", args[0], line);
  char *needle = to_text(args[1]);
  const char *found = strstr(text, needle);
  free(needle);
  return v_int(found ? found - text : -1);
}

BUILTIN(fn_count) {
  UNUSED_KW;
  need_args("count", count, 2, 2, line);
  long long total = 0;
  if (args[0].type == T_LIST) {
    for (int i = 0;i < args[0].as.list->count;i++) if (values_equal(args[0].as.list->items[i], args[1])) total++;
    return v_int(total);
  }
  const char *text = need_str("count", args[0], line);
  char *needle = to_text(args[1]);
  size_t length = strlen(needle);
  if (length > 0) for (const char *c = strstr(text, needle);c;c = strstr(c + length, needle)) total++;
  free(needle);
  return v_int(total);
}

BUILTIN(fn_append) {
  UNUSED_KW;
  need_args("append", count, 2, 2, line);
  list_push(need_list("append", args[0], line), incref(args[1]));
  return v_none();
}

BUILTIN(fn_extend) {
  UNUSED_KW;
  need_args("extend", count, 2, 2, line);
  List *target = need_list("extend", args[0], line);
  Value items = iteration_list(args[1], line);
  for (int i = 0;i < items.as.list->count;i++) list_push(target, incref(items.as.list->items[i]));
  decref(items);
  return v_none();
}

BUILTIN(fn_insert) {
  UNUSED_KW;
  need_args("insert", count, 3, 3, line);
  List *list = need_list("insert", args[0], line);
  long long at = as_index(args[1], line);
  if (at < 0) at += list->count;
  if (at < 0) at = 0;
  if (at > list->count) at = list->count;
  list_push(list, v_none());
  memmove(list->items + at + 1, list->items + at, (list->count - 1 - at) * sizeof(Value));
  list->items[at] = incref(args[2]);
  return v_none();
}

BUILTIN(fn_pop) {
  UNUSED_KW;
  need_args("pop", count, 1, 2, line);
  if (args[0].type == T_DICT) {
    Dict *d = args[0].as.dict;
    if (count < 2) fail(line, "pop() on a dict needs a key");
    for (int i = 0;i < d->count;i++) {
      if (values_equal(d->keys[i], args[1])) {
        Value result = d->values[i];
        decref(d->keys[i]);
        d->keys[i] = d->keys[d->count - 1];
        d->values[i] = d->values[d->count - 1];
        d->count--;
        return result;
      }
    }
    fail(line, "pop(): no such key");
  }
  List *list = need_list("pop", args[0], line);
  if (list->count == 0) fail(line, "pop() from an empty list");
  long long at = count > 1 ? as_index(args[1], line) : -1;
  if (at < 0) at += list->count;
  if (at < 0 || at >= list->count) fail(line, "pop() index out of range");
  Value result = list->items[at];
  memmove(list->items + at, list->items + at + 1, (list->count - at - 1) * sizeof(Value));
  list->count--;
  return result;
}

BUILTIN(fn_remove) {
  UNUSED_KW;
  need_args("remove", count, 2, 2, line);
  List *list = need_list("remove", args[0], line);
  for (int i = 0;i < list->count;i++) {
    if (values_equal(list->items[i], args[1])) {
      decref(list->items[i]);
      memmove(list->items + i, list->items + i + 1, (list->count - i - 1) * sizeof(Value));
      list->count--;
      return v_none();
    }
  }
  fail(line, "remove(): the item isn't in the list");
  return v_none();
}

static int sort_line;
static int sort_compare(const void *a, const void *b) {
  return compare_order(*(const Value *)a, *(const Value *)b, sort_line);
}

BUILTIN(fn_sorted) {
  need_args("sorted", count, 1, 1, line);
  Value items = iteration_list(args[0], line);
  Value copy = v_list();
  for (int i = 0;i < items.as.list->count;i++) list_push(copy.as.list, incref(items.as.list->items[i]));
  decref(items);
  sort_line = line;
  if (copy.as.list->count > 1) qsort(copy.as.list->items, copy.as.list->count, sizeof(Value), sort_compare);
  if (truthy(keyword("reverse", kw_names, kw_values, kw_count, v_bool(0)))) {
    for (int i = 0, j = copy.as.list->count - 1;i < j;i++, j--) {
      Value t = copy.as.list->items[i]; copy.as.list->items[i] = copy.as.list->items[j]; copy.as.list->items[j] = t;
    }
  }
  return copy;
}

BUILTIN(fn_reversed) {
  UNUSED_KW;
  need_args("reversed", count, 1, 1, line);
  if (args[0].type == T_STR) {
    Value result = v_str(args[0].as.str->text);
    for (size_t i = 0, j = result.as.str->length;j > 0 && i < j - 1;i++, j--) {
      char t = result.as.str->text[i]; result.as.str->text[i] = result.as.str->text[j - 1]; result.as.str->text[j - 1] = t;
    }
    return result;
  }
  Value items = iteration_list(args[0], line);
  Value copy = v_list();
  for (int i = items.as.list->count - 1;i >= 0;i--) list_push(copy.as.list, incref(items.as.list->items[i]));
  decref(items);
  return copy;
}

static Value extreme(const char *name, Value *args, int count, int line, int want_max) {
  Value pool = (count == 1) ? iteration_list(args[0], line) : v_list();
  if (count != 1) for (int i = 0;i < count;i++) list_push(pool.as.list, incref(args[i]));
  if (pool.as.list->count == 0) { decref(pool); fail(line, "%s() of nothing", name); }
  Value best = pool.as.list->items[0];
  for (int i = 1;i < pool.as.list->count;i++) {
    int c = compare_order(pool.as.list->items[i], best, line);
    if ((want_max && c > 0) || (!want_max && c < 0)) best = pool.as.list->items[i];
  }
  best = incref(best);
  decref(pool);
  return best;
}
BUILTIN(fn_min) { UNUSED_KW; need_args("min", count, 1, -1, line); return extreme("min", args, count, line, 0); }
BUILTIN(fn_max) { UNUSED_KW; need_args("max", count, 1, -1, line); return extreme("max", args, count, line, 1); }

BUILTIN(fn_sum) {
  UNUSED_KW;
  need_args("sum", count, 1, 1, line);
  Value items = iteration_list(args[0], line);
  Value total = v_int(0);
  for (int i = 0;i < items.as.list->count;i++) {
    Value next = binary(OP_ADD, total, items.as.list->items[i], line);
    decref(total);
    total = next;
  }
  decref(items);
  return total;
}

BUILTIN(fn_abs) {
  UNUSED_KW;
  need_args("abs", count, 1, 1, line);
  double d; int is_int; long long i;
  if (!as_number(args[0], &d, &is_int, &i)) fail(line, "abs() needs a number");
  return is_int ? v_int(i < 0 ? -i : i) : v_float(fabs(d));
}

BUILTIN(fn_round) {
  UNUSED_KW;
  need_args("round", count, 1, 2, line);
  double d; int is_int; long long i;
  if (!as_number(args[0], &d, &is_int, &i)) fail(line, "round() needs a number");
  if (count == 1) return v_int(llround(d));
  double scale = pow(10, (double)as_index(args[1], line));
  return v_float(round(d * scale) / scale);
}

BUILTIN(fn_keys) {
  UNUSED_KW;
  need_args("keys", count, 1, 1, line);
  if (args[0].type != T_DICT) fail(line, "keys() needs a dict");
  return iteration_list(args[0], line);
}

BUILTIN(fn_values) {
  UNUSED_KW;
  need_args("values", count, 1, 1, line);
  if (args[0].type != T_DICT) fail(line, "values() needs a dict");
  Value list = v_list();
  for (int i = 0;i < args[0].as.dict->count;i++) list_push(list.as.list, incref(args[0].as.dict->values[i]));
  return list;
}

BUILTIN(fn_items) {
  UNUSED_KW;
  need_args("items", count, 1, 1, line);
  if (args[0].type != T_DICT) fail(line, "items() needs a dict");
  Value list = v_list();
  for (int i = 0;i < args[0].as.dict->count;i++) {
    Value pair = v_list();
    list_push(pair.as.list, incref(args[0].as.dict->keys[i]));
    list_push(pair.as.list, incref(args[0].as.dict->values[i]));
    list_push(list.as.list, pair);
  }
  return list;
}

BUILTIN(fn_get) {
  UNUSED_KW;
  need_args("get", count, 2, 3, line);
  if (args[0].type != T_DICT) fail(line, "get() needs a dict");
  for (int i = 0;i < args[0].as.dict->count;i++) {
    if (values_equal(args[0].as.dict->keys[i], args[1])) return incref(args[0].as.dict->values[i]);
  }
  return count > 2 ? incref(args[2]) : v_none();
}

BUILTIN(fn_glob) {
  UNUSED_KW;
  need_args("glob", count, 1, 1, line);
  glob_t matches;
  Value list = v_list();
  if (glob(need_str("glob", args[0], line), 0, NULL, &matches) == 0) {
    for (size_t i = 0;i < matches.gl_pathc;i++) list_push(list.as.list, v_str(matches.gl_pathv[i]));
    globfree(&matches);
  }
  return list;
}

static int stat_mode(Value *args, int count, const char *name, int line, struct stat *info) {
  need_args(name, count, 1, 1, line);
  char *path = to_text(args[0]);
  int ok = stat(path, info) == 0;
  free(path);
  return ok;
}
BUILTIN(fn_exists) { UNUSED_KW; struct stat s; return v_bool(stat_mode(args, count, "exists", line, &s)); }
BUILTIN(fn_isdir) { UNUSED_KW; struct stat s; return v_bool(stat_mode(args, count, "isdir", line, &s) && S_ISDIR(s.st_mode)); }
BUILTIN(fn_isfile) { UNUSED_KW; struct stat s; return v_bool(stat_mode(args, count, "isfile", line, &s) && S_ISREG(s.st_mode)); }
BUILTIN(fn_size) {
  UNUSED_KW;
  struct stat s;
  if (!stat_mode(args, count, "size", line, &s)) fail(line, "size(): no such file");
  return v_int((long long)s.st_size);
}

BUILTIN(fn_basename) {
  UNUSED_KW;
  need_args("basename", count, 1, 1, line);
  char *path = to_text(args[0]);
  size_t length = strlen(path);
  while (length > 1 && path[length - 1] == '/') path[--length] = '\0';
  char *slash = strrchr(path, '/');
  Value result = v_str(slash && slash[1] ? slash + 1 : path);
  free(path);
  return result;
}

BUILTIN(fn_dirname) {
  UNUSED_KW;
  need_args("dirname", count, 1, 1, line);
  char *path = to_text(args[0]);
  char *slash = strrchr(path, '/');
  Value result = !slash ? v_str(".") : slash == path ? v_str("/") : v_strn(path, slash - path);
  free(path);
  return result;
}

BUILTIN(fn_abspath) {
  UNUSED_KW;
  need_args("abspath", count, 1, 1, line);
  char *path = to_text(args[0]);
  char resolved[PATH_MAX];
  Value result = realpath(path, resolved) ? v_str(resolved) : v_str(path);
  free(path);
  return result;
}

BUILTIN(fn_read) {
  UNUSED_KW;
  need_args("read", count, 1, 1, line);
  char *path = to_text(args[0]);
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    char message[512];
    snprintf(message, sizeof(message), "read(): %s: %s", path, strerror(errno));
    free(path);
    fail(line, "%s", message);
  }
  free(path);
  SB sb = { 0 };
  sb_add(&sb, "");
  char chunk[4096];
  size_t got;
  while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) sb_addn(&sb, chunk, got);
  fclose(file);
  return v_str_take(sb_take(&sb));
}

BUILTIN(fn_write) {
  need_args("write", count, 2, 2, line);
  char *path = to_text(args[0]);
  char *text = to_text(args[1]);
  int append_mode = truthy(keyword("append", kw_names, kw_values, kw_count, v_bool(0)));
  FILE *file = fopen(path, append_mode ? "a" : "w");
  if (file == NULL) {
    char message[512];
    snprintf(message, sizeof(message), "write(): %s: %s", path, strerror(errno));
    free(path); free(text);
    fail(line, "%s", message);
  }
  fputs(text, file);
  fclose(file);
  free(path);
  free(text);
  return v_none();
}

BUILTIN(fn_env) {
  UNUSED_KW;
  need_args("env", count, 1, 2, line);
  char *name = to_text(args[0]);
  const char *value = getenv(name);
  free(name);
  if (value) return v_str(value);
  return count > 1 ? incref(args[1]) : v_str("");
}

BUILTIN(fn_cwd) { UNUSED_KW; (void)args; need_args("cwd", count, 0, 0, line); char buffer[PATH_MAX]; return v_str(getcwd(buffer, sizeof(buffer)) ? buffer : "."); }

BUILTIN(fn_time) {
  (void)args;
  UNUSED_KW;
  need_args("time", count, 0, 0, line);
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return v_float(now.tv_sec + now.tv_nsec / 1e9);
}

BUILTIN(fn_sleep) {
  UNUSED_KW;
  need_args("sleep", count, 1, 1, line);
  double d; int is_int; long long i;
  if (!as_number(args[0], &d, &is_int, &i)) fail(line, "sleep() needs seconds");
  struct timespec delay = { (time_t)d, (long)((d - (time_t)d) * 1e9) };
  while (nanosleep(&delay, &delay) != 0 && errno == EINTR && !script_interrupted) {}
  return v_none();
}

BUILTIN(fn_random) {
  UNUSED_KW;
  need_args("random", count, 0, 2, line);
  static int seeded = 0;
  if (!seeded) { srandom((unsigned)time(NULL) ^ (unsigned)getpid()); seeded = 1; }
  if (count == 0) return v_float(random() / ((double)RAND_MAX + 1));
  long long low = count == 2 ? as_index(args[0], line) : 0;
  long long high = as_index(args[count - 1], line);
  if (high < low) fail(line, "random(a, b) needs a <= b");
  return v_int(low + random() % (high - low + 1));
}

BUILTIN(fn_exit) {
  UNUSED_KW;
  need_args("exit", count, 0, 1, line);
  int code = count ? (int)as_index(args[0], line) : 0;
  expand_set_last_status(code);
  last_statement_status = code;
  exec_request_exit();
  return v_none();
}

static const struct {
  const char *name;
  BuiltinFn fn;
}builtin_functions[] = {
  { "print", fn_print }, { "input", fn_input }, { "len", fn_len }, { "str", fn_str },
  { "int", fn_int }, { "float", fn_float }, { "bool", fn_bool }, { "type", fn_type },
  { "range", fn_range }, { "list", fn_list }, { "words", fn_words }, { "chars", fn_chars },
  { "lines", fn_lines }, { "split", fn_split }, { "join", fn_join }, { "strip", fn_strip },
  { "lstrip", fn_lstrip }, { "rstrip", fn_rstrip }, { "upper", fn_upper }, { "lower", fn_lower },
  { "replace", fn_replace }, { "startswith", fn_startswith }, { "endswith", fn_endswith },
  { "find", fn_find }, { "count", fn_count }, { "append", fn_append }, { "extend", fn_extend },
  { "insert", fn_insert }, { "pop", fn_pop }, { "remove", fn_remove }, { "sorted", fn_sorted },
  { "reversed", fn_reversed }, { "min", fn_min }, { "max", fn_max }, { "sum", fn_sum },
  { "abs", fn_abs }, { "round", fn_round }, { "keys", fn_keys }, { "values", fn_values },
  { "items", fn_items }, { "get", fn_get }, { "glob", fn_glob }, { "exists", fn_exists },
  { "isdir", fn_isdir }, { "isfile", fn_isfile }, { "size", fn_size }, { "basename", fn_basename },
  { "dirname", fn_dirname }, { "abspath", fn_abspath }, { "read", fn_read }, { "write", fn_write },
  { "env", fn_env }, { "cwd", fn_cwd }, { "time", fn_time }, { "sleep", fn_sleep },
  { "random", fn_random }, { "exit", fn_exit }, { NULL, NULL }
};

static int builtin_function_index_n(const char *name, size_t length) {
  for (int i = 0;builtin_functions[i].name != NULL;i++) {
    if (strlen(builtin_functions[i].name) == length && strncmp(builtin_functions[i].name, name, length) == 0) return i;
  }
  return -1;
}

static Value call_builtin(int index, Value *args, int count, char **kw_names, Value *kw_values, int kw_count, int line) {
  return builtin_functions[index].fn(args, count, kw_names, kw_values, kw_count, line);
}

int script_builtin_exists(const char *name) {
  return builtin_function_index(name) >= 0;
}

// ===========================================================================
// Running
// ===========================================================================

static Block **kept_programs = NULL;
static int kept_count = 0;
static int run_depth = 0;

static void on_interrupt(int signal_number) {
  (void)signal_number;
  script_interrupted = 1;
}

int script_run_source(const char *source, const char *origin, int first_line, int repl) {
  jmp_buf jump;
  jmp_buf *previous_jump = error_jump;
  const char *previous_origin = current_origin;
  int previous_offset = line_offset;
  int previous_repl = repl_mode;
  int previous_keep = keep_program;
  Frame *previous_frame = frame;

  Line *volatile lines = NULL;
  volatile int count = 0;
  Block *volatile program = NULL;
  volatile int status = 0;

  struct sigaction previous_action;
  int handle_interrupt = (run_depth == 0 && jobs_interactive());
  if (handle_interrupt) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_interrupt;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, &previous_action);
    script_interrupted = 0;
  }
  run_depth++;

  error_jump = &jump;
  current_origin = origin;
  line_offset = first_line - 1;
  repl_mode = repl;
  last_run_failed = 0;
  keep_program = 0;
  last_statement_status = 0;

  if (setjmp(jump) == 0) {
    int line_count = 0;
    lines = split_lines(source, &line_count);
    count = line_count;
    // `for x in items` followed by an indented block: a forgotten colon,
    // not a shell command.
    for (int k = 0;k + 1 < line_count;k++) {
      Line *here = (Line *)&lines[k];
      if (here->is_script || lines[k + 1].indent <= here->indent) continue;
      size_t word = 0;
      while (is_name_char(here->text[word])) word++;
      if (word > 0 && word_in(here->text, word, block_keywords) &&
          strstr(here->text, "; then") == NULL && strstr(here->text, "; do") == NULL &&
          strcmp(here->text + strlen(here->text) - (strlen(here->text) >= 2 ? 2 : 0), "do") != 0 &&
          strstr(here->text, " then") == NULL) {
        fail(here->line, "add a ':' at the end of this line to start the indented block");
      }
    }
    int index = 0;
    int base = line_count > 0 ? lines[0].indent : 0;
    program = parse_block((Line *)lines, &index, line_count, base);
    if (index < line_count) {
      fail(lines[index].line, "this line's indentation doesn't match any block above it");
    }
    Flow flow = exec_block((Block *)program);
    (void)flow;
    status = last_statement_status;
  }
  else {
    status = 1;
    frame = previous_frame;
    expand_set_last_status(1);
    last_run_failed = 1;
  }

  for (int i = 0;i < count;i++) free(lines[i].text);
  free((Line *)lines);
  if (program != NULL) {
    if (keep_program) {
      kept_programs = checked(realloc(kept_programs, (kept_count + 1) * sizeof(Block *)));
      kept_programs[kept_count++] = (Block *)program;
    }
    else {
      free_block((Block *)program);
    }
  }

  run_depth--;
  if (handle_interrupt) {
    sigaction(SIGINT, &previous_action, NULL);
  }
  error_jump = previous_jump;
  current_origin = previous_origin;
  line_offset = previous_offset;
  repl_mode = previous_repl;
  keep_program = previous_keep || keep_program;
  if (status == 0 && exec_should_exit()) {
    status = expand_last_status();
  }
  return status;
}

static Func *find_function(const char *name) {
  int index = scope_find(&globals, name, strlen(name));
  if (index >= 0 && globals.values[index].type == T_FUNC) return globals.values[index].as.func;
  return NULL;
}

int script_missing_colon(const char *text, const char *origin, int first_line) {
  const char *newline = strchr(text, '\n');
  if (newline == NULL) return 0;
  size_t word = 0;
  while (is_name_char(text[word])) word++;
  if (word == 0 || !word_in(text, word, block_keywords)) return 0;
  char *first = strndup(text, newline - text);
  int bash_style = strstr(first, "then") != NULL || strstr(first, " do") != NULL || strstr(first, ";do") != NULL ||
                   strncmp(newline + 1 + strspn(newline + 1, " \t"), "do", 2) == 0 ||
                   strncmp(newline + 1 + strspn(newline + 1, " \t"), "then", 4) == 0;
  // fish blocks end with `end`; bash ones with fi/done/esac.
  int fish_style = strstr(text, "\nend") != NULL || strstr(text, "; end") != NULL;
  free(first);
  int indented = newline[1] == ' ' || newline[1] == '\t';
  if (bash_style || fish_style || !indented) return 0;
  fprintf(stderr, "tarsh: %s, line %d: add a ':' at the end of this line to start the indented block\n",
          origin, first_line);
  return 1;
}

int script_last_failed(void) {
  return last_run_failed;
}

int script_function_exists(const char *name) {
  return find_function(name) != NULL;
}

int script_call_command(ArgList *argv) {
  Func *f = find_function(argv->items[0]);
  if (f == NULL) return 127;

  jmp_buf jump;
  jmp_buf *previous_jump = error_jump;
  const char *previous_origin = current_origin;
  int previous_offset = line_offset;
  Frame *previous_frame = frame;
  volatile int status = 0;
  int count = argv->count - 1;
  Value *volatile args = checked(calloc(count > 0 ? count : 1, sizeof(Value)));
  for (int i = 0;i < count;i++) args[i] = v_str(argv->items[i + 1]);

  error_jump = &jump;
  current_origin = f->name;
  line_offset = 0;
  f->refs++;
  if (setjmp(jump) == 0) {
    last_statement_status = 0;
    Value result = call_user(f, (Value *)args, count, NULL, NULL, 0, 0, 1);
    // Functions talk to the shell like commands: text is printed,
    // numbers and booleans become the exit status.
    if (result.type == T_BOOL) status = !result.as.boolean;
    else if (result.type == T_INT) status = (int)result.as.integer;
    else if (result.type == T_NONE) status = last_statement_status;
    else {
      char *text = to_text(result);
      if (result.type == T_LIST) {
        for (int i = 0;i < result.as.list->count;i++) {
          char *item = to_text(result.as.list->items[i]);
          printf("%s\n", item);
          free(item);
        }
      }
      else if (text[0]) {
        printf("%s\n", text);
      }
      free(text);
      fflush(stdout);
    }
    decref(result);
  }
  else {
    status = 1;
    frame = previous_frame;
  }
  Value function_value;
  function_value.type = T_FUNC;
  function_value.as.func = f;
  decref(function_value);
  for (int i = 0;i < count;i++) decref(args[i]);
  free((Value *)args);
  error_jump = previous_jump;
  current_origin = previous_origin;
  line_offset = previous_offset;
  return status;
}

char *script_function_source(const char *name) {
  Func *f = find_function(name);
  return f ? strdup(f->source) : NULL;
}

void script_function_names(ArgList *out) {
  for (int i = 0;i < globals.count;i++) {
    if (globals.values[i].type == T_FUNC) arglist_append(out, globals.names[i]);
  }
}

int script_function_erase(const char *name) {
  if (find_function(name) == NULL) return 1;
  scope_remove(&globals, name);
  return 0;
}

// ===========================================================================
// Shell variables
// ===========================================================================

int script_var_get(const char *name, char **string, ArgList *list) {
  size_t length = strlen(name);
  Value *found = NULL;
  if (frame != NULL) {
    int index = scope_find(&frame->locals, name, length);
    if (index >= 0) found = &frame->locals.values[index];
  }
  if (found == NULL) {
    int index = scope_find(&globals, name, length);
    if (index >= 0) found = &globals.values[index];
  }
  if (found == NULL) return 0;
  if (found->type == T_FUNC) return 0;
  if (found->type == T_LIST && list != NULL) {
    for (int i = 0;i < found->as.list->count;i++) {
      char *text = to_text(found->as.list->items[i]);
      arglist_append(list, text);
      free(text);
    }
    return 2;
  }
  if (found->type == T_LIST) {
    SB sb = { 0 };
    sb_add(&sb, "");
    for (int i = 0;i < found->as.list->count;i++) {
      char *text = to_text(found->as.list->items[i]);
      if (i) sb_addc(&sb, ' ');
      sb_add(&sb, text);
      free(text);
    }
    *string = sb_take(&sb);
    return 1;
  }
  *string = to_text(*found);
  return 1;
}

void script_var_set(const char *name, const char *value) {
  scope_set(&globals, name, v_str(value));
}

void script_var_set_list(const char *name, ArgList *values) {
  Value list = v_list();
  for (int i = 0;i < values->count;i++) list_push(list.as.list, v_str(values->items[i]));
  scope_set(&globals, name, list);
}

int script_var_unset(const char *name) {
  return scope_remove(&globals, name);
}

int script_var_exists(const char *name) {
  return scope_find(&globals, name, strlen(name)) >= 0 &&
         globals.values[scope_find(&globals, name, strlen(name))].type != T_FUNC;
}

void script_var_print_all(void) {
  for (int i = 0;i < globals.count;i++) {
    if (globals.values[i].type == T_FUNC) continue;
    char *text = to_repr(globals.values[i]);
    printf("%s %s\n", globals.names[i], text);
    free(text);
  }
}

int script_math(const char *expression, char **result) {
  jmp_buf jump;
  jmp_buf *previous_jump = error_jump;
  const char *previous_origin = current_origin;
  int previous_offset = line_offset;
  volatile int status = 0;
  Expr *volatile e = NULL;

  error_jump = &jump;
  current_origin = "math";
  line_offset = 0;
  if (setjmp(jump) == 0) {
    e = parse_expression_text(expression, 0);
    Value v = eval((Expr *)e);
    *result = to_text(v);
    decref(v);
  }
  else {
    status = 1;
  }
  free_expr((Expr *)e);
  error_jump = previous_jump;
  current_origin = previous_origin;
  line_offset = previous_offset;
  return status;
}
