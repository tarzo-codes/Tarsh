# tarsh

> a no-frills Unix shell written in C — not here to replace bash, just here to understand it.

## what is this?

`tarsh` is a minimal Unix shell built from scratch in C as a learning project. the goal isn't to ship a production shell — it's to understand what actually happens when you type a command and hit enter.

## build & run

**requirements:** `gcc` (or `clang`), `make`, any Unix-like system (Linux/macOS)

```bash
git clone https://github.com/yourusername/tarsh.git
cd tarsh
make
./tarsh
```

other targets: `make debug` (address/UB sanitizers), `make run`, `make clean`,
`make install` (defaults to `/usr/local/bin`).

## what works

- **a real prompt**, rendered from a config file you can edit
- **command execution** via `fork()` + `execvp()`, with the exit status kept
  around (`127` for not found, `126` for not executable, `128+n` when a
  command is killed by a signal)
- **quoting** — `'single'`, `"double"`, and `\` escapes are handled by the
  tokenizer instead of a naive space split
- **builtins** that have to run in the shell process itself:
  `cd` (incl. `cd -`), `pwd`, `echo [-n]`, `export`, `unset`, `help`, `exit [status]`
- **signals** — `Ctrl+C` kills the running command, not your shell;
  `Ctrl+D` exits cleanly
- **command timing**, so the prompt can tell you what took so long
- comments (`# ...`) and blank lines are ignored

## configuration

first run writes `~/.config/tarsh/config.t`:

```ini
FORMAT=[%u@%h] %w %s
DEPTH=1
SHOW_TIME=1
SYMBOL=$
```

| key | meaning |
| --- | --- |
| `FORMAT` | the prompt template |
| `DEPTH` | how many trailing path components `%w` shows (`0` = the whole path) |
| `SHOW_TIME` | whether `%t` reports the last command's duration |
| `SYMBOL` | what `%s` expands to |

format specifiers: `%u` username, `%h` hostname, `%w` working directory
(`$HOME` collapsed to `~`), `%s` symbol, `%t` last command duration
(hidden under 0.1s), `%?` last exit status (hidden when it was 0), `%%` a
literal percent.

so `FORMAT=%w %? %s ` with `SYMBOL=❯` gives you `Tarsh 1 ❯`.

## layout

| file | job |
| --- | --- |
| `src/main.c` | read → parse → dispatch loop, fork/exec, signals, timing |
| `src/parser.c` | tokenizer: whitespace splitting, quotes, escapes |
| `src/builtins.c` | commands that must change the shell's own state |
| `src/prompts.c` | config file loading and prompt rendering |

## not yet

pipes (`|`), redirection (`>`, `<`), `&&`/`||`, background jobs (`&`),
globbing, variable expansion (`$VAR`), history and line editing.

## why?

because the best way to understand a tool is to build it yourself. shells feel like magic until you write one.

## license

MIT — do whatever you want with it.
