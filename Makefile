CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS  := -lm
TARGET  := netprobe

.PHONY: all clean

all: $(TARGET)

$(TARGET): netprobe.c
	$(CC) $(CFLAGS) -o $(TARGET) netprobe.c $(LDLIBS)

clean:
	rm -f $(TARGET)
