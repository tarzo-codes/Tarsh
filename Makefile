CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2
SRC     := src/main.c src/parser.c src/prompts.c src/builtins.c
TARGET  := tarsh
PREFIX  ?= /usr/local

.PHONY: all clean run debug install uninstall

all: $(TARGET)

$(TARGET): $(SRC) src/parser.h src/prompts.h src/builtins.h
	$(CC) $(CFLAGS) $(SRC) -o $@

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
	rm -f $(TARGET)
