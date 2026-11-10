# Desktop:  make
# Device:   make CROSS=1 TC=<toolchain dir> FFMPEG=<ffmpeg install prefix> VERSION=1.0.0
VERSION ?= dev
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -std=gnu11 -funwind-tables
CFLAGS += -DAPP_VERSION='"$(VERSION)"'
LDFLAGS += -rdynamic
SRC := main.c core.c player.c ui.c net.c
ifdef CROSS
CC := $(TC)/bin/aarch64-nextui-linux-gnu-gcc
SYSROOT := $(TC)/aarch64-nextui-linux-gnu/libc
CFLAGS += -mcpu=cortex-a53 --sysroot=$(SYSROOT) -I$(SYSROOT)/usr/include/SDL2 -I$(FFMPEG)/include
LDLIBS := -L$(FFMPEG)/lib -lavformat -lavcodec -lswresample -lswscale -lavutil -lSDL2 -lpthread -lm -ldl
LDFLAGS += --sysroot=$(SYSROOT)
else
CFLAGS += $(shell pkg-config --cflags sdl2 libavformat libavcodec libswresample libswscale libavutil)
LDLIBS := $(shell pkg-config --libs sdl2 libavformat libavcodec libswresample libswscale libavutil) -lm -ldl
endif

sleep-shuffle: $(SRC) core.h player.h ui.h net.h font.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

font.h: ../tools/make_font.py
	python3 ../tools/make_font.py $(FONT) font.h font_small:24 font_med:32 font_big:64

clean:
	rm -f sleep-shuffle
