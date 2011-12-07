CC := gcc
RM := rm

AGAR_CFLAGS := $(shell agar-config --cflags)
AGAR_LDFLAGS := $(shell agar-config --libs)

SDL_CFLAGS := $(shell sdl-config --cflags)
SDL_LDFLAGS := $(shell sdl-config --libs)

SDLFFMPEG_CFLAGS :=
SDLFFMPEG_LDFLAGS := -lSDL_ffmpeg

CFLAGS := -g -O2 -Wall -std=gnu99
CFLAGS += $(AGAR_CFLAGS) $(SDL_CFLAGS) $(SDLFFMPEG_CFLAGS)

CPPFLAGS := -DUSE_SDL_SHADOWSURFACE -DUSE_OVERLAY -DSDEBUG

LDFLAGS :=
LDFLAGS += $(AGAR_LDFLAGS) $(SDL_LDFLAGS) $(SDLFFMPEG_LDFLAGS)

all : example

example : example.o ffmpeg_player.o ag_sdl_aux.o
	$(CC) $(LDFLAGS) -o $@ $^

clean :
	$(RM) -f *.o example
