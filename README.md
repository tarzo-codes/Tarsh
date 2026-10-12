# tarsh

> a beginner-friendly Linux shell: fzf pickers where you need them, fish-style suggestions and highlighting, a Python-like script language, and bash/zsh/fish commands that just work.

```
$ cd                       → press space: pick a folder from a list
$ git sta                  → git status          (grey suggestion from history; → accepts)
$ for f in glob("*.log"):  → tarsh script: reads like Python...
...     gzip $f            → ...and any other line is a command
$ for i in 1 2; do echo $i; done
1
2
↳ ran in bash: a for-loop: ran the commands between `do` and `done` once for each item.
```

## Install

```sh
git clone https://github.com/tarzo-codes/Tarsh.git
cd Tarsh
./install.sh        # builds, tests and installs to ~/.local, no sudo needed
tarsh               # start it
tarsh doctor        # check your setup
```

Full instructions, options, making it your login shell, updating and uninstalling: **[docs/INSTALL.md](docs/INSTALL.md)**.

## What it does

**Finding things.** Type `cd`, `nvim`, `cp`, `mv` or `rm` and press space: an [fzf](https://github.com/junegunn/fzf) picker opens. Tab completes commands, paths and **options with their descriptions**. Ctrl+R searches your history.

**Typing less (from fish).** Grey **autosuggestions** from history; **syntax highlighting** as you type (unknown commands show red); ↑ searches for what you've typed; **abbreviations** (`abbr gco git checkout`); `set -U` **universal variables** shared between terminals; folder history (`prevd`, `nextd`, `cdh`, Alt+←/→); typing a folder path goes there; `string`, `math`, `count`, `contains`, `random`, `and`/`or`/`not`; variables are never split at spaces.

**See what a command will do.** As you type a `|` / `>>` / `<<` chain, a line under the prompt explains every step and the output to expect. Regexes are explained in words. For commands you list (grep, awk, sed, jq, ...), a **live preview** shows the real output as you type, in a sandbox that can't change files or touch the network. grep matches are highlighted, so it doubles as a regex tester. `explain 'LINE'` prints the full breakdown.

**A real shell.** Pipes, all the redirections, `&&` `||` `;` `&`, wildcards, `$(...)`, aliases, Ctrl+Z / `fg` / `bg` / `jobs`, multi-line input, scripts, `-c`, login shells.

**tarsh script.** A small language that reads like Python but treats every other line as a command. `$var` works in commands, `$(cmd)` captures output, `?(cmd)` tests success, and functions you `def` run like commands (`funcsave` keeps them). Guide: **[docs/SCRIPTING.md](docs/SCRIPTING.md)**.

**Other shells' commands.** Paste bash, zsh or fish syntax and tarsh recognises it, runs it in that shell, brings back any folder or variable changes, and tells you in one line what happened. `source venv/bin/activate` just works. Scripts without a `#!` line are run by the right shell.

**Your setup.** `config` edits your startup file (`~/.config/tarsh/tarshrc`, tarsh's `.bashrc`). `import-rc` copies your aliases from bash and zsh. **`theme edit`** opens a theme editor in your browser where you design the prompt (git, programming languages, colors, Nerd Font icons) with a live preview.

**Help when things go wrong.** "did you mean: ls", "on Linux that's: clear", "that's a folder: cd into it", clear errors with line numbers, and `help` for a tour.

New to it? The **[guide](docs/GUIDE.md)** walks through everything in ten minutes.

## Documentation

| | |
| --- | --- |
| [docs/INSTALL.md](docs/INSTALL.md) | installing, extras, default shell, updating, uninstalling, troubleshooting |
| [docs/GUIDE.md](docs/GUIDE.md) | a hands-on tour for new users |
| [docs/SCRIPTING.md](docs/SCRIPTING.md) | the tarsh script language |
| [docs/CUSTOMIZING.md](docs/CUSTOMIZING.md) | tarshrc, settings, prompt themes and the theme editor |
| `man tarsh` | options, keys, builtins, files |

## Development

```sh
make            # build ./tarsh
make check      # run the test suite (tests/cases/*.tsh)
make debug      # build with address/undefined-behaviour sanitizers
```

| file | job |
| --- | --- |
| `src/main.c` | startup, the prompt loop, `-c` / script / login modes |
| `src/exec.c` | runs a line: script or command, native or another shell, builtins, functions |
| `src/script.c` | tarsh script: tokenizer, parser, interpreter, built-in functions |
| `src/syntax.c`, `src/expand.c` | the command parser; `$var`, `$(...)`, `~`, wildcards |
| `src/jobs.c` | processes, pipes, redirections, job control |
| `src/foreign.c`, `src/lang.c` | recognising bash/zsh/fish syntax, running it there, the notes |
| `src/lineedit.c`, `src/highlight.c` | the line editor: suggestions, highlighting, history, keys |
| `src/fish.c` | abbr, set, universal variables, string/math/..., folder history, saved functions |
| `src/fzf.c`, `src/complete.c` | pickers and completion (with option descriptions) |
| `src/pipehint.c`, `src/live.c` | explaining pipelines and regexes; the sandboxed live preview |
| `src/theme.c`, `src/theme_ui.c`, `ui/` | prompt themes and the browser theme editor |
| `src/builtins.c`, `src/alias.c`, `src/suggest.c` | builtins, aliases, "did you mean" |
| `src/config.c`, `src/prompts.c`, `src/util.c`, `src/parser.c` | settings, classic prompt, helpers |

## Why?

Because the best way to understand a tool is to build it yourself. Shells feel like magic until you write one.

## License

MIT. Do whatever you want with it.
