// accept4()
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "theme_ui.h"
#include "theme.h"
#include "theme_editor_html.h"

#define MAX_HEADER 16384
#define MAX_BODY (256 * 1024)

static char token[33];
static int server_port = 0;
static volatile sig_atomic_t interrupted = 0;
static int finished = 0;

static void on_interrupt(int signal_number) {
  (void)signal_number;
  interrupted = 1;
}

static int make_token(void) {
  unsigned char bytes[16];
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0 || read(fd, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes)) {
    if (fd >= 0) close(fd);
    return -1;
  }
  close(fd);
  for (int i = 0;i < 16;i++) {
    snprintf(token + i * 2, 3, "%02x", bytes[i]);
  }
  return 0;
}

static void write_all(int fd, const char *data, size_t length) {
  while (length > 0) {
    ssize_t written = write(fd, data, length);
    if (written < 0) {
      if (errno == EINTR) continue;
      return;
    }
    data += written;
    length -= written;
  }
}

static void respond(int fd, int status, const char *reason, const char *type, const char *body, size_t length) {
  char header[1024];
  int used = snprintf(header, sizeof(header),
    "HTTP/1.1 %d %s\r\n"
    "Content-Type: %s\r\n"
    "Content-Length: %zu\r\n"
    "Cache-Control: no-store\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; "
    "style-src 'unsafe-inline'; connect-src 'self'; img-src data:; font-src data:; "
    "frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n"
    "Connection: close\r\n\r\n",
    status, reason, type, length);
  write_all(fd, header, used);
  write_all(fd, body, length);
}

static void respond_text(int fd, int status, const char *reason, const char *message) {
  respond(fd, status, reason, "text/plain; charset=utf-8", message, strlen(message));
}

// Finds a header's value (case-insensitive name) in the raw header block.
static int header_value(const char *headers, const char *name, char *out, size_t size) {
  size_t name_length = strlen(name);
  const char *line = strstr(headers, "\r\n");
  while (line != NULL) {
    line += 2;
    if (strncasecmp(line, name, name_length) == 0 && line[name_length] == ':') {
      const char *value = line + name_length + 1;
      while (*value == ' ') value++;
      size_t length = strcspn(value, "\r\n");
      snprintf(out, size, "%.*s", (int)length, value);
      return 1;
    }
    line = strstr(line, "\r\n");
  }
  return 0;
}

static ThemeContext sample_context(int failed) {
  static char host[256];
  ThemeContext context;
  if (gethostname(host, sizeof(host)) != 0) {
    snprintf(host, sizeof(host), "localhost");
  }
  host[sizeof(host) - 1] = '\0';
  const char *user = getenv("USER");
  if (user == NULL) user = getenv("LOGNAME");
  context.user = user ? user : "user";
  context.host = host;
  context.shell_language = "bash";
  // The "after an error" preview also shows duration and a job.
  context.last_status = failed ? 127 : 0;
  context.last_duration = failed ? 4.2 : 0.1;
  context.jobs = failed ? 1 : 0;
  return context;
}

static void send_json(int fd, char *json, size_t length) {
  respond(fd, 200, "OK", "application/json; charset=utf-8", json, length);
  free(json);
}

static void handle_state(int fd) {
  char *json = NULL;
  size_t length = 0;
  FILE *out = open_memstream(&json, &length);
  if (out == NULL) {
    respond_text(fd, 500, "Internal Server Error", "out of memory");
    return;
  }
  char *saved = theme_saved_text();
  fputs("{\"theme\":", out);
  theme_json_string(out, saved);
  free(saved);
  fputs(",\"path\":", out);
  theme_json_string(out, theme_path());
  fprintf(out, ",\"active\":%s", theme_active() ? "true" : "false");
  fputs(",\"presets\":[", out);
  for (int i = 0;i < theme_preset_count();i++) {
    if (i > 0) fputc(',', out);
    fputs("{\"name\":", out);
    theme_json_string(out, theme_preset_name(i));
    fputs(",\"description\":", out);
    theme_json_string(out, theme_preset_description(i));
    fputs(",\"text\":", out);
    theme_json_string(out, theme_preset_text(i));
    fputc('}', out);
  }
  fputs("],\"context\":", out);
  theme_write_context_json(out);
  fputc('}', out);
  fclose(out);
  send_json(fd, json, length);
}

