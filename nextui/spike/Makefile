# Desktop:  make
# Device:   make CROSS=1 TC=<toolchain dir> FFMPEG=<ffmpeg install prefix>
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11
ifdef CROSS
CC := $(TC)/bin/aarch64-nextui-linux-gnu-gcc
SYSROOT := $(TC)/aarch64-nextui-linux-gnu/libc
CFLAGS += -mcpu=cortex-a53 --sysroot=$(SYSROOT) -I$(SYSROOT)/usr/include/SDL2 -I$(FFMPEG)/include
LDLIBS := -L$(FFMPEG)/lib -lavformat -lavcodec -lswresample -lswscale -lavutil -lSDL2 -lpthread -lm -ldl
LDFLAGS += --sysroot=$(SYSROOT)
else
CFLAGS += $(shell pkg-config --cflags sdl2 libavformat libavcodec libswresample libswscale libavutil)
LDLIBS := $(shell pkg-config --libs sdl2 libavformat libavcodec libswresample libswscale libavutil) -lm
endif

sleep-shuffle-test: main.c net.c net.h font.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ main.c net.c $(LDLIBS)

font.h: ../tools/make_font.py
	python3 ../tools/make_font.py $(FONT) font.h font_small:20 font_big:56

clean:
	rm -f sleep-shuffle-test
