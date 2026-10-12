# tarsh

> a beginner-friendly Unix shell written in C: fzf pickers where you need them, and bash, zsh or fish when you need a real language.

## what is this?

`tarsh` is a small, beginner-friendly Unix shell built from scratch in C. it started as a way to understand what actually happens when you type a command and hit enter, and it's growing into a shell that helps you find your way around instead of making you remember paths.

## the idea

- **you shouldn't have to remember paths.** type `cd`, `nvim`, `cp`, `mv` or `rm` and press the space bar: an [fzf](https://github.com/junegunn/fzf) picker opens so you can fuzzy-search for the file or folder.
- **it's a real shell.** pipes, redirections, `&&`/`||`, wildcards, `$(...)`, aliases, background jobs and Ctrl+Z / `fg` / `bg` are all built in.
- **you keep the shell language you know.** `if`/`for`/`while`, functions and other scripting syntax run in **bash, zsh or fish**, whichever you choose.
- **a prompt you design yourself.** `theme edit` opens a theme editor in your browser: git, languages, colors, Nerd Font glyphs, with a live preview.
- **your own startup file.** aliases and variables live in `~/.config/tarsh/tarshrc`. you don't need `.bashrc`, and `import-rc` copies over what you already have.
- **mistakes get explained.** `sl` → *did you mean: ls*, `cls` → *on Linux that's: clear*, typing a folder name → *to go into it: cd projects*.

## build & run

**requirements:** `gcc` (or `clang`), `make`, Linux. **recommended:** `fzf`, plus `zsh`/`fish` if you want those languages.

```bash
sudo apt install build-essential fzf      # Fedora: dnf, Arch: pacman
git clone https://github.com/tarzo-codes/Tarsh.git
cd Tarsh
make
./tarsh
```

other targets: `make debug` (address/UB sanitizers), `make run`, `make clean`,
`make install` (defaults to `/usr/local/bin`).

## fzf pickers

| you type | what happens |
| --- | --- |
| `cd` + space | pick a folder (`..` and `~` are at the top) |
| `nvim`, `vim`, `nano`, `cat`, `less` or `code` + space | pick files (Tab selects several) |
| `cp` or `mv` + space | pick what to copy/move, then pick where it goes |
| `rm` + space | pick files to delete (Tab selects several) |
| `sudo nvim` + space | same as `nvim` + space |
| **Tab** | on the first word: pick a command. after that: pick a path for the word you're typing |
| **Ctrl+R** | search your command history |

in a picker: type to filter, **Enter** to choose, **Esc** to close it and type the path yourself. a preview on the right shows the folder contents or the start of the file.

pressing space a second time never opens a picker, so you can always type normally. which commands open a picker is set by `FZF_COMMANDS` in the config.

## the shell basics

| you type | what it does |
| --- | --- |
| `a \| b` | pipe a's output into b |
| `a > f`, `a >> f`, `a < f` | write / append / read a file |
| `a 2> f`, `a 2>&1`, `a &> f` | redirect errors, merge them into output, or both into a file |
| `a && b`, `a \|\| b`, `a ; b` | run b if a worked / failed / always |
| `a &` | run in the background (`$!` is its pid) |
| `*.txt`, `file?.c`, `[ab]*` | wildcards (a pattern that matches nothing stays as typed) |
| `$VAR`, `${VAR}`, `${VAR:-default}` | variables |
| `$(cmd)`, `` `cmd` `` | use a command's output |
| `$?`, `$$`, `$!`, `$0`–`$9`, `$#`, `$@` | last status, shell pid, last background pid, script arguments |
| `~`, `~/code` | your home folder |
| `NAME=value`, `NAME=value cmd` | set a variable, or set it just for one command |
| `'...'`, `"..."`, `\` | quoting |
| `# note` | comment |

### job control

| key / command | what it does |
| --- | --- |
| Ctrl+C | stop the running program |
| Ctrl+Z | pause it |
| `jobs` | list paused and background jobs |
| `fg [%n]` | bring a job back to the front |
| `bg [%n]` | let a paused job keep running in the background |
| `kill [-SIGNAL] %n\|pid` | send a signal (`kill -l` lists them) |
| `wait [%n\|pid]` | wait for background jobs |

finished background jobs are reported at the next prompt (`[1]+  Done  sleep 5`).

### multi-line input

