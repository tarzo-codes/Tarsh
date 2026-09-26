#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/wait.h>

#include "jobs.h"
#include "builtins.h"
#include "expand.h"
#include "suggest.h"

#define MAX_JOBS 64

typedef enum {
  PROC_RUNNING,
  PROC_STOPPED,
  PROC_DONE
}ProcState;

typedef struct {
  int id;
  pid_t pgid;
  pid_t *pids;
  ProcState *states;
  int *statuses;
  int count;
  char *text;
  struct termios tmodes;
  int has_tmodes;
  int background;
  int notified;
  unsigned long order;   // when it was last started or stopped
}Job;

static Job *job_table[MAX_JOBS];
static unsigned long job_clock = 0;

static int interactive = 0;
static pid_t shell_pgid = 0;
static struct termios shell_tmodes;
static int have_shell_tmodes = 0;

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void jobs_init(int want_interactive) {
  interactive = want_interactive;
  if (!interactive) {
    return;
  }

  // If we were started in the background, wait until we're in front.
  while (tcgetpgrp(STDIN_FILENO) != (shell_pgid = getpgrp())) {
    kill(-shell_pgid, SIGTTIN);
  }

  // The shell ignores the job-control signals; its children won't.
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);

  // Lead our own process group and take the terminal.
  shell_pgid = getpid();
  if (setpgid(shell_pgid, shell_pgid) < 0 && errno != EPERM) {
    perror("tarsh: couldn't put the shell in its own process group");
  }
  shell_pgid = getpgrp();
  tcsetpgrp(STDIN_FILENO, shell_pgid);
  if (tcgetattr(STDIN_FILENO, &shell_tmodes) == 0) {
    have_shell_tmodes = 1;
  }
}

int jobs_interactive(void) {
  return interactive;
}

static void reset_child_signals(void) {
  signal(SIGINT, SIG_DFL);
  signal(SIGQUIT, SIG_DFL);
  signal(SIGTSTP, SIG_DFL);
  signal(SIGTTIN, SIG_DFL);
  signal(SIGTTOU, SIG_DFL);
  signal(SIGCHLD, SIG_DFL);
}

void jobs_become_subshell(void) {
  interactive = 0;
  reset_child_signals();
  // The subshell's jobs belong to the parent shell; forget them here.
  for (int i = 0;i < MAX_JOBS;i++) {
    job_table[i] = NULL;
  }
}

// ---------------------------------------------------------------------------
// Job table
// ---------------------------------------------------------------------------

static Job *job_new(int count, const char *text) {
  int slot = -1;
  for (int i = 0;i < MAX_JOBS;i++) {
    if (job_table[i] == NULL) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    fprintf(stderr, "tarsh: too many jobs\n");
    return NULL;
  }

  Job *job = calloc(1, sizeof(Job));
  if (job == NULL) {
    return NULL;
  }
  job->pids = calloc(count, sizeof(pid_t));
  job->states = calloc(count, sizeof(ProcState));
  job->statuses = calloc(count, sizeof(int));
  job->text = strdup(text ? text : "");
  job->count = count;
  job->id = slot + 1;
  job->order = ++job_clock;
  job_table[slot] = job;
  return job;
}

static void job_free(Job *job) {
  job_table[job->id - 1] = NULL;
  free(job->pids);
  free(job->states);
  free(job->statuses);
  free(job->text);
  free(job);
}

static int job_done(const Job *job) {
  for (int i = 0;i < job->count;i++) {
    if (job->states[i] != PROC_DONE) {
      return 0;
    }
  }
  return 1;
}

static int job_stopped(const Job *job) {
  int any_stopped = 0;
  for (int i = 0;i < job->count;i++) {
    if (job->states[i] == PROC_RUNNING) {
      return 0;
    }
    if (job->states[i] == PROC_STOPPED) {
      any_stopped = 1;
    }
  }
  return any_stopped;
}

static void mark_status(pid_t pid, int status) {
  for (int j = 0;j < MAX_JOBS;j++) {
    Job *job = job_table[j];
    if (job == NULL) {
      continue;
    }
    for (int i = 0;i < job->count;i++) {
      if (job->pids[i] != pid) {
        continue;
      }
      if (WIFSTOPPED(status)) {
        job->states[i] = PROC_STOPPED;
        job->notified = 0;
        job->order = ++job_clock;
      }
      else {
        job->states[i] = PROC_DONE;
        job->statuses[i] = status;
      }
      return;
    }
  }
}

