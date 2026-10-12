#!/bin/sh
# Runs every tests/cases/*.tsh with tarsh and compares the output (stdout
# and stderr together) with the matching .out file.
#   sh tests/run.sh ./tarsh            run all
#   UPDATE=1 sh tests/run.sh ./tarsh   rewrite the .out files (check the diff!)

set -u
TARSH=$(cd "$(dirname "${1:-./tarsh}")" && pwd)/$(basename "${1:-./tarsh}")
HERE=$(cd "$(dirname "$0")" && pwd)
pass=0
failed=0

for case_file in "$HERE"/cases/*.tsh; do
  name=$(basename "$case_file" .tsh)
  expected="$HERE/cases/$name.out"
  work=$(mktemp -d)
  # A clean home so no personal config changes the results.
  mkdir -p "$work/home"
  cp "$case_file" "$work/test.tsh"
  actual=$(cd "$work" && HOME="$work/home" TERM=dumb "$TARSH" test.tsh one two 2>&1; echo "[exit $?]")
  if [ "${UPDATE:-0}" = 1 ]; then
    printf '%s\n' "$actual" > "$expected"
  fi
  if [ -f "$expected" ] && [ "$actual" = "$(cat "$expected")" ]; then
    pass=$((pass + 1))
    printf '  ok    %s\n' "$name"
  else
    failed=$((failed + 1))
    printf '  FAIL  %s\n' "$name"
    printf '%s\n' "$actual" > "$work/actual"
    diff -u "$expected" "$work/actual" | sed 's/^/        /' | head -40
  fi
  rm -rf "$work"
done

printf '\n%d passed, %d failed\n' "$pass" "$failed"
[ "$failed" = 0 ]
