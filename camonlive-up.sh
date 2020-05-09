#! /bin/sh
# Start CamONLive stream as virtual camera for conferencing etc.
HOST=localhost
[ -z "$V4DEV" ] && V4DEV=/dev/video10
[ -z "$1" ] && adb forward tcp:8080 tcp:8080
[ -n "$1" ] && HOST=$1

echo "connecting: $HOST:8080/video/mjpeg"
# No need for format conversion and CPU cycles wasted through ffmpeg..
# ffmpeg -hide_banner -i http://$HOST:8080/video/mjpeg -f v4l2 -pix_fmt yuyv422 /dev/video2
# ..when you can simply push raw MJPEG frames to the consumer :)
./v4l2play -d $V4DEV -u http://$HOST:8080/video/mjpeg
[ -z "$1" ] && adb kill-server
