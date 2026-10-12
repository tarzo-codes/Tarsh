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

// Options that make an otherwise read-only command write or run things.
static const char *dangerous_option(const char *command, const char *word) {
  if (!strcmp(command, "sed") && (!strncmp(word, "-i", 2) || !strncmp(word, "--in-place", 10))) return "sed -i edits files";
  if (!strcmp(command, "sort") && (!strncmp(word, "-o", 2) || !strncmp(word, "--output", 8))) return "sort -o writes a file";
  if (!strcmp(command, "find") && (!strcmp(word, "-delete") || !strncmp(word, "-exec", 5) || !strncmp(word, "-ok", 3) ||
                                   !strncmp(word, "-fprint", 7) || !strcmp(word, "-fls"))) return "find would act on files";
  if ((!strcmp(command, "awk") || !strcmp(command, "gawk")) && (!strcmp(word, "-i") || !strncmp(word, "--include", 9))) return "awk -i";
  return NULL;
}

static void say(char *why, size_t size, const char *text) {
  if (why && size) snprintf(why, size, "%s", text);
}

int live_eligible(const char *text, char *why, size_t why_size) {
  if (!enabled) { say(why, why_size, "live preview is off"); return 0; }
  if (text[strspn(text, " \t")] == '\0') { say(why, why_size, "nothing typed"); return 0; }
  char *line = alias_expand_line(text);
  int ok = 0;

  if (script_claims(line) || lang_needs_backend(line)) { say(why, why_size, "not a plain pipeline"); goto done; }
  // Only one pipeline, nothing that runs other commands.
  for (const char *c = line;*c;c++) {
    if (*c == '`' || (*c == '$' && c[1] == '(')) { say(why, why_size, "$(...) isn't previewed"); goto done; }
  }

  CommandList list;
  char error[256];
  if (syntax_parse(line, &list, error, sizeof(error)) != 0) { say(why, why_size, "unfinished"); goto done; }
  if (list.count != 1 || list.connectors[0] != CONNECT_SEQ) {
    say(why, why_size, "only single pipelines are previewed");
    syntax_free(&list);
    goto done;
  }
  Pipeline *pipeline = &list.pipelines[0];
  ok = 1;
  for (int i = 0;i < pipeline->count && ok;i++) {
    Command *command = &pipeline->commands[i];
    if (command->words.count == 0) { ok = 0; say(why, why_size, "empty command"); break; }
    for (int r = 0;r < command->redir_count;r++) {
      if (command->redirs[r].kind != REDIR_IN) { ok = 0; say(why, why_size, "lines that write files aren't previewed"); break; }
    }
    if (!ok) break;
    ArgList words;
    arglist_init(&words);
    parser_split_words(command->words.items[0], &words);
    const char *name = words.count ? words.items[0] : "";
    if (!listed(name)) {
      char message[160];
      snprintf(message, sizeof(message), "%s isn't in LIVE_COMMANDS", name);
      say(why, why_size, message);
      ok = 0;
    }
    for (int k = 1;ok && k < command->words.count;k++) {
      ArgList arg;
      arglist_init(&arg);
      parser_split_words(command->words.items[k], &arg);
      const char *danger = arg.count ? dangerous_option(name, arg.items[0]) : NULL;
      if (danger) { say(why, why_size, danger); ok = 0; }
      arglist_free(&arg);
    }
    arglist_free(&words);
  }
  syntax_free(&list);

done:
  free(line);
  return ok;
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

  // grep shows what matched, in color.
  char *preview_line = NULL;
  {
    ArgList words;
    arglist_init(&words);
    parser_split_words(line, &words);
    size_t length = strlen(line) + 64;
    preview_line = malloc(length);
    snprintf(preview_line, length, "%s", line);
    arglist_free(&words);
  }

  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
    free(preview_line);
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
    setenv("GREP_COLORS", "mt=01;33", 1);
    setenv("TMPDIR", "/nonexistent-tarsh-preview", 1);
    if (install_sandbox() != 0) {
      fprintf(stderr, "live preview needs seccomp, which this system doesn't offer\n");
      _exit(125);
    }
    // Inside the sandbox now: run the pipeline with tarsh itself.
    // grep --color makes matches stand out.
    char *colored = NULL;
    {
      // Insert --color=always after each grep.
      const char *greps[] = { "grep ", "egrep ", "fgrep ", "rg ", NULL };
      size_t length = strlen(preview_line) * 2 + 64;
      colored = malloc(length);
      size_t used = 0;
      const char *c = preview_line;
      // Only the last command's matches are colored: colors in the middle
      // of a pipe would end up inside the data (cut, sort, awk...).
      const char *last_pipe = NULL;
      {
        char quote = 0;
        for (const char *k = preview_line;*k;k++) {
          if (quote) { if (*k == '\\' && quote == '"' && k[1]) { k++; continue; } if (*k == quote) quote = 0; continue; }
          if (*k == '\'' || *k == '"') quote = *k;
          else if (*k == '|') last_pipe = k;
        }
      }
      int command_start = 1;
      while (*c && used < length - 32) {
        if (command_start) {
          while (*c == ' ') colored[used++] = *c++;
          for (int k = 0;greps[k] && (last_pipe == NULL || c > last_pipe);k++) {
            size_t n = strlen(greps[k]);
            if (!strncmp(c, greps[k], n)) {
              memcpy(colored + used, c, n);
              used += n;
              c += n;
              used += sprintf(colored + used, "--color=always ");
              break;
            }
          }
          command_start = 0;
          continue;
        }
        if (*c == '|') command_start = 1;
        if (*c == '\'' || *c == '"') {
          char quote = *c;
          colored[used++] = *c++;
          while (*c && *c != quote && used < length - 32) { if (*c == '\\' && quote == '"' && c[1]) colored[used++] = *c++; colored[used++] = *c++; }
          if (*c) colored[used++] = *c++;
          continue;
        }
        colored[used++] = *c++;
      }
      colored[used] = '\0';
    }
    int status = exec_run_line(colored);
    fflush(stdout);
    fflush(stderr);
    _exit(status);
  }
  free(preview_line);
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
    return errors;
  }
  free(errors);
  if (output_length == 0 && timed_out) {
    free(output);
    *failed = 1;
    return strdup("(still running after a second; preview stopped)");
  }
  sanitize(output);
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
