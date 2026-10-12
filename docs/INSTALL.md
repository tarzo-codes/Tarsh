# Installing tarsh

tarsh is built from source. It takes about a minute and doesn't need root.

## Quick install

```sh
git clone https://github.com/tarzo-codes/Tarsh.git
cd Tarsh
./install.sh
```

Then start it:

```sh
tarsh
```

That's it. The first time it starts, tarsh creates its settings in `~/.config/tarsh/` and shows a short welcome.

## What the installer does

`./install.sh` walks through these steps and tells you what it's doing at each one:

1. **Checks for a C compiler and `make`.** If either is missing it shows the exact command for your system (`apt`, `dnf`, `pacman`, `zypper`, `apk`, `xbps`, `brew`) and offers to run it.
2. **Builds tarsh** and **tests the build**. A plain command and a tarsh-script line both have to work before anything is installed.
3. **Installs** the `tarsh` program, its man page (`man tarsh`) and these guides. The default location is `~/.local`, so no `sudo` is needed.
4. **Checks your `PATH`.** If `~/.local/bin` isn't on it, the installer offers to add one line to `~/.profile`.
5. **Looks for the optional extras** (below) and offers to install any that are missing.
6. **Optionally makes tarsh your login shell**, but only if you ask with `--default-shell`.

Nothing outside the install folder changes unless you say yes to it.

### Installer options

| option | what it does |
| --- | --- |
| *(none)* | install just for you, in `~/.local` |
| `--system` | install for everyone, in `/usr/local` (asks for `sudo`) |
| `--prefix DIR` | install somewhere else |
| `--with-extras` | also install fzf, git, zsh and fish if they're missing |
| `--default-shell` | also make tarsh your login shell |
| `--yes` | answer yes to every question (for scripts) |
| `--uninstall` | remove tarsh again (your settings stay) |

## Recommended extras

tarsh works without these. Each one switches on a feature:

| install | for |
| --- | --- |
| **fzf** | the pickers: `cd` + space, `nvim` + space, Tab, Ctrl+R |
| **git** | the branch and changes in your prompt |
| **bash, zsh, fish** | running other shells' syntax and scripts (bash is usually already there) |
| **a [Nerd Font](https://www.nerdfonts.com)** | icons and powerline shapes in prompt themes. Install one, e.g. *JetBrainsMono Nerd Font*, and choose it in your terminal's settings |

Commands for the common systems:

```sh
sudo apt install fzf git zsh fish          # Debian, Ubuntu, Mint, Pop!_OS
sudo dnf install fzf git zsh fish          # Fedora
sudo pacman -S fzf git zsh fish            # Arch, Manjaro, EndeavourOS
sudo zypper install fzf git zsh fish       # openSUSE
sudo apk add fzf git zsh fish              # Alpine
```

After installing, run **`tarsh doctor`**. It checks everything above and tells you how to fix anything that's missing.

## Building by hand

If you'd rather not use the installer:

```sh
make                         # builds ./tarsh
make check                   # runs the test suite
./tarsh                      # try it without installing
make install PREFIX=~/.local # or: sudo make install   (for /usr/local)
```

Requirements: a C11 compiler (gcc or clang), `make`, and Linux. Nothing else; tarsh has no library dependencies. (macOS isn't supported yet: tarsh uses a few Linux-only system calls.)

## Making tarsh your default shell

Try it for a while first: just type `tarsh` in your usual terminal. When you're happy with it:

```sh
./install.sh --default-shell
```

That adds tarsh to `/etc/shells` and runs `chsh -s` for you. New terminals and logins will start tarsh. As a login shell, tarsh still reads `/etc/profile` and `~/.profile`, so your `PATH` and system settings stay the same.

To switch back at any time:

```sh
chsh -s /bin/bash
```

> **Tip:** keep one terminal open the first time you switch. If something goes wrong you can run `chsh -s /bin/bash` from it.

Some terminals have their own "command" setting that overrides your login shell. If tarsh doesn't start, set that to `tarsh` (or the full path from `command -v tarsh`).

## Updating

```sh
cd Tarsh
git pull
./install.sh
```

Your settings, history, themes, aliases and saved functions in `~/.config/tarsh/` are kept.

## Uninstalling

```sh
./install.sh --uninstall           # or: make uninstall PREFIX=~/.local
```

If tarsh is your login shell, switch back first (`chsh -s /bin/bash`); the uninstaller checks for this. Your settings stay in `~/.config/tarsh/`. Delete that folder to remove them too.

## Troubleshooting

**`tarsh: command not found` after installing.** `~/.local/bin` isn't on your `PATH` yet. Open a new terminal (the installer adds it to `~/.profile`), or add this line to `~/.profile` yourself:
`export PATH="$HOME/.local/bin:$PATH"`

**Icons show as boxes or question marks.** Your terminal font doesn't have Nerd Font glyphs. Install a Nerd Font and select it in the terminal's settings, or switch your theme to plain glyphs: `theme edit`, then *Theme → Glyphs → Unicode*.

**`cd` + space doesn't open a picker.** fzf isn't installed. `tarsh doctor` shows the command to install it.

**A bash script behaves differently.** Scripts with a `#!/bin/bash` line run in bash exactly as before. A bash line typed at the prompt also runs in bash; tarsh prints a one-line note saying so.

**The build fails.** Make sure `gcc` (or `clang`) and `make` are installed. The installer saves the error log as `build.log`; please include it if you open an issue.
