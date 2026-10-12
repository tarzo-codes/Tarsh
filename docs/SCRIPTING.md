# tarsh script

tarsh script is tarsh's own language. It reads like Python, but it's a shell language first: **every line that isn't a statement is a command**, and commands, pipes and redirections work just as they do at the prompt.

```python
#!/usr/bin/env tarsh
# backup.tsh: copy every changed .txt file to a backup folder
dest = "backup"
mkdir -p $dest                          # a command

copied = 0
for f in glob("*.txt"):                 # a statement
    if not exists(f"{dest}/{f}") or size(f) != size(f"{dest}/{f}"):
        cp $f $dest/                    # $f works in commands
        copied += 1

print(f"copied {copied} file(s) to {dest}/")
```

Run it with `tarsh backup.tsh`, or make it executable (`chmod +x backup.tsh`) and run `./backup.tsh`. You can also type any of this at the prompt.

---

## Statements vs. commands

tarsh looks at each line:

| the line looks like | it is |
| --- | --- |
| `x = 5`, `x += 1`, `items[0] = "a"`, `a, b = 1, 2` | an assignment |
| `if …:`, `elif …:`, `else:`, `for … in …:`, `while …:`, `def …(…):` | a block (body indented below) |
| `return …`, `break`, `continue`, `pass`, `global x` | a statement |
| `print(…)`, `name(…)`, `x.upper()`, `2 + 3`, `"text"`, `[1, 2]` | an expression |
| anything else: `ls -la`, `git push`, `echo $x \| wc -l` | a **command** |

Two details make this work. `x = 5` (with spaces) is a statement, while `x=5` (no spaces) is the shell's way of setting a variable, so both habits work. A call like `print(…)` is only a statement when tarsh knows the function; otherwise the line is treated as a command.

## Values

```python
n = 42                  # int
pi = 3.14               # float
name = "ada"            # string
ok = True               # bool (true / false work too)
nothing = None          # none
files = ["a.txt", "b"]  # list
ages = {"ada": 36}      # dict
```

**Strings.** Double-quoted strings understand shell-style `$name`, `${name}` and `$(command)`. f-strings understand `{expressions}`. Single-quoted strings are taken as-is.

```python
print("hello $name, it's $(date +%A)")
print(f"{name.upper()} is {ages['ada']} ({pi:.1f})")
print('no $expansion here')
```

Escapes: `\n` `\t` `\\` `\"` `\$`.

**Numbers mix with text the shell way.** `"5" + 1` is `6`, because the shell hands you text that often holds numbers, while `"a" + "b"` is `"ab"`. When a value goes into a command or `print`, `True` becomes `true` and `None` becomes empty.

## Operators

| kind | operators |
| --- | --- |
| math | `+ - * / // % **` (`/` always gives a float; `//` rounds down) |
| compare | `== != < <= > >=` (chains like `1 < x < 10` work) |
| logic | `and or not` |
| membership | `in`, `not in` (substring, list item, dict key) |
| choose | `a if condition else b` |
| repeat / join | `"ab" * 3`, `[1] * 3`, `[1, 2] + [3]` |

Index with `x[0]` and `x[-1]`; slice with `x[1:3]`, `x[:2]` and `x[2:]` (strings and lists).

## Blocks

```python
if count > 10:
    print("lots")
elif count > 0:
    print("some")
else:
    print("none")

for f in glob("*.md"):
    wc -l $f

for name, age in items(ages):     # unpacking
    print(name, age)

while not ?(ping -c1 example.com):
    sleep(1)
```

`for` loops over a list, the keys of a dict, or the **words** of a string. Use `lines(text)` for lines and `chars(text)` for characters. `break` and `continue` work as in Python.

Indent with spaces (any consistent amount). At the prompt, a line ending in `:` keeps reading, each new line starts indented for you, and an **empty line** runs the block.

## Commands inside scripts

Commands are the point, so they get special help:

| write | to |
| --- | --- |
| `gzip $f` | run a command; `$name` is any variable |
| `out = $(git branch --show-current)` | capture a command's output (trailing newlines removed) |
| `if ?(grep -q TODO notes.txt):` | test whether a command **succeeds** (exit status 0) |
| `status` | the exit status of the last command |
| `argv`, `$1` … `$9` | the script's arguments |

