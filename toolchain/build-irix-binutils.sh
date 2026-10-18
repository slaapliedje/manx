#!/bin/sh
# build-irix-binutils.sh [PREFIX] - binutils for toolchain/irix-cc, in
# PREFIX (default ~/opt/irix-cross): 2.41 for mips-sgi-irix6.5 (as and the
# rest), and 2.23.2 with SGUG RSE's IRIX fixes, of which only ld is kept, as
# mips-sgi-irix6.5-ld-sgug. Stock ld (2.23 or 2.41) puts an executable's own
# global symbols in the GOT's local part, and IRIX's rld faults on that
# before loading a library; SGUG's patch keeps them global, as SGI's
# linkers did. 2.23.2 is from 2013: it's built as gnu89.
set -e
prefix=${1:-$HOME/opt/irix-cross}
work=${TMPDIR:-/tmp}/irix-binutils.$$
sgug=https://raw.githubusercontent.com/sgidevnet/sgug-rse/master/packages/binutils
mkdir -p "$work" "$prefix"
cd "$work"
curl -sSLO https://ftp.gnu.org/gnu/binutils/binutils-2.41.tar.xz
curl -sSLO https://ftp.gnu.org/gnu/binutils/binutils-2.23.2.tar.bz2
curl -sSLO $sgug/binutils2_23.sgifixes.patch
tar xf binutils-2.41.tar.xz
tar xf binutils-2.23.2.tar.bz2
(cd binutils-2.23.2 && patch -p1 -s < ../binutils2_23.sgifixes.patch)

mkdir b241 && cd b241
# (gnu17: in C23 static_assert is a keyword, which 2.41's MIPS tables use)
CFLAGS="-O2 -std=gnu17" ../binutils-2.41/configure --target=mips-sgi-irix6.5 \
	--prefix="$prefix" --disable-nls --disable-werror --disable-gdb \
	--disable-gprofng --disable-sim > configure.log
make -j"$(nproc)" > make.log
make install > install.log
cd ..

mkdir b223 && cd b223
CFLAGS="-O2 -std=gnu89 -fcommon -w" ../binutils-2.23.2/configure \
	--target=mips-sgi-irix6.5 --prefix="$work/inst223" --disable-nls \
	--disable-werror --disable-gdb > configure.log
make -j"$(nproc)" all-ld > make.log
make install-ld > install.log
cp "$work/inst223/bin/mips-sgi-irix6.5-ld" "$prefix/bin/mips-sgi-irix6.5-ld-sgug"
cd /
rm -rf "$work"
echo "binutils for IRIX in $prefix/bin"
