# Build v4l2play, libavplay, sdlprobe
#

SDL_FLG=$(shell pkg-config --cflags --libs sdl2)
LAV_FLG=$(shell pkg-config --cflags --libs libavformat libavcodec libavutil)

all: v4l2play libavplay sdlprobe

debug: v4l2playd

testlav: all
	xinit ./xrun.sh :10 ./libavplay ../deepbacksub/fishtank.mp4 -- /usr/bin/Xephyr :10 -ac -screen 1024x600

testsdl: all
	xinit ./xrun.sh :10 ./sdlprobe video -- /usr/bin/Xephyr :10 -ac -screen 1024x600

clean:
	rm -f v4l2play v4l2playd libavplay sdlprobe

v4l2playd: v4l2play.c
	$(CC) -g -o $@ $<

v4l2play: v4l2play.c
	$(CC) -o $@ $<

libavplay: libavplay.c
	$(CC) -g -o $@ $< $(SDL_FLG) $(LAV_FLG)

sdlprobe: sdlprobe.c
	$(CC) -o $@ $< $(SDL_FLG)
