# Build v4l2play, libavplay
#
all: v4l2play libavplay

debug: v4l2playd

test: all
	xinit ./xrun.sh :10 -- /usr/bin/Xephyr :10 -ac -screen 1024x600

clean:
	rm -f v4l2play v4l2playd libavplay

v4l2playd: v4l2play.c
	$(CC) -g -o $@ $<

v4l2play: v4l2play.c
	$(CC) -o $@ $<

libavplay: libavplay.c
	$(CC) -o $@ $< $(shell pkg-config --cflags --libs sdl2 libavformat libavcodec libavutil)
