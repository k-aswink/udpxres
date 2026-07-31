CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS  := -lm
TARGET  := udpx

.PHONY: all clean

all: $(TARGET)

$(TARGET): udpx.c
	$(CC) $(CFLAGS) -o $(TARGET) udpx.c $(LDLIBS)

clean:
	rm -f $(TARGET)
