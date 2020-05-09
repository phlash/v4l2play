#! /bin/sh
# start metacity, then libavplay on provided display

export DISPLAY=$1
metacity &
./libavplay rtsp://192.168.42.129:8080/video/h264
