#ifndef JOBS_H
#define JOBS_H

#include "parser.h"
#include "syntax.h"

// A redirection after expansion: file names are final.
typedef struct {
  RedirKind kind;
  int fd;
  int target_fd;
  char *path;
}ExpandedRedirect;

// A command ready to run: final argv, redirections, and NAME=value
// assignments that apply to this command only.
typedef struct {
  ArgList argv;
  ExpandedRedirect *redirs;
  int redir_count;
  ArgList assignments;
}ExpandedCommand;

// Remembers where a descriptor pointed before a redirection, so builtins
// that run inside the shell can be redirected and then put back.
typedef struct {
  int fd;
  int copy;   // -1 if the descriptor was closed before
}SavedFd;

#define MAX_SAVED_FDS 16

// Interactive shells get job control: their own process group and
// control of the terminal.
void jobs_init(int interactive);
int jobs_interactive(void);

// Called in a forked copy of the shell (for $(...)): no job control.
void jobs_become_subshell(void);

// Runs a pipeline of `count` commands. Foreground: waits and returns the
// last command's status (148 if it was stopped with Ctrl+Z). Background:
// returns 0 immediately.
int jobs_launch(ExpandedCommand *commands, int count, int background, const char *text);

// Runs one program in the foreground, e.g. the bash/zsh/fish backend.
// *stopped is set if the user suspended it with Ctrl+Z.
int jobs_run_argv(char *const argv[], const char *text, int *stopped);

// Applies redirections. With `saved`, the old descriptors are recorded
// for jobs_restore_fds(); without, the change is permanent.
int jobs_apply_redirects(const ExpandedRedirect *redirs, int count,
                         SavedFd *saved, int *saved_count);
void jobs_restore_fds(SavedFd *saved, int saved_count);

// Reaps finished background jobs and reports them ("[1]+ Done ...").
void jobs_notify(void);

int jobs_has_stopped(void);
// Sends SIGHUP to stopped jobs when the shell exits.
void jobs_shutdown(void);

// The jobs, fg, bg, wait and kill builtins.
int jobs_builtin_jobs(ArgList *args);
int jobs_builtin_fg(ArgList *args);
int jobs_builtin_bg(ArgList *args);
int jobs_builtin_wait(ArgList *args);
int jobs_builtin_kill(ArgList *args);

#endif
