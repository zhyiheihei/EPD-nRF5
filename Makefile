CC      ?= gcc
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)

CFLAGS = -O2 -Wall -IGUI -D__HEAP_SIZE=32768 $(SDL_CFLAGS)
LDLIBS = $(SDL_LIBS)

SRCS = GUI/Adafruit_GFX.c GUI/u8g2_font.c GUI/fonts.c GUI/GUI.c GUI/Lunar.c emulator.c
OBJS = $(SRCS:%.c=build/emu/%.o)
TARGET = build/emu/emulator

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDLIBS)

build/emu/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build

.PHONY: all clean