// The job `fg`/`bg` pick with no argument: the most recently stopped or
// started one (marked + by `jobs`).
static Job *current_job(void) {
  Job *best = NULL;
  for (int i = 0;i < MAX_JOBS;i++) {
    Job *job = job_table[i];
    if (job == NULL) {
      continue;
    }
    if (best == NULL ||
        (job_stopped(job) && !job_stopped(best)) ||
        (job_stopped(job) == job_stopped(best) && job->order > best->order)) {
      best = job;
    }
  }
  return best;
}

static int exit_code(int status) {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return status;
}

static void signal_job(Job *job, int signal_number) {
  if (interactive && job->pgid > 0) {
    kill(-job->pgid, signal_number);
    return;
  }
  for (int i = 0;i < job->count;i++) {
    if (job->states[i] != PROC_DONE) {
      kill(job->pids[i], signal_number);
    }
  }
}

// ---------------------------------------------------------------------------
// Redirections
// ---------------------------------------------------------------------------

static int remember_fd(int fd, SavedFd *saved, int *saved_count) {
  if (saved == NULL) {
    return 0;
  }
  for (int i = 0;i < *saved_count;i++) {
    if (saved[i].fd == fd) {
      return 0;
    }
  }
  if (*saved_count >= MAX_SAVED_FDS) {
    return -1;
  }
  saved[*saved_count].fd = fd;
  saved[*saved_count].copy = fcntl(fd, F_DUPFD_CLOEXEC, 10);
  (*saved_count)++;
  return 0;
}

static int open_onto(const char *path, int flags, int fd) {
  int opened = open(path, flags, 0666);
  if (opened < 0) {
    fprintf(stderr, "tarsh: %s: %s\n", path, strerror(errno));
    return -1;
  }
  if (opened != fd) {
    dup2(opened, fd);
    close(opened);
  }
  return 0;
}

int jobs_apply_redirects(const ExpandedRedirect *redirs, int count,
                         SavedFd *saved, int *saved_count) {
  for (int i = 0;i < count;i++) {
    const ExpandedRedirect *r = &redirs[i];
    int both = (r->kind == REDIR_BOTH || r->kind == REDIR_BOTH_APPEND);

    if (remember_fd(r->fd, saved, saved_count) != 0 ||
        (both && remember_fd(STDERR_FILENO, saved, saved_count) != 0)) {
      fprintf(stderr, "tarsh: too many redirections\n");
      return -1;
    }

    switch (r->kind) {
      case REDIR_IN:
        if (open_onto(r->path, O_RDONLY, r->fd) != 0) return -1;
        break;
      case REDIR_OUT:
        if (open_onto(r->path, O_WRONLY | O_CREAT | O_TRUNC, r->fd) != 0) return -1;
        break;
      case REDIR_APPEND:
        if (open_onto(r->path, O_WRONLY | O_CREAT | O_APPEND, r->fd) != 0) return -1;
        break;
      case REDIR_BOTH:
      case REDIR_BOTH_APPEND: {
        int flags = O_WRONLY | O_CREAT | (r->kind == REDIR_BOTH ? O_TRUNC : O_APPEND);
        if (open_onto(r->path, flags, STDOUT_FILENO) != 0) return -1;
        dup2(STDOUT_FILENO, STDERR_FILENO);
        break;
      }
      case REDIR_DUP:
        if (fcntl(r->target_fd, F_GETFD) < 0) {
          fprintf(stderr, "tarsh: %d: bad file descriptor\n", r->target_fd);
          return -1;
        }
        dup2(r->target_fd, r->fd);
        break;
      case REDIR_CLOSE:
        close(r->fd);
        break;
    }
  }
  return 0;
}

void jobs_restore_fds(SavedFd *saved, int saved_count) {
  for (int i = saved_count - 1;i >= 0;i--) {
    if (saved[i].copy >= 0) {
      dup2(saved[i].copy, saved[i].fd);
      close(saved[i].copy);
    }
    else {
      close(saved[i].fd);
    }
  }
}

// ---------------------------------------------------------------------------
// Launching
// ---------------------------------------------------------------------------

