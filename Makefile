# Makefile for pacemaker_sim - FAKE synthetic pacemaker simulator.
# NOT A REAL MEDICAL DEVICE. See README.md.

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -std=c99 -Iinclude -g
LDFLAGS ?=

SRC := src/main.c \
       src/pacer_core.c \
       src/sensing.c \
       src/modes.c \
       src/telemetry.c \
       src/battery.c \
       src/eeprom.c \
       src/arrhythmia.c

OBJ := $(SRC:src/%.c=build/%.o)
BIN := bin/pacemaker_sim

.PHONY: all clean run

all: $(BIN)

$(BIN): $(OBJ) | bin
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: src/%.c include/pacer.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

bin:
	mkdir -p bin

run: all
	./$(BIN)

clean:
	rm -rf build bin pacer_nvram.bin
