# Customizing tarsh

Everything personal lives in `~/.config/tarsh/`:

| file | what's in it | edit with |
| --- | --- | --- |
| `tarshrc` | commands run at startup: aliases, abbreviations, variables | `config` |
| `config.t` | settings (below) | `config settings` |
| `theme.t` | your prompt theme | `theme edit` (browser) or `config theme` |
| `universal` | universal variables (`set -U`) | `set -U NAME value` |
| `functions/` | saved functions | `funcsave NAME`, `funced NAME` |
| `history` | your command history | `history clear`, `history delete TEXT` |

## tarshrc: your startup file

tarsh doesn't read `~/.bashrc`. Your startup file is `~/.config/tarsh/tarshrc`, written in tarsh's own syntax (commands and tarsh script). The first run creates it with a few useful aliases. A typical one:

```sh
alias ll='ls -lh'
alias ..='cd ..'
abbr gco git checkout
export EDITOR=nvim
set -x PATH "$HOME/.local/bin" $PATH
lang fish
```

`import-rc` copies the `alias` and `export` lines from `~/.bashrc`, `~/.bash_aliases`, `~/.zshrc` and `~/.profile`. Login shells also run `/etc/profile` and `~/.profile` through `sh` first, so system settings keep working.

## config.t: settings

```ini
FORMAT=[%u@%h] %w (%l) %s
DEPTH=1
SHOW_TIME=1
SYMBOL=$
FZF=1
FZF_COMMANDS=cd,cp,mv,rm,nvim,vim,nano,cat,less,code,source
SHELL_LANG=bash
EXPLAIN=1
HIGHLIGHT=1
AUTOSUGGEST=1
GREETING=
```

| key | meaning |
| --- | --- |
| `FORMAT` | the classic prompt (used when no theme is on): `%u` user, `%h` host, `%w` folder, `%l` shell language, `%s` symbol, `%t` last duration, `%?` last exit status, `%%` a percent sign |
| `DEPTH` | how many folders `%w` shows (`0` = the whole path) |
| `SHOW_TIME`, `SYMBOL` | whether `%t` shows; what `%s` shows |
| `FZF`, `FZF_COMMANDS` | turn the pickers off, or choose which commands open one on space (also supported: `vi`, `micro`, `hx`, `emacs`, `bat`, `ls`, `ln`, `rmdir`, `pushd`, `chmod`) |
| `SHELL_LANG` | `bash`, `zsh`, `fish` or `sh`: the shell used when tarsh can't tell which one a line was written for (`lang NAME` changes it for the session) |
| `EXPLAIN` | `0` hides the one-line notes after bash/zsh/fish commands (`explain off` for the session) |
| `HIGHLIGHT`, `AUTOSUGGEST` | syntax highlighting and grey suggestions while typing |
| `GREETING` | a line of text to show at startup |

## Prompt themes

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

### The theme editor

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

### Theme commands

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

