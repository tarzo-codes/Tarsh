#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include "prompts.h"
#include "lang.h"

#define DEFAULT_FORMAT "[%u@%h] %w (%l) %s "
#define PROMPT_RENDER_MAX 1024

static PromptState active_config;
static char rendered_prompt[PROMPT_RENDER_MAX];

void prompt_write_defaults(FILE *file) {
  fprintf(file, "# Prompt\n");
  fprintf(file, "#   %%u username   %%h hostname   %%w working directory   %%l shell language\n");
  fprintf(file, "#   %%s symbol     %%t last duration   %%? last exit status\n");
  fprintf(file, "FORMAT=%s\n", DEFAULT_FORMAT);
  fprintf(file, "DEPTH=%d\n", active_config.directory_depth);
  fprintf(file, "SHOW_TIME=%d\n", active_config.show_execution_time);
  fprintf(file, "SYMBOL=%s\n", active_config.prompt_symbol);
}

int prompt_configure(const char *key, const char *value) {
  if (strcmp(key, "FORMAT") == 0 || strcmp(key, "FORMATE") == 0) {
    // FORMATE is the misspelling older configs were written with.
    snprintf(active_config.format, sizeof(active_config.format), "%s", value);
  }
  else if (strcmp(key, "DEPTH") == 0) {
    int depth = atoi(value);
    if (depth >= 0) {
      active_config.directory_depth = depth;
    }
  }
  else if (strcmp(key, "SHOW_TIME") == 0) {
    active_config.show_execution_time = atoi(value) != 0;
  }
  else if (strcmp(key, "SYMBOL") == 0) {
    if (value[0] != '\0') {
      snprintf(active_config.prompt_symbol, sizeof(active_config.prompt_symbol), "%s", value);
    }
  }
  else {
    return 0;
  }
  return 1;
}

void setup_prompt(void) {
  active_config.directory_depth = 1;
  active_config.show_execution_time = 1;
  active_config.last_command_duration = 0.0;
  active_config.last_command_status = 0;
  snprintf(active_config.prompt_symbol, sizeof(active_config.prompt_symbol), "$");
  snprintf(active_config.format, sizeof(active_config.format), "%s", DEFAULT_FORMAT);

  active_config.username = getenv("USER");
  if (active_config.username == NULL) {
    active_config.username = getenv("LOGNAME");
  }
  if (active_config.username == NULL) {
    active_config.username = "user";
  }

  if (gethostname(active_config.hostname, sizeof(active_config.hostname)) != 0) {
    snprintf(active_config.hostname, sizeof(active_config.hostname), "localhost");
  }
  active_config.hostname[sizeof(active_config.hostname) - 1] = '\0';
}

void update_prompt(double duration_seconds, int status) {
  active_config.last_command_duration = duration_seconds;
  active_config.last_command_status = status;
}

// Writes the working directory, collapsing $HOME to "~" and keeping only
// the last `directory_depth` components (0 means the whole path).
static void append_working_directory(char *out, size_t out_size, size_t *used) {
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    snprintf(cwd, sizeof(cwd), "?");
  }

  const char *display = cwd;
  char collapsed[PATH_MAX + 1];
  char *home = getenv("HOME");

  if (home != NULL && home[0] != '\0') {
    size_t home_length = strlen(home);
    if (strncmp(cwd, home, home_length) == 0 &&
        (cwd[home_length] == '\0' || cwd[home_length] == '/')) {
      snprintf(collapsed, sizeof(collapsed), "~%s", cwd + home_length);
      display = collapsed;
    }
  }

  if (active_config.directory_depth > 0) {
    // Walk back from the end over `depth` separators.
    int separators = 0;
    const char *cut = display + strlen(display);
    while (cut > display) {
      if (*(cut - 1) == '/') {
        separators++;
        if (separators >= active_config.directory_depth) {
          break;
        }
      }
      cut--;
    }
    if (cut > display && *cut != '\0') {
      display = cut;
    }
  }

  *used += snprintf(out + *used, (*used < out_size) ? out_size - *used : 0, "%s", display);
}

const char *render_prompt(void) {
  size_t used = 0;
  rendered_prompt[0] = '\0';

  for (const char *cursor = active_config.format; *cursor != '\0'; cursor++) {
    if (used >= sizeof(rendered_prompt) - 1) {
      break;
    }

    if (*cursor != '%') {
      rendered_prompt[used++] = *cursor;
      rendered_prompt[used] = '\0';
      continue;
    }

    cursor++;
    size_t remaining = sizeof(rendered_prompt) - used;

    switch (*cursor) {
      case 'u':
        used += snprintf(rendered_prompt + used, remaining, "%s", active_config.username);
        break;
      case 'h':
        used += snprintf(rendered_prompt + used, remaining, "%s", active_config.hostname);
        break;
      case 'w':
        append_working_directory(rendered_prompt, sizeof(rendered_prompt), &used);
        break;
      case 'l':
        used += snprintf(rendered_prompt + used, remaining, "%s", lang_current_name());
        break;
      case 's':
        used += snprintf(rendered_prompt + used, remaining, "%s", active_config.prompt_symbol);
        break;
      case 't':
        if (active_config.show_execution_time && active_config.last_command_duration >= 0.1) {
          used += snprintf(rendered_prompt + used, remaining, "%.2fs",
                           active_config.last_command_duration);
        }
        break;
      case '?':
        if (active_config.last_command_status != 0) {
          used += snprintf(rendered_prompt + used, remaining, "%d",
                           active_config.last_command_status);
        }
        break;
      case '%':
        rendered_prompt[used++] = '%';
        rendered_prompt[used] = '\0';
        break;
      case '\0':
        // Trailing '%' with nothing after it: emit it and stop.
        rendered_prompt[used++] = '%';
        rendered_prompt[used] = '\0';
        cursor--;
        break;
      default:
        used += snprintf(rendered_prompt + used, remaining, "%%%c", *cursor);
        break;
    }

    if (used >= sizeof(rendered_prompt) - 1) {
      used = sizeof(rendered_prompt) - 1;
      rendered_prompt[used] = '\0';
      break;
    }
  }

  return rendered_prompt;
}
