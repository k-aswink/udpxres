CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS  := -lm
TARGET  := udpxres 

.PHONY: all clean

all: $(TARGET)

$(TARGET): udpxres.c
	$(CC) $(CFLAGS) -o $(TARGET) udpxres.c $(LDLIBS)

clean:
	rm -f $(TARGET)