static void child_run(ExpandedCommand *command, pid_t pgid, int background, int in_fd, int out_fd) {
  if (interactive) {
    pid_t pid = getpid();
    if (pgid == 0) {
      pgid = pid;
    }
    setpgid(pid, pgid);
    if (!background) {
      tcsetpgrp(STDIN_FILENO, pgid);
    }
  }
  reset_child_signals();
  if (!interactive && background) {
    // Without job control, Ctrl+C must not reach background commands.
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
  }

  if (in_fd != STDIN_FILENO) {
    dup2(in_fd, STDIN_FILENO);
    close(in_fd);
  }
  if (out_fd != STDOUT_FILENO) {
    dup2(out_fd, STDOUT_FILENO);
    close(out_fd);
  }

  for (int i = 0;i < command->assignments.count;i++) {
    putenv(strdup(command->assignments.items[i]));
  }

  if (jobs_apply_redirects(command->redirs, command->redir_count, NULL, NULL) != 0) {
    _exit(1);
  }

  if (command->argv.count == 0) {
    _exit(0);
  }

  char **argv = command->argv.items;
  if (strcmp(argv[0], "exec") == 0 && command->argv.count > 1) {
    argv++;
  }
  else if (builtin_exists(argv[0])) {
    int status = 0;
    int should_exit = 0;
    builtin_run(&command->argv, &status, &should_exit);
    fflush(stdout);
    fflush(stderr);
    _exit(status);
  }

  execvp(argv[0], argv);

  int saved_errno = errno;
  if (saved_errno == ENOENT && strchr(argv[0], '/') == NULL) {
    suggest_command_not_found(argv[0]);
    _exit(127);
  }
  fprintf(stderr, "tarsh: %s: %s\n", argv[0], strerror(saved_errno));
  _exit(saved_errno == ENOENT ? 127 : 126);
}

static void wait_for_job(Job *job) {
  while (!job_done(job) && !job_stopped(job)) {
    int status;
    pid_t pid = waitpid(-1, &status, WUNTRACED);
    if (pid < 0) {
      if (errno == EINTR) {
        continue;
      }
      // Nothing left to wait for; don't spin.
      for (int i = 0;i < job->count;i++) {
        job->states[i] = PROC_DONE;
      }
      break;
    }
    mark_status(pid, status);
  }
}

static void describe_death(int status) {
  if (!WIFSIGNALED(status)) {
    return;
  }
  int signal_number = WTERMSIG(status);
  if (signal_number == SIGINT) {
    // Ctrl+C leaves the cursor mid-line.
    printf("\n");
  }
  else if (signal_number != SIGPIPE) {
    fprintf(stderr, "%s%s\n", strsignal(signal_number),
            WCOREDUMP(status) ? " (core dumped)" : "");
  }
}

static int last_stopped = 0;

static int put_in_foreground(Job *job, int resume) {
  last_stopped = 0;
  if (interactive) {
    tcsetpgrp(STDIN_FILENO, job->pgid);
    if (resume && job->has_tmodes) {
      tcsetattr(STDIN_FILENO, TCSADRAIN, &job->tmodes);
    }
  }
  if (resume) {
    for (int i = 0;i < job->count;i++) {
      if (job->states[i] == PROC_STOPPED) {
        job->states[i] = PROC_RUNNING;
      }
    }
    signal_job(job, SIGCONT);
  }
  job->background = 0;

  wait_for_job(job);

  if (interactive) {
    tcsetpgrp(STDIN_FILENO, shell_pgid);
    if (tcgetattr(STDIN_FILENO, &job->tmodes) == 0) {
      job->has_tmodes = 1;
    }
    if (have_shell_tmodes) {
      tcsetattr(STDIN_FILENO, TCSADRAIN, &shell_tmodes);
    }
  }

  if (job_stopped(job)) {
    job->background = 1;
    job->notified = 1;
    last_stopped = 1;
    printf("\n[%d]+  Stopped                 %s\n", job->id, job->text);
    printf("      (type fg to bring it back, bg to keep it running in the background)\n");
    return 128 + SIGTSTP;
  }

  int status = job->statuses[job->count - 1];
  describe_death(status);
  int code = exit_code(status);
  job_free(job);
  return code;
}

int jobs_launch(ExpandedCommand *commands, int count, int background, const char *text) {
  Job *job = job_new(count, text);
  if (job == NULL) {
    return 1;
  }

  int in_fd = STDIN_FILENO;
  for (int i = 0;i < count;i++) {
    int pipe_fds[2] = { -1, -1 };
    int out_fd = STDOUT_FILENO;
    if (i < count - 1) {
      if (pipe(pipe_fds) != 0) {
        perror("tarsh: pipe");
        break;
      }
      out_fd = pipe_fds[1];
    }

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
      perror("tarsh: fork");
      if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
      }
      job->count = i;
      break;
    }
    if (pid == 0) {
      if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
      }
      child_run(&commands[i], job->pgid, background, in_fd, out_fd);
    }

    job->pids[i] = pid;
    if (job->pgid == 0) {
      job->pgid = pid;
    }
    if (interactive) {
      // Also done in the child: whichever runs first wins the race.
      setpgid(pid, job->pgid);
    }

    if (in_fd != STDIN_FILENO) {
      close(in_fd);
    }
    if (out_fd != STDOUT_FILENO) {
      close(out_fd);
    }
    in_fd = pipe_fds[0];
  }
  if (in_fd != STDIN_FILENO && in_fd >= 0) {
    close(in_fd);
  }

  if (job->count == 0) {
    job_free(job);
    return 1;
  }

  if (background) {
    job->background = 1;
    expand_set_last_background(job->pids[job->count - 1]);
    if (interactive) {
      printf("[%d] %d\n", job->id, (int)job->pids[job->count - 1]);
    }
    return 0;
  }
  return put_in_foreground(job, 0);
}

