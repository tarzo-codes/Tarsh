CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2
SRC     := $(wildcard src/*.c)
TARGET  := tarsh
PREFIX  ?= /usr/local

.PHONY: all clean run debug install uninstall

all: $(TARGET)

$(TARGET): $(SRC) $(wildcard src/*.h)
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
