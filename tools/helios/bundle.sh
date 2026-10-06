#!/bin/sh
# bundle.sh DIR - lay xmanx's sources out for building on Helios (the
# ATW800/2's transputer) in DIR: c/ the C files, h/ the headers, o/ for
# objects, and build.csh to run there (source build.csh in DIR as seen
# from Helios, e.g. /helios/local/src/manx).
#
# Helios reaches its files through the I/O server and GEMDOS: names are
# 8.3, and GEMDOS truncates longer ones when it opens a file. So headers
# are stored under their truncated names - #include "tokenizer.h" then
# opens TOKENIZE.H - and C files under unique 8.3 names. The shell cuts
# long command lines, so each step is one short line.
#
# The build is HTTP-only (Helios C has no 64-bit integers for BearSSL):
# os/helios/tls.h and tls_none.c stand in for tls/.
set -e
DIR=${1:?usage: bundle.sh DIR}
HDIR=${HDIR:-/helios/local/src/manx}
cd "$(dirname "$0")/../.."

SRCS="src/manx.c src/pagecss.c src/pageimg.c
	$(ls frontend/x11/*.c text/*.c html/*.c style/*.c layout/*.c image/*.c net/*.c os/*.c)
	os/helios/os_time.c os/helios/poll.c os/helios/tls_none.c os/sysv4/snprintf.c"
HDRS="$(ls os/*.h net/*.h text/*.h html/*.h style/*.h layout/*.h image/*.h frontend/*.h src/*.h 2>/dev/null)
	tls/entropy.h os/helios/poll.h os/helios/tls.h"

rm -rf "$DIR"
mkdir -p "$DIR/c" "$DIR/h" "$DIR/o"

short() {	# GEMDOS's 8.3 form of a file name, lower case
	b=${1%.*}; e=${1##*.}
	[ "$e" = "$1" ] && e=
	b=$(printf '%s' "$b" | cut -c1-8); e=$(printf '%s' "$e" | cut -c1-3)
	printf '%s%s' "$b" "${e:+.$e}" | tr 'A-Z' 'a-z'
}

for h in $HDRS; do
	n=$(short "$(basename "$h")")
	# os/helios's tls.h replaces tls/tls.h, which is not bundled
	[ -e "$DIR/h/$n" ] && { echo "bundle: header name clash: $n ($h)" >&2; exit 1; }
	cp "$h" "$DIR/h/$n"
done

: > "$DIR/files.txt"
{
	echo "# xmanx for Helios: source this from $HDIR"
	echo "cd $HDIR"
} > "$DIR/build.csh"
for s in $SRCS; do
	n=$(short "$(basename "$s")")
	[ -e "$DIR/c/$n" ] && { echo "bundle: source name clash: $n ($s)" >&2; exit 1; }
	cp "$s" "$DIR/c/$n"
	echo "$n $s" >> "$DIR/files.txt"
	o=${n%.c}.o
	echo "c -c -Dconst= -D_BSD -DMANX_HELIOS -I$HDIR/h -o o/$o c/$n >& o/${n%.c}.log" >> "$DIR/build.csh"
done
echo "echo compiled > o/done.txt" >> "$DIR/build.csh"
# Xlib comes in two parts: libx11.a, and the shared library's stub that
# -lX finds (/helios/lib/xlib.def); the archive goes first
echo "c -o xmanx o/*.o /helios/lib/libx11.a -lX -lbsd -s20000 -h1000 >& o/link.log" >> "$DIR/build.csh"
echo "echo linked > o/linked.txt" >> "$DIR/build.csh"
# back home: root's startx is found there, through . in the path
echo "cd" >> "$DIR/build.csh"
awk '{ if (length > 120) { print "bundle: long line: " $0 > "/dev/stderr"; bad = 1 } } END { exit bad }' "$DIR/build.csh"
echo "bundle: $(wc -l < "$DIR/files.txt") C files, $(ls "$DIR/h" | wc -l) headers in $DIR"
