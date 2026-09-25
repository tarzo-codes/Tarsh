# tarsh

> a beginner-friendly Unix shell written in C: fzf pickers where you need them, and bash, zsh or fish when you need a real language.

## what is this?

`tarsh` is a small, beginner-friendly Unix shell built from scratch in C. it started as a way to understand what actually happens when you type a command and hit enter, and it's growing into a shell that helps you find your way around instead of making you remember paths.

## the idea

- **you shouldn't have to remember paths.** type `cd`, `nvim`, `cp`, `mv` or `rm` and press space: an [fzf](https://github.com/junegunn/fzf) picker opens so you can fuzzy-search for the file or folder.
- **you keep the shell language you know.** simple commands run in tarsh itself. anything that needs real shell syntax (pipes, redirects, `&&`, loops, wildcards, `$(...)`) is run by **bash, zsh or fish**, whichever you choose.
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
| `cd␣` | pick a folder (`..` and `~` are at the top) |
| `nvim␣` `vim␣` `nano␣` `cat␣` `less␣` `code␣` | pick files (Tab selects several) |
| `cp␣` `mv␣` | pick what to copy/move, then pick where it goes |
| `rm␣` | pick files to delete (Tab selects several) |
| `sudo nvim␣` | same as `nvim␣` |
| **Tab** | on the first word: pick a command. after that: pick a path for the word you're typing |
| **Ctrl+R** | search your command history |

in a picker: type to filter, **Enter** to choose, **Esc** to close it and type the path yourself. a preview on the right shows the folder contents or the start of the file.

pressing space a second time never opens a picker, so you can always type normally. which commands open a picker is set by `FZF_COMMANDS` in the config.

## shell languages: bash, zsh, fish

```
[you@box] Tarsh (bash) $ ls | wc -l                     # runs through bash
[you@box] Tarsh (bash) $ lang fish
now using fish syntax for pipes, redirects, loops and scripts
[you@box] Tarsh (fish) $ for f in *.c; echo $f; end    # fish syntax now
```

- `lang` lists the languages and shows which are installed; `lang zsh` switches. If a shell isn't installed, tarsh tells you how to install it.
- changes still carry back to tarsh when a line runs in the backend: `cd src && make` leaves you in `src`, and `export X=$(date)`, fish's `set -gx`, or `source venv/bin/activate` keep their variables.
- simple commands (`ls -la`, `git status`, `nvim file.c`) run directly in tarsh, with `$VAR`, `${VAR}`, `$?` and `~` expanded natively.

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

`cd [dir|-]`, `pwd`, `echo [-n]`, `export N=V` (a bare `N=V` works too), `unset`, `lang [name]`, `history [n]`, `help`, `exit [status]`.

## configuration

first run writes `~/.config/tarsh/config.t`:

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

## using tarsh as your shell

`tarsh -c "command"` runs one line and exits, which is what editors and tools expect from `$SHELL`. to try it as your login shell:

```bash
sudo make install
echo /usr/local/bin/tarsh | sudo tee -a /etc/shells
chsh -s /usr/local/bin/tarsh
```

## how it fits together

| file | job |
| --- | --- |
| `src/main.c` | read → decide → run loop, `-c` mode, first-run welcome |
| `src/lineedit.c` | raw-mode line editor, key handling, history |
| `src/fzf.c` | the space / Tab / Ctrl+R pickers |
| `src/lang.c` | when to hand a line to bash/zsh/fish, and syncing cwd + env back |
| `src/parser.c` | tokenizer: quotes, escapes, `$VAR`, `~` |
| `src/builtins.c` | commands that must change the shell's own state |
| `src/suggest.c` | "command not found" help |
| `src/prompts.c` | prompt rendering |
| `src/config.c` | loading `config.t` and handing keys to each module |
| `src/util.c` | PATH lookup, quoting, fork/exec/wait |

## known limits

- lines run in bash/zsh/fish don't load your `.bashrc`/`.zshrc`, so aliases and functions defined there aren't available (fish does read `config.fish`).
- only exported variables come back from a backend line, and unsetting a variable there doesn't carry back.
- `history | grep x` runs bash's own `history` (which is empty) because of the pipe. use Ctrl+R instead.
- no job control yet (`Ctrl+Z`, `fg`, `bg`), and very long lines that wrap the terminal redraw imperfectly.

## why?

because the best way to understand a tool is to build it yourself. shells feel like magic until you write one.

## license

MIT — do whatever you want with it.
