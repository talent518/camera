CC=gcc
CFLAGS=$(shell pkg-config --cflags libavutil libavcodec libavformat libswscale libavdevice libswresample libavfilter sdl2) -O2 -Wno-deprecated-declarations
LDFLAGS=$(shell pkg-config --libs libavutil libavcodec libavformat libswscale libavdevice libswresample libavfilter sdl2) -lpthread -lm

all: camera
	@echo -n

camera: camera.o
	@echo LD $@
	@$(CC) -o $@ $< $(LDFLAGS)

clean:
	@rm -vf camera *.o

%.o: %.c
	@echo CC $@
	@$(CC) -o $@ $(CFLAGS) -c $<
