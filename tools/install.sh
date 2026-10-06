#!/bin/sh
# install.sh - put Manx where the system keeps its programs. Run it as
# root on Atari System V or AMIX, in the directory holding the binaries:
#
#	manx manxtrust ufetch	-> /usr/bin
#	xmanx			-> /usr/x11r6/bin (Atari System V's X11R6.3),
#				   else /usr/X/bin (AMIX's X11R5)
#
# BIN=dir and X11BIN=dir choose others. (Helios has its own places,
# /helios/bin and /helios/bin/x11: tools/helios/bundle.py's build.csh
# installs there.) Bourne shell: no $(...).
BIN=${BIN:-/usr/bin}
if [ -z "$X11BIN" ]; then
	if [ -d /usr/x11r6/bin ]; then
		X11BIN=/usr/x11r6/bin
	elif [ -d /usr/X/bin ]; then
		X11BIN=/usr/X/bin
	fi
fi

put() {
	# (a running program's file can be removed but not overwritten)
	rm -f $2/$1
	cp $1 $2/$1 && chmod 755 $2/$1 && echo "$2/$1"
}

for p in manx manxtrust ufetch; do
	if [ -f $p ]; then
		put $p $BIN || exit 1
	fi
done
if [ -f xmanx ]; then
	if [ -z "$X11BIN" ]; then
		echo "install.sh: no X11 directory here: set X11BIN" >&2
		exit 1
	fi
	put xmanx $X11BIN || exit 1
fi
