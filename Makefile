# Build v4l2play
#
all: v4l2play

debug: v4l2playd

clean:
	rm -f v4l2play v4l2playd

v4l2playd: v4l2play.c
	$(CC) -g -o $@ $<

v4l2play: v4l2play.c
	$(CC) -o $@ $<
