#!/bin/sh
# release.sh - a Manx release: every platform built, the updater's signed
# list of programs, the IRIX inst package; with --publish, tagged and put
# on GitHub as release vVERSION.
#
#   tools/release.sh NOTES [--publish]
#
# NOTES: what's new, a line each (the manifest's notes, and the release's
# text). The version is VERSION's; the tree must be committed.
#
# Builds (into build/rel-*, without debugging information):
#   irix-mips   IRIX_SYSROOT (default ~/dev/irix-update/sysroot), and
#               again with PACKAGED=inst for the inst package, whose
#               gendist runs where GENDIST_SH says (tools/mkinst.sh)
#   sysv4-m68k  when AMIX_SYSROOT and ASV_CROSS are here; or SYSV4_BUILD,
#               a directory of programs built elsewhere from this commit
# The release key: ~/.config/manx-release/release-key.pem (MANX_KEY).
set -e
cd "$(dirname "$0")/.."
notes=$1
publish=$2
[ -f "$notes" ] || { echo "usage: release.sh NOTES [--publish]" >&2; exit 2; }
ver=$(cat VERSION)
out=build/release/v$ver
key=${MANX_KEY:-$HOME/.config/manx-release/release-key.pem}
progs="manx xmanx manxmail manxtrust"

if ! git diff --quiet || ! git diff --cached --quiet; then
	echo "release.sh: commit first" >&2
	exit 1
fi
rm -rf "$out"
mkdir -p "$out"
plats=

# IRIX: the updater's, and inst's
sysroot=${IRIX_SYSROOT:-$HOME/dev/irix-update/sysroot}
if [ -d "$sysroot" ]; then
	for b in rel-irix:'' rel-irix-inst:inst; do
		d=${b%%:*}
		make -j8 TARGET=irix IRIX_SYSROOT="$sysroot" B=build/$d G= \
			PACKAGED=${b#*:} $(for p in $progs; do echo build/$d/$p; done) \
			> build/$d.log 2>&1 || { tail -20 build/$d.log; exit 1; }
	done
	plats="$plats irix-mips=build/rel-irix"
else
	echo "release.sh: no IRIX sysroot ($sysroot): no IRIX programs" >&2
fi

# the 68030: AMIX's static programs (Atari System V runs them too)
if [ -n "$SYSV4_BUILD" ]; then
	plats="$plats sysv4-m68k=$SYSV4_BUILD"
elif [ -d "${AMIX_SYSROOT:-$HOME/dev/OpenUA/data/work/asv/amix/sysroot}" ] \
	&& [ -d "${ASV_CROSS:-$HOME/opt/asv-cross}" ]; then
	make -j8 TARGET=sysv4 B=build/rel-sysv4 build/rel-sysv4/manx \
		build/rel-sysv4/xmanx build/rel-sysv4/manxtrust \
		build/rel-sysv4/t425wipe > build/rel-sysv4.log 2>&1 \
		|| { tail -20 build/rel-sysv4.log; exit 1; }
	plats="$plats sysv4-m68k=build/rel-sysv4"
else
	echo "release.sh: no 68030 toolchain here, nor SYSV4_BUILD: no sysv4-m68k" >&2
fi
[ -n "$plats" ] || { echo "release.sh: nothing built" >&2; exit 1; }

# the signed list, and the programs gzipped
python3 tools/mkrelease.py "$ver" "$out" $plats --notes "$notes" --key "$key"

# IRIX's inst package
if [ -d build/rel-irix-inst ]; then
	tools/mkinst.sh "$ver" build/rel-irix-inst "$out"
fi
# (the 68030 programs as one archive too, for a first install)
if echo "$plats" | grep -q sysv4-m68k; then
	d=$(echo "$plats" | tr ' ' '\n' | sed -n 's/^sysv4-m68k=//p')
	tar cf - -C "$d" $(cd "$d" && ls manx xmanx manxtrust t425wipe 2>/dev/null) \
		| gzip -9 > "$out/manx-$ver-sysv4-m68k.tar.gz"
fi
if [ -d build/rel-irix ]; then
	tar cf - -C build/rel-irix $progs | gzip -9 > "$out/manx-$ver-irix-mips.tar.gz"
fi
ls -l "$out"

[ "$publish" = --publish ] || { echo "(not published: --publish does)"; exit 0; }
git tag -l "v$ver" | grep -q . || git tag -a "v$ver" -m "Manx $ver"
git push origin "v$ver"
gh release create "v$ver" "$out"/* --title "Manx $ver" --notes-file "$notes"
