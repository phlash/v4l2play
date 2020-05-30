#! /bin/sh
# start metacity, then specified application on specified display

export DISPLAY=$1
shift
metacity &
$*
