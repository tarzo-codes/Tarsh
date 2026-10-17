# The tarsh guide

A hands-on tour. Open tarsh and try things as you read. Nothing here can break anything.

- [1. The basics](#1-the-basics)
- [2. Finding files without typing paths](#2-finding-files-without-typing-paths)
- [3. Typing less](#3-typing-less)
- [4. Combining commands](#4-combining-commands)
- [5. Seeing what a command will do](#5-seeing-what-a-command-will-do)
- [6. Running programs](#6-running-programs)
- [7. Variables](#7-variables)
- [8. tarsh script](#8-tarsh-script)
- [9. Commands from bash, zsh and fish](#9-commands-from-bash-zsh-and-fish)
- [10. Making tarsh yours](#10-making-tarsh-yours)
- [11. Cheat sheet](#11-cheat-sheet)

---

## 1. The basics

You type a command and press Enter:

```
$ ls
$ cd Documents
$ pwd
```

If you mistype, tarsh tries to help:

```
$ sl
tarsh: command not found: sl
  did you mean: ls
$ cls
tarsh: command not found: cls
  on Linux that's: clear
```

As you type, the command name turns **green** when tarsh knows it and **red** when it doesn't, so you can spot typos before pressing Enter.

`help` shows everything at a glance. `tarsh doctor` checks your setup.

## 2. Finding files without typing paths

If [fzf](INSTALL.md#recommended-extras) is installed, type one of these commands and press the **space bar**. A picker opens:

| type | then space | you get |
| --- | --- | --- |
| `cd` | | a folder picker (`..` and `~` are at the top) |
| `nvim`, `vim`, `nano`, `cat`, `less`, `code` | | a file picker (Tab selects several) |
| `cp` or `mv` | | pick what to copy, then where it goes |
| `rm` | | pick files to delete |

In a picker, **type** to narrow it down, use the arrow keys to move, **Enter** to choose, and **Esc** to close it and type the path yourself. When you paste or type quickly, the picker stays out of your way.

Without fzf, **Tab** completes commands and paths the classic way.

## 3. Typing less

**Suggestions.** As you type, tarsh shows the rest of a matching earlier command in grey. Press **→** (or End) to accept it, or **Alt+→** for one word at a time.

**History.** **↑** and **↓** step through earlier commands. Type part of a command first, then press ↑, to search for commands containing it. **Ctrl+R** opens a searchable list (with fzf). `history`, `history search TEXT`, `history delete TEXT` and `history clear` manage it.

**Tab** completes commands, files and folders. After a `-` it lists the command's **options with what each does**:

```
$ ls --al<Tab>
  --all                    do not ignore entries starting with .
  --almost-all             do not list implied . and ..
```

**Abbreviations** expand as you type:

```
$ abbr gco git checkout
$ gco<space>          → becomes: git checkout
```

**Aliases** are shortcuts that stay short:

```
$ alias ll='ls -lh'
```

Put abbreviations and aliases in your startup file (`config`) to keep them.

**Folders.** Typing a folder's path on its own goes there (`../`, `~/code/`). `prevd` / `nextd` (or **Alt+←** / **Alt+→** on an empty line) move back and forward through the folders you've visited. `cdh` lets you pick a recent one, and `cd -` returns to the last folder.

**Alt+.** inserts the last word of the previous command.

## 4. Combining commands

| you type | what it does |
| --- | --- |
| `a \| b` | send a's output into b: `ls \| grep txt` |
| `a > file` | save the output to a file (replacing it) |
| `a >> file` | add the output to the end of a file |
| `a < file` | read input from a file |
| `a 2> file`, `a &> file` | save the error messages, or output and errors together |
| `a && b` | run b only if a worked |
| `a \|\| b` | run b only if a failed |
| `a ; b` | run a, then b |
| `*.txt` | every .txt file here |
| `~` | your home folder |
| `$(a)` | use a's output in another command: `echo "today is $(date)"` |

fish's style works too: `make; and echo built`, `test -f x; or echo missing`, `not grep -q TODO notes.txt`.

## 5. Seeing what a command will do

Long chains of `|`, `>>` and `<<` are hard to read. As you type one, tarsh explains it under the prompt, step by step, and says what you'll get at the end. Nothing is run to do this:

```
$ ps aux | grep -i nginx | awk '{print $2}' | sort -n >> pids.txt
  ↳ ps: lists running programs → grep: keeps lines matching /nginx/, ignoring case
    → awk: prints column 2 → sort: sorts the lines as numbers, adds the output to the end of pids.txt
    ⇒ added to the end of pids.txt (nothing on screen)
```

**Live preview.** When every command in the line is one you've listed in `LIVE_COMMANDS` (grep, awk, sed, cut, sort, uniq, head, tail, wc, tr, jq and a few more by default), tarsh also runs it as you type and shows the first lines of real output. It runs when you pause, so typing stays fast. For grep, the matches are **highlighted**, which makes it a live regex tester:

```
$ grep -E '(ada|grace)' people.csv
  ▸ live: 2 lines
  │ 1,ada,london
  │ 3,grace,new york
```

The preview runs in a **sandbox**: it can't create, change, delete or move files, use the network, signal other programs or reach your terminal, and it's stopped after a second. A mistyped command, or an awk program that calls `system("rm …")`, can't do anything harmful. Lines that write files (`>`, `>>`, `sed -i`, `sort -o`, `find -delete`), chain several commands (`;`, `&&`) or use `$(…)` are only explained, never previewed.

**`explain`** prints the full breakdown, including what each regex means, any time:

```
$ explain grep -E '^[0-9]{3}-[0-9]{4}$' phones.txt
1. grep -E '^[0-9]{3}-[0-9]{4}$' phones.txt
     keeps lines matching /^[0-9]{3}-[0-9]{4}$/ in phones.txt
     /^[0-9]{3}-[0-9]{4}$/ means: the line starts, then exactly 3 digits, then "-", then exactly 4 digits, then the line ends
result: the matching lines
```

| command | does |
| --- | --- |
| `explain 'LINE'` | the step-by-step breakdown of a command line |
| `live` | show the preview settings and the previewed commands |
| `live on` / `live off` | turn the live preview on or off for this session |
| `live add jq` / `live remove ls` | change which commands are previewed (this session) |
| `live test 'LINE'` | run the preview once and print it |
| `hints on` / `hints off` | the explanation under the prompt |

To keep changes, set `HINTS`, `LIVE`, `LIVE_COMMANDS`, `LIVE_LINES` and `LIVE_DELAY` in `config settings` (see [CUSTOMIZING.md](CUSTOMIZING.md)).

## 6. Running programs

| key / command | what it does |
| --- | --- |
| **Ctrl+C** | stop the running program |
| **Ctrl+Z** | pause it |
| `fg` | bring the paused program back |
| `bg` | let it keep running in the background |
| `cmd &` | start something in the background right away |
| `jobs` | list paused and background programs |
| `kill %1` | stop job 1 |
| `wait` | wait for background programs to finish |

When a background program finishes, tarsh tells you at the next prompt.

## 7. Variables

```
$ name=world              # a variable for this shell
$ echo "hello $name"
$ export EDITOR=nvim      # exported: programs you run can see it too
$ set fruits apple pear   # a list (fish style)
$ echo $fruits
$ set -U THEME_COLOR teal # universal: saved, and shared by every open tarsh
```

Like fish, tarsh **never splits a variable at spaces**. If `f="my file.txt"`, then `rm $f` removes one file named `my file.txt`, not two files called `my` and `file.txt`. Lists are how you pass several arguments.

## 8. tarsh script

tarsh has its own little language, for when a few commands aren't enough. It reads like Python, but any line that isn't a statement is just a command:

```
$ for f in glob("*.log"):
...     if size(f) > 1000000:
...         gzip $f
...         print(f"packed {f}")
...
```

At the prompt, a line ending in `:` keeps reading. The next lines are indented for you, and an empty line runs the block. Save it in a `.tsh` file and run it with `tarsh myscript.tsh`.

```
def greet(name="friend"):
    echo "hello $name"

greet            # functions run like commands...
greet ada
print(greet)     # ...and are values too
```

`funcsave greet` keeps a function for good; it loads by itself next time you use it. The full language is in **[SCRIPTING.md](SCRIPTING.md)**.

## 9. Commands from bash, zsh and fish

Found a command online written for bash? Just paste it. tarsh recognises bash, zsh and fish syntax, runs it in that shell, and tells you what happened in one line:

```
$ for i in 1 2 3; do echo $i; done
1
2
3
↳ ran in bash: a for-loop: ran the commands between `do` and `done` once for each item.
  tarsh script: for x in ["a", "b"]:  (then indent the body)

$ for d in */; do cd "$d"; break; done
↳ ran in bash: a for-loop: ran the commands between `do` and `done` once for each item. It changed folder, so you're now in ~/project/src.
```

If the command changes folder or sets exported variables, those changes come back to tarsh. So `source venv/bin/activate` works, and so does `export PATH=...` from a tutorial.

Scripts work too. A script with a `#!` line runs as usual, and one without (`./setup.sh`) is run by the right shell, with a note. `explain off` hides the notes.

## 10. Making tarsh yours

| command | what it opens |
| --- | --- |
| `config` | **tarshrc**, your startup file: aliases, abbreviations, variables, anything to run at start |
| `config settings` | **config.t**: prompt, pickers, highlighting, suggestions, greeting |
| `theme edit` | the **theme editor** in your browser: design your prompt with git, languages, colors and icons |
| `theme list` / `theme use NAME` | built-in prompt themes |
| `theme starship` | use your [starship](https://starship.rs) prompt (on by itself when you have a `starship.toml`) |
| `lang fish` | which shell runs bash/zsh/fish syntax when tarsh can't tell |
| `import-rc` | copy your aliases and exports from `~/.bashrc` / `~/.zshrc` |

tarsh doesn't read `~/.bashrc`. Your settings live in `~/.config/tarsh/`, and `import-rc` brings the useful parts over. Everything about settings and themes is in **[CUSTOMIZING.md](CUSTOMIZING.md)**.

## 11. Cheat sheet

```
cd + space        pick a folder          Tab           complete / pick
nvim + space      pick files             Ctrl+R        search history
→                 accept suggestion      ↑             search what you typed
Alt+← / Alt+→     previous/next folder   Alt+.         last argument
Ctrl+C            stop                   Ctrl+Z        pause (fg / bg)
Ctrl+A / Ctrl+E   start / end of line    Ctrl+W        delete a word
Ctrl+L            clear the screen       Ctrl+D        exit

help · explain 'LINE' · live · tarsh doctor · config · theme edit · lang · history · jobs · abbr · set
```
