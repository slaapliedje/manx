#!/bin/sh
# mkinst.sh - Manx as an IRIX inst package (a tardist), for inst and the
# Software Manager.
#
#   tools/mkinst.sh VERSION BUILDDIR OUTDIR
#
# BUILDDIR holds IRIX programs built with PACKAGED=inst (so the updater
# leaves them to inst): make TARGET=irix B=build/irix-inst PACKAGED=inst.
# They go to /usr/local/bin. The spec and idb files are written here; the
# images need IRIX's gendist (inst_dev.sw.base, on the 6.5 overlays' disc
# 2), so this runs on IRIX, or, given GENDIST_SH, through that command:
# GENDIST_SH='python3 ~/dev/iris/tools/irix-sh.py run' with GENDIST_HOST
# the IRIX path of this machine's directory GENDIST_SHARE (an NFS share).
# GENDIST: where gendist is, if not on the PATH (on a machine without
# inst_dev: tools/irix-extract.py takes it out of inst_dev_6522m.sw).
# The result: OUTDIR/manx-VERSION-irix.tardist (manx, manx.idb, manx.sw).
set -e
[ $# -eq 3 ] || { echo "usage: mkinst.sh VERSION BUILDDIR OUTDIR" >&2; exit 2; }
ver=$1 build=$2 out=$3

# inst's versions are one number: 0.9.0 is 900, 1.2.3 is 10203
v=$(echo "$ver" | awk -F. '{ printf "%d", ($1 * 100 + $2) * 100 + $3 }')

work=${GENDIST_SHARE:-${TMPDIR:-/tmp}}/manx-inst.$$
rm -rf "$work"
mkdir -p "$work/src/usr/local/bin" "$work/dist"
progs=
for p in manx xmanx manxmail manxtrust; do
	[ -f "$build/$p" ] || continue
	cp "$build/$p" "$work/src/usr/local/bin/$p"
	progs="$progs $p"
done
[ -n "$progs" ] || { echo "mkinst.sh: no programs in $build" >&2; exit 1; }

cat > "$work/manx.spec" <<EOF
product manx
	id "Manx $ver, a web browser"
	image sw
		id "Manx $ver Software"
		version $v
		order 9999
		subsys base default
			id "Manx $ver: manx, xmanx, manxmail, manxtrust"
			replaces self
			exp manx.sw.base
		endsubsys
	endimage
endproduct
EOF

# (in order of where each goes, as gendist wants)
{
	echo "d 0755 root sys usr/local usr/local manx.sw.base"
	echo "d 0755 root sys usr/local/bin usr/local/bin manx.sw.base"
	for p in $progs; do
		echo "f 0755 root sys usr/local/bin/$p usr/local/bin/$p manx.sw.base"
	done
} | LC_ALL=C sort -k 5,5 > "$work/manx.idb"

# (-nostrip: gendist would run IRIX's strip, which few machines have)
gd="${GENDIST:-gendist} -verbose -all -nostrip -rbase / -sbase SRC -idb SRC/../manx.idb -spec SRC/../manx.spec -dist DIST"
if [ -n "$GENDIST_SH" ]; then
	# (a script: a telnet session cuts long lines)
	h=$GENDIST_HOST/manx-inst.$$
	echo "$gd" | sed "s|SRC|$h/src|g; s|DIST|$h/dist|" > "$work/gendist.sh"
	$GENDIST_SH "sh $h/gendist.sh"
else
	sh -c "$(echo "$gd" | sed "s|SRC|$work/src|g; s|DIST|$work/dist|")"
fi
for f in manx manx.idb manx.sw; do
	[ -f "$work/dist/$f" ] || { echo "mkinst.sh: gendist made no $f" >&2; exit 1; }
done
mkdir -p "$out"
out=$(cd "$out" && pwd)
(cd "$work/dist" && tar cf "$out/manx-$ver-irix.tardist" manx manx.idb manx.sw)
rm -rf "$work"
echo "$out/manx-$ver-irix.tardist"
