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

.PHONY: all clean run debug install uninstall

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
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET) $(EDITOR_HEADER)
	rm -rf build
