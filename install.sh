#!/bin/sh
# tarsh installer.
#
#   ./install.sh                  install just for you, in ~/.local (no sudo)
#   ./install.sh --system         install for everyone, in /usr/local (asks for sudo)
#   ./install.sh --prefix DIR     install somewhere else
#   ./install.sh --with-extras    also install fzf, git, zsh and fish if they're missing
#   ./install.sh --default-shell  also make tarsh your login shell
#   ./install.sh --yes            don't ask questions (answer yes)
#   ./install.sh --uninstall      remove tarsh again (your settings stay)
#
# Works with sh, bash, zsh and dash. Nothing outside the install folder
# is touched unless you ask for it.

set -eu

PREFIX=""
SYSTEM=0
EXTRAS=0
DEFAULT_SHELL=0
ASSUME_YES=0
UNINSTALL=0

while [ $# -gt 0 ]; do
  case "$1" in
    --system) SYSTEM=1 ;;
    --prefix) shift; PREFIX="${1:?--prefix needs a folder}" ;;
    --prefix=*) PREFIX="${1#--prefix=}" ;;
    --with-extras) EXTRAS=1 ;;
    --default-shell) DEFAULT_SHELL=1 ;;
    --yes|-y) ASSUME_YES=1 ;;
    --uninstall) UNINSTALL=1 ;;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "install.sh: unknown option $1 (see ./install.sh --help)" >&2; exit 2 ;;
  esac
  shift
done

if [ -z "$PREFIX" ]; then
  if [ "$SYSTEM" = 1 ]; then PREFIX=/usr/local; else PREFIX="$HOME/.local"; fi
fi

# --- output helpers ---------------------------------------------------------
if [ -t 1 ]; then
  BOLD=$(printf '\033[1m'); DIM=$(printf '\033[2m'); GREEN=$(printf '\033[32m')
  YELLOW=$(printf '\033[33m'); RED=$(printf '\033[31m'); RESET=$(printf '\033[0m')
else
  BOLD=""; DIM=""; GREEN=""; YELLOW=""; RED=""; RESET=""
fi
step() { printf '\n%s==>%s %s%s%s\n' "$GREEN" "$RESET" "$BOLD" "$*" "$RESET"; }
ok()   { printf '  %s✓%s %s\n' "$GREEN" "$RESET" "$*"; }
warn() { printf '  %s!%s %s\n' "$YELLOW" "$RESET" "$*"; }
fail() { printf '  %s✗%s %s\n' "$RED" "$RESET" "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

ask() {
  # ask "question" -> 0 for yes
  if [ "$ASSUME_YES" = 1 ]; then return 0; fi
  if [ ! -t 0 ]; then return 1; fi
  printf '  %s [y/N] ' "$1"
  read -r answer || return 1
  case "$answer" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

# Runs a command as root when needed.
as_root() {
  if [ "$(id -u)" = 0 ]; then "$@"
  elif have sudo; then sudo "$@"
  else fail "this needs root, and sudo isn't available: run it as root instead"
  fi
}

needs_root_for() {
  # Is $1 (or its nearest existing parent) writable by us?
  dir=$1
  while [ ! -d "$dir" ]; do dir=$(dirname "$dir"); done
  [ ! -w "$dir" ]
}

# --- package manager ------------------------------------------------------------
PM=""
for candidate in apt-get dnf yum pacman zypper apk xbps-install brew; do
  if have "$candidate"; then PM=$candidate; break; fi
done

install_command() {
  # install_command PACKAGE... -> the command to install them here
  case "$PM" in
    apt-get) echo "sudo apt-get install -y $*" ;;
    dnf) echo "sudo dnf install -y $*" ;;
    yum) echo "sudo yum install -y $*" ;;
    pacman) echo "sudo pacman -S --needed $*" ;;
    zypper) echo "sudo zypper install -y $*" ;;
    apk) echo "sudo apk add $*" ;;
    xbps-install) echo "sudo xbps-install -y $*" ;;
    brew) echo "brew install $*" ;;
    *) echo "install $* with your package manager" ;;
  esac
}

install_packages() {
  case "$PM" in
    apt-get) as_root apt-get install -y "$@" ;;
    dnf) as_root dnf install -y "$@" ;;
    yum) as_root yum install -y "$@" ;;
    pacman) as_root pacman -S --needed --noconfirm "$@" ;;
    zypper) as_root zypper install -y "$@" ;;
    apk) as_root apk add "$@" ;;
    xbps-install) as_root xbps-install -y "$@" ;;
    brew) brew install "$@" ;;
    *) return 1 ;;
  esac
}

build_packages() {
  case "$PM" in
    apt-get) echo "build-essential" ;;
    dnf|yum) echo "gcc make" ;;
    pacman) echo "base-devel" ;;
    zypper) echo "gcc make" ;;
    apk) echo "build-base" ;;
    xbps-install) echo "base-devel" ;;
    brew) echo "make" ;;
    *) echo "a C compiler and make" ;;
  esac
}

cd "$(dirname "$0")"
BIN="$PREFIX/bin/tarsh"