static void handle_render(int fd, const char *body) {
  ArgList warnings;
  arglist_init(&warnings);
  Theme *theme = theme_parse(body, &warnings);
  if (theme == NULL) {
    arglist_free(&warnings);
    respond_text(fd, 500, "Internal Server Error", "out of memory");
    return;
  }

  ThemeContext ok = sample_context(0);
  ThemeContext failed = sample_context(1);
  char *ok_prompt = theme_render(theme, &ok);
  char *error_prompt = theme_render(theme, &failed);

  char *json = NULL;
  size_t length = 0;
  FILE *out = open_memstream(&json, &length);
  if (out != NULL) {
    fputs("{\"ok\":", out);
    theme_json_string(out, ok_prompt);
    fputs(",\"error\":", out);
    theme_json_string(out, error_prompt);
    fputs(",\"warnings\":[", out);
    for (int i = 0;i < warnings.count;i++) {
      if (i > 0) fputc(',', out);
      theme_json_string(out, warnings.items[i]);
    }
    fputs("]}", out);
    fclose(out);
    send_json(fd, json, length);
  }
  else {
    respond_text(fd, 500, "Internal Server Error", "out of memory");
  }

  free(ok_prompt);
  free(error_prompt);
  theme_free(theme);
  arglist_free(&warnings);
}

static void handle_save(int fd, const char *body) {
  if (theme_save_text(body) != 0) {
    char message[512];
    snprintf(message, sizeof(message), "{\"saved\":false,\"error\":\"could not write %s: %s\"}",
             theme_path(), strerror(errno));
    respond(fd, 500, "Internal Server Error", "application/json", message, strlen(message));
    return;
  }
  printf("  saved %s\n", theme_path());
  fflush(stdout);
  const char *reply = "{\"saved\":true}";
  respond(fd, 200, "OK", "application/json", reply, strlen(reply));
}