an unfinished line (an open quote, a trailing `|` or `&&` or `\`, an `if`/`for`/`while` without its `fi`/`done`) gets a `>` prompt so you can keep typing. history stores it as one line.

## shell languages: bash, zsh, fish

tarsh runs commands, pipes, redirections and everything in the table above itself. for real scripting syntax it hands the line to **bash, zsh or fish**:

- `if` / `for` / `while` / `until` / `case`, functions (`name() { ... }`, fish's `function`)
- subshells `( ... )`, here-documents `<<`, arithmetic `$(( ))`, brace expansion `{a,b}`, `[[ ]]`
- fish's `set`, `begin`, `and`/`or`, `(cmd)` substitution, `**` in zsh/fish

```
[you@box] Tarsh (bash) $ for f in *.c; do wc -l $f; done       # runs through bash
[you@box] Tarsh (bash) $ lang fish
now using fish for loops, functions and other fish syntax
[you@box] Tarsh (fish) $ for f in *.c; wc -l $f; end            # fish syntax now
```

- `lang` lists the languages and which are installed; `lang zsh` switches. a missing shell comes with its install command.
- directory and variable changes come back to tarsh: `cd src && make` leaves you in `src`, and `export X=$(date)`, fish's `set -gx` or `source venv/bin/activate` keep their variables.
- Ctrl+Z and `fg` work on these too.

## prompt themes

the prompt is built from **segments** you arrange and color yourself:

| segment | shows |
| --- | --- |
| `cwd` | the folder you're in (`~` for home, last *n* folders) |
| `git` | branch, a ● when there are changes or `+staged ~modified ?untracked` counts, `⇡ahead ⇣behind` |
| `langs` | languages found in the folder (python, node, rust, go, java, kotlin, c, c++, ruby, php, lua, elixir, haskell, dart, zig, swift, .NET, deno, docker), optionally with versions, in each language's own color |
| `venv` | the active Python virtualenv / conda env |
| `status`, `duration`, `jobs` | exit code after a failure, how long a slow command took, how many jobs are running |
| `user`, `host`, `userhost`, `os`, `shell`, `time` | who, where, your distro's logo, bash/zsh/fish, the clock |
| `symbol` | the `❯` you type after; can turn red when the last command failed |
| `text`, `newline` | anything you like; a second line |

each segment has its own text and background color (terminal colors, 256-color numbers or any `#hex`), bold/italic, an icon, text before/after it, and colors for "after an error". segments join with **powerline** arrows, **round** pills, **slant** edges, plain blocks, or as plain text. glyphs come in three sets: **nerd** (needs a [Nerd Font](https://www.nerdfonts.com) in your terminal), **unicode**, or **ascii** for any terminal. hex colors are converted automatically for terminals with 256 or 16 colors.

### the editor

```
theme edit
```

opens the theme editor in your browser:

- **live preview** drawn by tarsh's own engine with the real git branch and languages of the folder you're in, both normally and after a failed command
- **segments list**: add, drag to reorder, duplicate, remove
- **colors**: terminal colors, ready-made Nord / Catppuccin / Gruvbox / Tokyo Night palettes, or any color
- **glyph picker**: searchable Nerd Font icons for git, folders, languages, operating systems and more, or paste your own
- **presets** to start from, and a `theme.t` tab to edit the file directly with warnings for mistakes
- **Save** applies it to your shell straight away; **Done** (or Enter in the terminal) closes the editor

the editor only listens on `127.0.0.1`, uses a secret link, and runs only while it's open. on a machine without a browser (e.g. over ssh), `theme edit --no-open` prints the link and the `ssh -L` command to reach it from your computer.

### other theme commands

| command | does |
| --- | --- |
| `theme list` / `theme use NAME` | built-in themes: `powerline`, `rounded`, `minimal`, `boxed`, `classic` |
| `theme off` / `theme on` | switch between your theme and the classic `FORMAT` prompt |
| `theme reload`, `config theme` | re-read `theme.t` / edit it in your text editor |

themes live in `~/.config/tarsh/theme.t`, a plain text file:

```ini
[theme]
name=mine
separator=powerline
glyphs=nerd

[segment]
type=cwd
fg=#ffffff
bg=#5e81ac
depth=2

[segment]
type=git
fg=black
bg=yellow
counts=1

[segment]
type=symbol
fg=green
error_fg=red
```

## line editing

| key | action |
| --- | --- |
| ↑ / ↓ | previous / next command (saved in `~/.config/tarsh/history`) |
| ← → / Ctrl+← → | move by character / word |
| Ctrl+A / Ctrl+E | start / end of line |
| Ctrl+W / Ctrl+U / Ctrl+K | delete word / to start / to end |
| Ctrl+C | cancel the line, or stop the running command |
| Ctrl+L | clear the screen |
| Ctrl+D | exit (on an empty line) |

a line that starts with a space is kept out of history.

## builtins

| builtin | what it does |
| --- | --- |
| `cd [dir\|-]`, `pwd` | change / show the folder |
| `echo [-n] [-e]` | print text (`-e` understands `\n`, `\t`) |
| `export N=V`, `unset N` | set / remove variables |
| `alias n='cmd'`, `unalias n` | shortcuts |
| `source file` | run a file in this shell: tarsh files natively, anything else (like a venv's `activate`) through your shell language |
| `type name` | is it an alias, a builtin or a program? |
| `read [-p prompt] [var...]` | read a line of input |
| `shift [n]`, `exec cmd`, `umask [mode]`, `true`, `false`, `:` | the usual |
| `jobs`, `fg`, `bg`, `wait`, `kill` | job control |
| `history [n]` | previous commands |
| `lang [name]` | show or change the shell language |
| `config [settings\|theme]` | edit tarshrc (or config.t, or theme.t) in your editor and reload it |
| `theme [edit\|list\|use\|on\|off]` | design your prompt (see "prompt themes") |
| `import-rc` | copy aliases and exports from `~/.bashrc`, `~/.bash_aliases`, `~/.zshrc`, `~/.profile` |
| `help`, `exit [status]` | |

## tarshrc: your startup file

tarsh doesn't read `~/.bashrc` or `~/.zshrc`. those are written in bash's and zsh's own languages, and tarsh would have to run a whole bash just to read them. it has its own startup file instead, `~/.config/tarsh/tarshrc`, written in tarsh's own syntax. it runs every time an interactive tarsh starts, and the first run creates it with some useful aliases:

```sh
alias ls='ls --color=auto'
alias ll='ls -lh'
alias la='ls -A'
alias grep='grep --color=auto'
alias ..='cd ..'
alias ...='cd ../..'

# export EDITOR=nvim
# export PATH="$HOME/.local/bin:$PATH"
# lang fish
```

- `config` opens it in your editor (`$EDITOR`, or nano/vim) and reloads it when you close it.
- `import-rc` copies the `alias` and `export` lines from your bash/zsh files into it, so switching over takes one command.
- login shells (`tarsh -l`, or tarsh set with `chsh`) also run `/etc/profile` and `~/.profile` through `sh` first, so the system `PATH` and friends are set up as usual.

## configuration

settings (not commands) live in `~/.config/tarsh/config.t` (`config settings` opens it):

```ini
FORMAT=[%u@%h] %w (%l) %s 
DEPTH=1
SHOW_TIME=1
SYMBOL=$

FZF=1
FZF_COMMANDS=cd,cp,mv,rm,nvim,vim,nano,cat,less,code,source

SHELL_LANG=bash
```

| key | meaning |
| --- | --- |
| `FORMAT` | the prompt template (trailing spaces are kept) |
| `DEPTH` | how many trailing path components `%w` shows (`0` = the whole path) |
| `SHOW_TIME` | whether `%t` reports the last command's duration |
| `SYMBOL` | what `%s` expands to |
| `FZF` | `0` turns the pickers off |
| `FZF_COMMANDS` | commands that open a picker on space. also supported: `vi`, `micro`, `hx`, `emacs`, `bat`, `ls`, `ln`, `rmdir`, `pushd`, `chmod` |
| `SHELL_LANG` | `bash`, `zsh`, `fish` or `sh` |

prompt specifiers: `%u` username, `%h` hostname, `%w` working directory (`$HOME` collapsed to `~`), `%l` shell language, `%s` symbol, `%t` last command duration (hidden under 0.1s), `%?` last exit status (hidden when it was 0), `%%` a literal percent.

## scripts and using tarsh as your shell

```bash
tarsh script.tsh arg1 arg2      # run a script ($0, $1, $#, $@, shift work)
tarsh -c 'ls | wc -l'           # run one line (what editors call when tarsh is $SHELL)
tarsh -l                        # login shell
```

scripts can start with `#!/usr/local/bin/tarsh`. to make tarsh your login shell:

```bash
sudo make install
echo /usr/local/bin/tarsh | sudo tee -a /etc/shells
chsh -s /usr/local/bin/tarsh
```

## how it fits together

| file | job |
| --- | --- |
| `src/main.c` | startup (settings, login profile, tarshrc), prompt loop, `-c` and script modes |
| `src/exec.c` | runs a line: aliases → native or language backend → `&&`/`\|\|` chain → builtins/programs; scripts and `source` |
| `src/syntax.c` | parser for pipelines, redirections, `; && \|\| &`, comments, and "is this line finished?" |
| `src/expand.c` | `~`, `$VAR`, `$(...)`, word splitting, wildcards |
| `src/jobs.c` | process groups, pipes, redirections, Ctrl+Z, `jobs`/`fg`/`bg`/`wait`/`kill` |
| `src/alias.c` | aliases |
| `src/lang.c` | when to hand a line to bash/zsh/fish, and syncing cwd + env back |
| `src/lineedit.c` | raw-mode line editor, key handling, history |
| `src/complete.c` | classic Tab completion (without fzf) |
| `src/fzf.c` | the space / Tab / Ctrl+R pickers |
| `src/builtins.c` | the builtin commands |
| `src/suggest.c` | "command not found" help |
| `src/theme.c` | the theme engine: segments, colors, git/language detection, presets |
| `src/theme_ui.c`, `ui/theme-editor.html` | `theme edit`: the local web server and the editor page (compiled into the binary by `tools/embed.c`) |
| `src/prompts.c`, `src/config.c` | classic prompt rendering; config.t and tarshrc |
| `src/parser.c`, `src/util.c` | word splitting for the pickers; PATH lookup and quoting |

## known limits

- variables are always exported (there's no separate "shell-only" variable), so every `NAME=value` is visible to programs you run.
- functions can't be defined in tarshrc: they'd have to live in bash/zsh/fish. aliases cover most uses.
- a line that goes to bash/zsh/fish only sends back exported variables and the working directory; an `unset` there doesn't carry back.
- very long lines that wrap past the terminal width redraw imperfectly.

## why?

because the best way to understand a tool is to build it yourself. shells feel like magic until you write one.

## license

MIT — do whatever you want with it.