int jobs_run_argv(char *const argv[], const char *text, int *stopped) {
  ExpandedCommand command;
  memset(&command, 0, sizeof(command));
  arglist_init(&command.argv);
  arglist_init(&command.assignments);
  for (int i = 0;argv[i] != NULL;i++) {
    arglist_append(&command.argv, argv[i]);
  }
  int status = jobs_launch(&command, 1, 0, text);
  arglist_free(&command.argv);
  arglist_free(&command.assignments);
  if (stopped != NULL) {
    *stopped = last_stopped;
  }
  return status;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

static void print_job(const Job *job, const char *state) {
  Job *current = current_job();
  printf("[%d]%c  %-22s  %s\n", job->id, job == current ? '+' : ' ', state, job->text);
}

void jobs_notify(void) {
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
    mark_status(pid, status);
  }

  for (int i = 0;i < MAX_JOBS;i++) {
    Job *job = job_table[i];
    if (job == NULL || !job->background) {
      continue;
    }
    if (job_done(job)) {
      int code = exit_code(job->statuses[job->count - 1]);
      if (interactive) {
        char state[32];
        if (code == 0) {
          snprintf(state, sizeof(state), "Done");
        }
        else {
          snprintf(state, sizeof(state), "Exit %d", code);
        }
        print_job(job, state);
      }
      job_free(job);
    }
    else if (job_stopped(job) && !job->notified) {
      if (interactive) {
        print_job(job, "Stopped");
      }
      job->notified = 1;
    }
  }
}

int jobs_has_stopped(void) {
  for (int i = 0;i < MAX_JOBS;i++) {
    if (job_table[i] != NULL && job_stopped(job_table[i])) {
      return 1;
    }
  }
  return 0;
}

void jobs_shutdown(void) {
  for (int i = 0;i < MAX_JOBS;i++) {
    Job *job = job_table[i];
    if (job != NULL && job_stopped(job)) {
      signal_job(job, SIGHUP);
      signal_job(job, SIGCONT);
    }
  }
}

// ---------------------------------------------------------------------------
// Builtins
// ---------------------------------------------------------------------------

// Finds a job from %n, %+, %%, %prefix or a bare number; NULL means the
// current job.
static Job *find_job(const char *spec, const char *builtin) {
  if (spec == NULL || strcmp(spec, "%") == 0 || strcmp(spec, "%%") == 0 || strcmp(spec, "%+") == 0) {
    Job *job = current_job();
    if (job == NULL) {
      fprintf(stderr, "tarsh: %s: there are no jobs\n", builtin);
    }
    return job;
  }

  const char *body = (spec[0] == '%') ? spec + 1 : spec;
  char *end;
  long id = strtol(body, &end, 10);
  if (*end == '\0' && id >= 1 && id <= MAX_JOBS && job_table[id - 1] != NULL) {
    return job_table[id - 1];
  }
  if (spec[0] == '%' && *end != '\0') {
    for (int i = 0;i < MAX_JOBS;i++) {
      if (job_table[i] != NULL && strncmp(job_table[i]->text, body, strlen(body)) == 0) {
        return job_table[i];
      }
    }
  }
  fprintf(stderr, "tarsh: %s: %s: no such job (see: jobs)\n", builtin, spec);
  return NULL;
}

int jobs_builtin_jobs(ArgList *args) {
  (void)args;
  jobs_notify();
  for (int i = 0;i < MAX_JOBS;i++) {
    Job *job = job_table[i];
    if (job != NULL) {
      print_job(job, job_stopped(job) ? "Stopped" : "Running");
    }
  }
  return 0;
}

int jobs_builtin_fg(ArgList *args) {
  if (!interactive) {
    fprintf(stderr, "tarsh: fg: no job control here\n");
    return 1;
  }
  Job *job = find_job(args->count > 1 ? args->items[1] : NULL, "fg");
  if (job == NULL) {
    return 1;
  }
  printf("%s\n", job->text);
  return put_in_foreground(job, 1);
}