# --- uninstall --------------------------------------------------------------------
if [ "$UNINSTALL" = 1 ]; then
  step "Removing tarsh from $PREFIX"
  login_shell=$(getent passwd "$(id -un)" 2>/dev/null | cut -d: -f7 || true)
  if [ "${login_shell:-}" = "$BIN" ]; then
    fail "tarsh is your login shell. switch back first, e.g.: chsh -s /bin/bash   (then run this again)"
  fi
  run=""
  if needs_root_for "$PREFIX/bin"; then run=as_root; fi
  $run rm -f "$BIN" "$PREFIX/share/man/man1/tarsh.1"
  $run rm -rf "$PREFIX/share/doc/tarsh"
  ok "removed $BIN, its man page and docs"
  if [ -f /etc/shells ] && grep -qx "$BIN" /etc/shells; then
    if ask "remove $BIN from /etc/shells too?"; then
      tmp=$(mktemp)
      grep -vxF "$BIN" /etc/shells > "$tmp" && as_root cp "$tmp" /etc/shells && ok "removed from /etc/shells"
      rm -f "$tmp"
    fi
  fi
  if [ -f "$HOME/.profile" ] && grep -q "added by the tarsh installer" "$HOME/.profile"; then
    printf '  the installer added a PATH line to ~/.profile; remove it if you like (it says "added by the tarsh installer").\n'
  fi
  printf '\n  your settings are still in ~/.config/tarsh (delete that folder to remove them too).\n'
  exit 0
fi

printf '%s\n' "${BOLD}tarsh installer${RESET} ${DIM}(installing to $PREFIX)${RESET}"

# --- 1. build tools -------------------------------------------------------------
step "Checking build tools"
CC_FOUND=""
for compiler in cc gcc clang; do
  if have "$compiler"; then CC_FOUND=$compiler; break; fi
done
if [ -z "$CC_FOUND" ] || ! have make; then
  packages=$(build_packages)
  warn "tarsh is built from source and needs a C compiler and make."
  if [ "$PM" != "" ] && ask "install them now? ($(install_command "$packages"))"; then
    # shellcheck disable=SC2086
    install_packages $packages || fail "couldn't install $packages"
  else
    fail "install them with:  $(install_command "$packages")   then run ./install.sh again"
  fi
  for compiler in cc gcc clang; do
    if have "$compiler"; then CC_FOUND=$compiler; break; fi
  done
fi
ok "compiler: $CC_FOUND, make: $(command -v make)"

# --- 2. build & self-test -------------------------------------------------------
step "Building tarsh"
make clean >/dev/null 2>&1 || true
if ! make CC="$CC_FOUND" >build.log 2>&1; then
  tail -n 20 build.log >&2
  fail "the build failed (full log: build.log)"
fi
rm -f build.log
ok "built ./tarsh ($(./tarsh --version))"

if [ "$(./tarsh -c 'echo ok')" != "ok" ]; then fail "the self-test failed: ./tarsh -c 'echo ok' didn't print ok"; fi
if [ "$(./tarsh -c 'x = 6 * 7
print(x)')" != "42" ]; then fail "the self-test of tarsh script failed"; fi
ok "self-test passed"

# --- 3. install -----------------------------------------------------------------
step "Installing"
if needs_root_for "$PREFIX/bin"; then
  as_root make install PREFIX="$PREFIX" >/dev/null
else
  make install PREFIX="$PREFIX" >/dev/null
fi
ok "installed $BIN"
ok "man page: man tarsh    docs: $PREFIX/share/doc/tarsh"

case ":$PATH:" in
  *":$PREFIX/bin:"*) ok "$PREFIX/bin is on your PATH" ;;
  *)
    warn "$PREFIX/bin isn't on your PATH yet, so typing tarsh won't find it."
    profile="$HOME/.profile"
    line="export PATH=\"$PREFIX/bin:\$PATH\""
    if ask "add it to $profile for you?"; then
      printf '\n# added by the tarsh installer\n%s\n' "$line" >> "$profile"
      ok "added to $profile (open a new terminal for it to take effect)"
    else
      printf '    add this line to %s:\n      %s\n' "$profile" "$line"
    fi
    ;;
esac

# --- 4. optional extras ---------------------------------------------------------
step "Optional extras"
missing=""
check_extra() {
  # check_extra COMMAND PACKAGE WHY
  if have "$1"; then ok "$1: found ($3)"; else warn "$1: not found ($3)"; missing="$missing $2"; fi
}
check_extra fzf fzf "file pickers: cd + space, Tab, Ctrl+R"
check_extra git git "branch and changes in the prompt"
check_extra bash bash "runs bash syntax and scripts"
check_extra zsh zsh "runs zsh syntax"
check_extra fish fish "runs fish syntax"
if [ -n "$missing" ]; then
  if [ "$EXTRAS" = 1 ] || ask "install the missing ones now? ($(install_command $missing))"; then
    # shellcheck disable=SC2086
    if install_packages $missing; then ok "installed:$missing"; else warn "couldn't install:$missing"; fi
  else
    printf '    whenever you like: %s\n' "$(install_command $missing)"
  fi
fi
printf '    icons in themes need a Nerd Font in your terminal: https://www.nerdfonts.com\n'

# --- 5. default shell -------------------------------------------------------------
if [ "$DEFAULT_SHELL" = 1 ]; then
  step "Making tarsh your login shell"
  if [ -f /etc/shells ] && ! grep -qx "$BIN" /etc/shells; then
    echo "$BIN" | as_root tee -a /etc/shells >/dev/null
    ok "added $BIN to /etc/shells"
  fi
  if chsh -s "$BIN"; then
    ok "done: new terminals and logins start tarsh"
    printf '    to switch back later: chsh -s /bin/bash\n'
  else
    warn "chsh didn't work; try: chsh -s $BIN"
  fi
fi

# --- done ---------------------------------------------------------------------------
step "All set"
cat <<DONE
  start it:          tarsh
  check your setup:  tarsh doctor
  the guide:         $PREFIX/share/doc/tarsh/GUIDE.md   (or: man tarsh)
  make it yours:     theme edit   (prompt)   config   (aliases, startup)
DONE
if [ "$DEFAULT_SHELL" = 0 ]; then
  printf '  make it your login shell later: ./install.sh --default-shell\n'
fi