static void handle_client(int fd) {
  struct timeval timeout = { 5, 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  char *request = malloc(MAX_HEADER + MAX_BODY + 1);
  if (request == NULL) {
    return;
  }
  size_t used = 0;
  char *header_end = NULL;
  while (used < MAX_HEADER && header_end == NULL) {
    ssize_t got = read(fd, request + used, MAX_HEADER - used);
    if (got <= 0) {
      free(request);
      return;
    }
    used += got;
    request[used] = '\0';
    header_end = strstr(request, "\r\n\r\n");
  }
  if (header_end == NULL) {
    respond_text(fd, 431, "Request Header Fields Too Large", "headers too large");
    free(request);
    return;
  }

  char method[8] = "";
  char path[512] = "";
  if (sscanf(request, "%7s %511s", method, path) != 2) {
    respond_text(fd, 400, "Bad Request", "bad request");
    free(request);
    return;
  }

  // Only this machine, by name, may talk to us (blocks DNS rebinding).
  char host[256] = "";
  char expected_ip[64];
  char expected_name[64];
  snprintf(expected_ip, sizeof(expected_ip), "127.0.0.1:%d", server_port);
  snprintf(expected_name, sizeof(expected_name), "localhost:%d", server_port);
  header_value(request, "Host", host, sizeof(host));
  if (strcmp(host, expected_ip) != 0 && strcmp(host, expected_name) != 0) {
    respond_text(fd, 403, "Forbidden", "wrong host");
    free(request);
    return;
  }

  // Everything lives under /<token>/.
  size_t token_length = strlen(token);
  if (path[0] != '/' || strncmp(path + 1, token, token_length) != 0 ||
      (path[1 + token_length] != '/' && path[1 + token_length] != '\0')) {
    respond_text(fd, 404, "Not Found", "not found (the editor link includes a secret; copy it from the terminal)");
    free(request);
    return;
  }
  const char *route = path + 1 + token_length;
  if (route[0] == '\0') {
    route = "/";
  }

  int is_post = strcmp(method, "POST") == 0;
  char *body = header_end + 4;
  size_t have = used - (body - request);
  if (is_post) {
    // Browsers only add custom headers from our own page, never from a
    // form or another site, so this blocks cross-site writes.
    char marker[16] = "";
    if (!header_value(request, "X-Tarsh", marker, sizeof(marker)) || strcmp(marker, "1") != 0) {
      respond_text(fd, 403, "Forbidden", "missing X-Tarsh header");
      free(request);
      return;
    }
    char length_text[32] = "0";
    header_value(request, "Content-Length", length_text, sizeof(length_text));
    long wanted = atol(length_text);
    if (wanted < 0 || wanted > MAX_BODY) {
      respond_text(fd, 413, "Payload Too Large", "theme too large");
      free(request);
      return;
    }
    while ((long)have < wanted) {
      ssize_t got = read(fd, body + have, wanted - have);
      if (got <= 0) break;
      have += got;
    }
    body[have < (size_t)wanted ? have : (size_t)wanted] = '\0';
  }

  if (!is_post && strcmp(method, "GET") != 0) {
    respond_text(fd, 405, "Method Not Allowed", "method not allowed");
  }
  else if (!is_post && strcmp(route, "/") == 0) {
    respond(fd, 200, "OK", "text/html; charset=utf-8", (const char *)theme_editor_html, theme_editor_html_length);
  }
  else if (!is_post && strcmp(route, "/api/state") == 0) {
    handle_state(fd);
  }
  else if (is_post && strcmp(route, "/api/render") == 0) {
    handle_render(fd, body);
  }
  else if (is_post && strcmp(route, "/api/save") == 0) {
    handle_save(fd, body);
  }
  else if (is_post && strcmp(route, "/api/done") == 0) {
    const char *reply = "{\"done\":true}";
    respond(fd, 200, "OK", "application/json", reply, strlen(reply));
    finished = 1;
  }
  else {
    respond_text(fd, 404, "Not Found", "not found");
  }
  free(request);
}

static int has_display(void) {
  const char *display = getenv("DISPLAY");
  const char *wayland = getenv("WAYLAND_DISPLAY");
  const char *browser = getenv("BROWSER");
  if ((display && *display) || (wayland && *wayland) || (browser && *browser)) {
    return 1;
  }
#ifdef __APPLE__
  return 1;
#else
  // WSL can open the Windows browser.
  return access("/proc/sys/fs/binfmt_misc/WSLInterop", F_OK) == 0;
#endif
}

static void open_browser(const char *url) {
  pid_t pid = fork();
  if (pid != 0) {
    return;
  }
  setsid();
  signal(SIGINT, SIG_DFL);
  signal(SIGQUIT, SIG_DFL);
  signal(SIGTSTP, SIG_DFL);
  signal(SIGTTIN, SIG_DFL);
  signal(SIGTTOU, SIG_DFL);
  int null = open("/dev/null", O_RDWR);
  if (null >= 0) {
    dup2(null, STDIN_FILENO);
    dup2(null, STDOUT_FILENO);
    dup2(null, STDERR_FILENO);
  }
  const char *browser = getenv("BROWSER");
  if (browser != NULL && *browser) {
    execlp(browser, browser, url, (char *)NULL);
  }
  execlp("xdg-open", "xdg-open", url, (char *)NULL);
  execlp("wslview", "wslview", url, (char *)NULL);
  execlp("open", "open", url, (char *)NULL);
  _exit(127);
}

int theme_editor_run(int want_browser, int port) {
  if (make_token() != 0) {
    fprintf(stderr, "tarsh: theme: couldn't make a secret for the editor link\n");
    return 1;
  }

  int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener < 0) {
    perror("tarsh: theme: socket");
    return 1;
  }
  int yes = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons((unsigned short)port);
  if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 8) != 0) {
    fprintf(stderr, "tarsh: theme: can't listen on 127.0.0.1:%d: %s\n", port, strerror(errno));
    close(listener);
    return 1;
  }
  socklen_t address_length = sizeof(address);
  getsockname(listener, (struct sockaddr *)&address, &address_length);
  server_port = ntohs(address.sin_port);

  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/%s/", server_port, token);

  printf("\n  theme editor: %s\n", url);
  if (want_browser && has_display()) {
    printf("  opening your browser...\n");
    open_browser(url);
  }
  else {
    printf("  open that link in your browser.\n");
    printf("  over ssh? run this on your own computer first, then open the link there:\n");
    printf("    ssh -N -L %d:127.0.0.1:%d <this-machine>\n", server_port, server_port);
  }
  printf("  changes apply when you click Save. press Enter here (or click Done) to finish.\n\n");
  fflush(stdout);

  // Ctrl+C should end the editor; the shell normally ignores it.
  struct sigaction stop_action;
  struct sigaction previous_action;
  memset(&stop_action, 0, sizeof(stop_action));
  stop_action.sa_handler = on_interrupt;
  sigemptyset(&stop_action.sa_mask);
  sigaction(SIGINT, &stop_action, &previous_action);
  interrupted = 0;
  finished = 0;

  int watch_terminal = isatty(STDIN_FILENO);
  while (!finished && !interrupted) {
    struct pollfd fds[2] = {
      { listener, POLLIN, 0 },
      { STDIN_FILENO, POLLIN, 0 }
    };
    int ready = poll(fds, watch_terminal ? 2 : 1, -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (watch_terminal && (fds[1].revents & (POLLIN | POLLHUP))) {
      char discard[256];
      if (read(STDIN_FILENO, discard, sizeof(discard)) >= 0) {
        break;
      }
    }
    if (fds[0].revents & POLLIN) {
      int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
      if (client >= 0) {
        handle_client(client);
        close(client);
      }
    }
  }

  sigaction(SIGINT, &previous_action, NULL);
  close(listener);
  printf("  theme editor closed%s\n", theme_active() ? ". your new prompt is ready." : ".");
  return 0;
}