int jobs_builtin_bg(ArgList *args) {
  if (!interactive) {
    fprintf(stderr, "tarsh: bg: no job control here\n");
    return 1;
  }
  Job *job = find_job(args->count > 1 ? args->items[1] : NULL, "bg");
  if (job == NULL) {
    return 1;
  }
  for (int i = 0;i < job->count;i++) {
    if (job->states[i] == PROC_STOPPED) {
      job->states[i] = PROC_RUNNING;
    }
  }
  job->background = 1;
  job->notified = 0;
  job->order = ++job_clock;
  signal_job(job, SIGCONT);
  printf("[%d]+ %s &\n", job->id, job->text);
  return 0;
}

int jobs_builtin_wait(ArgList *args) {
  int code = 0;
  if (args->count == 1) {
    for (int i = 0;i < MAX_JOBS;i++) {
      Job *job = job_table[i];
      if (job == NULL || job_stopped(job)) {
        continue;
      }
      wait_for_job(job);
      if (job_done(job)) {
        code = exit_code(job->statuses[job->count - 1]);
        job_free(job);
      }
    }
    return code;
  }

  for (int a = 1;a < args->count;a++) {
    Job *job = NULL;
    const char *spec = args->items[a];
    if (spec[0] == '%') {
      job = find_job(spec, "wait");
    }
    else {
      pid_t pid = (pid_t)atoi(spec);
      for (int i = 0;i < MAX_JOBS && job == NULL;i++) {
        for (int p = 0;job_table[i] != NULL && p < job_table[i]->count;p++) {
          if (job_table[i]->pids[p] == pid) {
            job = job_table[i];
          }
        }
      }
      if (job == NULL) {
        fprintf(stderr, "tarsh: wait: %s is not a child of this shell\n", spec);
        code = 127;
        continue;
      }
    }
    if (job == NULL) {
      code = 127;
      continue;
    }
    wait_for_job(job);
    if (job_done(job)) {
      code = exit_code(job->statuses[job->count - 1]);
      job_free(job);
    }
  }
  return code;
}

static const struct {
  const char *name;
  int number;
}signal_names[] = {
  { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "KILL", SIGKILL },
  { "TERM", SIGTERM }, { "STOP", SIGSTOP }, { "CONT", SIGCONT }, { "TSTP", SIGTSTP },
  { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 }, { "ALRM", SIGALRM }, { "PIPE", SIGPIPE },
  { "CHLD", SIGCHLD }, { "WINCH", SIGWINCH }, { NULL, 0 }
};

static int parse_signal(const char *text) {
  if (text[0] >= '0' && text[0] <= '9') {
    return atoi(text);
  }
  if (strncmp(text, "SIG", 3) == 0) {
    text += 3;
  }
  for (int i = 0;signal_names[i].name != NULL;i++) {
    if (strcasecmp(signal_names[i].name, text) == 0) {
      return signal_names[i].number;
    }
  }
  return -1;
}

int jobs_builtin_kill(ArgList *args) {
  int signal_number = SIGTERM;
  int first = 1;

  if (args->count > 1 && strcmp(args->items[1], "-l") == 0) {
    for (int i = 0;signal_names[i].name != NULL;i++) {
      printf("%2d) SIG%s\n", signal_names[i].number, signal_names[i].name);
    }
    return 0;
  }
  if (args->count > 2 && strcmp(args->items[1], "-s") == 0) {
    signal_number = parse_signal(args->items[2]);
    first = 3;
  }
  else if (args->count > 1 && args->items[1][0] == '-' && args->items[1][1] != '\0') {
    signal_number = parse_signal(args->items[1] + 1);
    first = 2;
  }
  if (signal_number < 0) {
    fprintf(stderr, "tarsh: kill: unknown signal (see: kill -l)\n");
    return 1;
  }
  if (first >= args->count) {
    fprintf(stderr, "usage: kill [-SIGNAL] pid|%%job ...\n");
    return 2;
  }

  int code = 0;
  for (int i = first;i < args->count;i++) {
    const char *target = args->items[i];
    if (target[0] == '%') {
      Job *job = find_job(target, "kill");
      if (job == NULL) {
        code = 1;
        continue;
      }
      signal_job(job, signal_number);
      // A stopped job needs a nudge to notice most signals.
      if (job_stopped(job) && signal_number != SIGKILL && signal_number != SIGCONT) {
        signal_job(job, SIGCONT);
      }
    }
    else if (kill((pid_t)atoi(target), signal_number) != 0) {
      fprintf(stderr, "tarsh: kill: %s: %s\n", target, strerror(errno));
      code = 1;
    }
  }
  return code;
}
