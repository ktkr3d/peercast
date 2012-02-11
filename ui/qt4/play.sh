#!/bin/sh
if [ -x $1 ] ;
then
	exec $*
fi 
case $1 in
	*.pls) beep-media-player $1;;
	*.asx) gmplayer -prefer-ipv4 -playlist $1;;
	*) echo I cannot handle $1.;;
esac

