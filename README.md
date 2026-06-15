# tarsh

> a no-frills Unix shell written in C — not here to replace bash, just here to understand it.

## what is this?

`tarsh` is a minimal Unix shell built from scratch in C as a learning project. the goal isn't to ship a production shell — it's to understand what actually happens when you type a command and hit enter.

> 🚧 early stage — just getting started.


**requirements:** `gcc`, any Unix-like system (Linux/macOS)

```bash
git clone https://github.com/yourusername/tarsh.git
cd tarsh
gcc src/main.c src/parser.c -o tarsh
./tarsh
```

## why?

because the best way to understand a tool is to build it yourself. shells feel like magic until you write one.

## license

MIT — do whatever you want with it.
