CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2
SRC     := $(wildcard src/*.c)
TARGET  := tarsh
PREFIX  ?= /usr/local
LDLIBS  ?= -lm

# The theme editor page is compiled into the binary.
EDITOR_PAGE   := ui/theme-editor.html
EDITOR_HEADER := src/theme_editor_html.h
EMBED         := build/embed

BINDIR  := $(PREFIX)/bin
MANDIR  := $(PREFIX)/share/man/man1
DOCDIR  := $(PREFIX)/share/doc/tarsh

.PHONY: all clean run debug install uninstall check

all: $(TARGET)

$(TARGET): $(SRC) $(wildcard src/*.h) $(EDITOR_HEADER)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDLIBS)

$(EMBED): tools/embed.c
	@mkdir -p build
	$(CC) -O2 $< -o $@

$(EDITOR_HEADER): $(EDITOR_PAGE) $(EMBED)
	$(EMBED) theme_editor_html < $(EDITOR_PAGE) > $@

debug: CFLAGS += -g -O0 -fsanitize=address,undefined
debug: clean $(TARGET)

run: $(TARGET)
	./$(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR) $(DESTDIR)$(DOCDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -m 644 docs/tarsh.1 $(DESTDIR)$(MANDIR)/tarsh.1
	install -m 644 README.md docs/INSTALL.md docs/GUIDE.md docs/SCRIPTING.md docs/CUSTOMIZING.md $(DESTDIR)$(DOCDIR)/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET) $(DESTDIR)$(MANDIR)/tarsh.1
	rm -rf $(DESTDIR)$(DOCDIR)

# Runs the test suite against the freshly built binary.
check: $(TARGET)
	@sh tests/run.sh ./$(TARGET)

clean:
	rm -f $(TARGET) $(EDITOR_HEADER)
	rm -rf build
