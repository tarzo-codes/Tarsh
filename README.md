# tarsh

> a no-frills Unix shell written in C — not here to replace bash, just here to understand it.

## what is this?

`tarsh` is a minimal Unix shell built from scratch in C as a learning project. the goal isn't to ship a production shell — it's to understand what actually happens when you type a command and hit enter.

> 🚧 early stage — just getting started.

## current progress

- [x] input parsing and tokenization

coming next:
- [ ] command execution via `execvp`
- [ ] built-in commands (`cd`, `exit`, `help`)
- [ ] piping (`|`)
- [ ] i/o redirection (`>`, `<`, `>>`)
- [ ] background processes (`&`)

## build & run

**requirements:** `gcc`, any Unix-like system (Linux/macOS)

```bash
git clone https://github.com/yourusername/tarsh.git
cd tarsh
gcc src/main.c src/parser.c -o tarsh
./tarsh
```

## project structure

```
tarsh/
├── modules/         # optional extensions — add your own stuff here
├── functions/       # custom shell features (coming soon)
├── src/
│   ├── main.c       # entry point, REPL loop
│   ├── parser.c     # tokenizer and input handling
│   └── parser.h     # parser declarations
└── README.md
```

## why?

because the best way to understand a tool is to build it yourself. shells feel like magic until you write one.

## license

MIT — do whatever you want with it.