**Variables in commands** follow fish's rule, which avoids the classic shell surprises:

- a **string** variable is always **one** argument, even with spaces in it (`touch $f` with `f = "my file.txt"` makes one file);
- a **list** becomes **one argument per item** (`rm $old_files`);
- variables are never treated as wildcards; use `glob("*.txt")` when you want matching files.

A command that fails doesn't stop the script, which is normal for a shell. Check `status` or use `?(…)` when it matters. A mistake in a *statement* (an unknown name, a type error) stops the script with a message saying where:

```
tarsh: backup.tsh, line 7: 'dset' isn't set (assign it first, e.g. dset = "")
```

## Functions

```python
def greet(name="friend", punctuation="!"):
    return f"hello {name}{punctuation}"

print(greet())                    # hello friend!
print(greet("ada", punctuation="?"))
```

Functions are commands too, which is the shell part:

```python
def deploy(env="staging"):
    echo "deploying to $env"
    git push origin $env

deploy                # → deploying to staging
deploy production
deploy prod | tee deploy.log      # in pipes, with redirections, in the background
```

When run as a command, the arguments arrive as strings in order (missing ones use their default, or `""`), and all of them are in `argv`. The **return value** becomes the result:

- `True` / `False` → exit status 0 / 1;
- an int → that exit status;
- a string → printed (a list → one line per item);
- `None` (or no return) → the status of the last command inside.

Variables assigned inside a function are local. Use `global name` to change a top-level one.

**Keeping functions.** `funcsave NAME` saves a function to `~/.config/tarsh/functions/NAME.tsh`. From then on it loads by itself the first time you use it. `funced NAME` edits one in your editor, `functions` lists them, `functions NAME` shows one, and `functions -e NAME` forgets one.

## Built-in functions

| group | functions |
| --- | --- |
| output / input | `print(*values, sep=" ", end="\n")`, `input(prompt)` |
| types | `str`, `int`, `float`, `bool`, `list`, `type` |
| numbers | `abs`, `round(x, digits)`, `min`, `max`, `sum`, `range(stop)`, `range(start, stop, step)`, `random()`, `random(a, b)` |
| text | `len`, `upper`, `lower`, `strip`, `lstrip`, `rstrip`, `replace(s, old, new)`, `split(s, sep)`, `join(list, sep)`, `startswith`, `endswith`, `find`, `count`, `lines`, `words`, `chars` |
| lists | `append`, `extend`, `insert`, `pop`, `remove`, `sorted(x, reverse=true)`, `reversed`, `find`, `count` |
| dicts | `keys`, `values`, `items`, `get(d, key, default)`, `pop(d, key)` |
| files | `glob(pattern)`, `exists`, `isfile`, `isdir`, `size`, `read(path)`, `write(path, text, append=false)`, `basename`, `dirname`, `abspath`, `cwd()` |
| system | `env(name, default)`, `time()`, `sleep(seconds)`, `exit(status)` |

**Method calls** work for all of them: `name.upper()` is `upper(name)`, and `files.append(f)` is `append(files, f)`. `", ".join(items)` works too.

## Shell variables

Script variables are shell variables, the same ones the prompt uses:

```python
editor = "nvim"
echo $editor            # nvim
export editor           # now programs can see it as $editor too
```

Assigning to a name that's already exported (like `PATH` or `HOME`) updates the exported value, as in any shell.

## Differences from Python

- No classes, imports, exceptions, `lambda`, comprehensions or `with`. Use functions, and commands for everything else.
- `for` over a string gives **words**, not characters (`chars(s)` gives characters).
- `"5" + 1` is `6` (Python would raise an error).
- `True` prints as `true` and `None` as nothing.
- Any line that isn't a statement is run as a command.

## Mixing in bash

Pasted bash works inside `.tsh` files too:

```bash
for i in 1 2 3; do
  echo "bash loop $i"
done
print("back in tarsh script")
```

tarsh notices the bash syntax and runs that part in bash.
