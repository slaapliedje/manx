#!/usr/bin/env python3
"""
bundle.py DIR - lay Manx's sources out for building on Helios (the
ATW800/2's T425) in DIR, to be copied to /helios/local/src/manx (or HDIR)
and built there with  source build.csh :

  c/        the C files, under unique 8.3 names, and mont.s
  h/        the headers
  o/ ox/ ou/ om/ ot/  objects: shared, xmanx's, ufetch's, manxtrust's,
		    test_sigkat's
  log/      the compiler's and linker's output, one file each
  build.csh compiles everything, then links xmanx, ufetch, manxtrust
	    and test_sigkat
  files.txt each file's 8.3 name and where it came from

Helios reaches files through the I/O server and GEMDOS. Names are 8.3,
and GEMDOS truncates a longer one when it opens a file, so a header is
stored under its truncated name (#include "tokenizer.h" then opens
TOKENIZE.H) unless that would make two the same: BearSSL's bearssl_*.h
become br_*.h and its config.h brconfig.h, the #include lines rewritten.
Helios C has no stdint.h: os/helios/stdint.h. The shell cuts long command
lines, so each step is a line of its own.

TLS is BearSSL built for a compiler with no 64-bit integer type
(BR_NO_U64), the files in tools/helios/bearssl.txt.
"""
import glob
import os
import re
import shutil
import zlib
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
HDIR = os.environ.get('HDIR', '/helios/local/src/manx')
BR = 'third_party/bearssl'

FLAGS = ('-Dconst= -Dinline= -D_BSD -DMANX_HELIOS -DBR_NO_U64=1 '
	'-DBR_USE_UNIX_TIME=1')
LINK_X = '/helios/lib/libx11.a -lX -lbsd -s50000 -h1000'
LINK = '-lbsd -s50000 -h1000'


def files(*patterns):
	out = []
	for p in patterns:
		got = sorted(glob.glob(os.path.join(ROOT, p)))
		if not got:
			sys.exit('bundle: nothing matches ' + p)
		out += [os.path.relpath(g, ROOT) for g in got]
	return out


def bearssl_files():
	with open(os.path.join(ROOT, 'tools/helios/bearssl.txt')) as f:
		return [os.path.join(BR, l.strip()) for l in f
			if l.strip() and not l.startswith('#')]


# the object groups: (directory, sources)
# (tls/mont.c's place is taken by os/helios/mont.s, assembled: ASM)
SHARED = [f for f in files('net/*.c', 'os/*.c', 'tls/*.c',
	'os/helios/os_time.c', 'os/helios/poll.c', 'os/sysv4/snprintf.c')
	if f != 'tls/mont.c'] + bearssl_files()
XMANX = files('src/manx.c', 'src/pagecss.c', 'src/pageimg.c',
	'frontend/x11/*.c', 'text/*.c', 'html/*.c', 'style/*.c', 'layout/*.c',
	'image/*.c')
UFETCH = files('src/ufetch.c')
TRUST = files('src/manxtrust.c')
TEST = files('tests/test_sigkat.c')
GROUPS = [('o', SHARED), ('ox', XMANX), ('ou', UFETCH), ('om', TRUST),
	('ot', TEST)]
# assembly, into the shared objects: Montgomery multiplication on lmul
ASM = ['os/helios/mont.s']

HEADERS = files('os/*.h', 'net/*.h', 'text/*.h', 'html/*.h', 'style/*.h',
	'layout/*.h', 'image/*.h', 'frontend/*.h', 'src/*.h', 'tls/*.h',
	'os/helios/poll.h', 'os/helios/stdint.h', BR + '/inc/*.h',
	BR + '/src/inner.h', BR + '/src/config.h')


def short(name):
	"""GEMDOS's 8.3 form of a file name, lower case."""
	base, ext = os.path.splitext(name)
	return (base[:8] + ext[:4]).lower()


def c_name(src):
	"""A C file's 8.3 name: its own if that fits, else six letters and two
	hex digits of its path's CRC, so that a file keeps its name (and its
	object) whatever else is bundled."""
	base = os.path.splitext(os.path.basename(src))[0].lower()
	if len(base) <= 8:
		return base
	return base.replace('_', '')[:6] + '%02x' % (zlib.crc32(src.encode()) & 0xFF)


def header_name(path):
	base = os.path.basename(path)
	if path == BR + '/src/config.h':
		return 'brconfig.h'
	m = re.match(r'bearssl_(\w+)\.h$', base)
	if m:
		return short('br_' + m.group(1) + '.h')
	return short(base)


def uninline(text):
	"""BearSSL's headers define many small functions static inline, and
	Helios C compiles every one of them into every file that includes
	the header, used or not: some 18 KB a BearSSL file, most of a
	megabyte in all. Each becomes a declaration here, its body kept
	for brinline.c (BR_INLINE_BODIES), which compiles them once."""
	lines = text.split('\n')
	out = []
	i = 0
	while i < len(lines):
		l = lines[i]
		if not l.startswith('static inline'):
			out.append(l)
			i += 1
			continue
		j = i
		while lines[j] != '{':
			j += 1
		k = j
		while lines[k] != '}':
			k += 1
		out.append(l[len('static inline'):].lstrip())
		out += lines[i + 1:j]
		out.append('#ifdef BR_INLINE_BODIES')
		out += lines[j:k + 1]
		out += ['#else', ';', '#endif']
		i = k + 1
	return '\n'.join(out)


BRINLINE = """/*
 * brinline.c - BearSSL's static inline functions, compiled here once
 * (tools/helios/bundle.py: the headers only declare them for Helios C).
 */
#define BR_INLINE_BODIES 1
#include "inner.h"
"""


def rewrite(text, path, hmap):
	"""Point #include lines at the headers' names in the bundle."""
	def inc(m):
		name = m.group(2)
		if name in hmap:
			return m.group(1) + '"' + hmap[name] + '"'
		return m.group(0)
	text = re.sub(r'(#\s*include\s*)"([^"]+)"', inc, text)
	if path.startswith(BR + '/src/'):
		text = re.sub(r'(#\s*include\s*)"config\.h"',
			r'\1"brconfig.h"', text)
	if path.startswith(BR) and path.endswith('.h'):
		text = uninline(text)
	return text


def main():
	if len(sys.argv) != 2:
		sys.exit('usage: bundle.py DIR')
	out = sys.argv[1]
	if os.path.exists(out):
		shutil.rmtree(out)
	for d in ['c', 'h', 'log'] + [g for g, _ in GROUPS]:
		os.makedirs(os.path.join(out, d))

	# headers, and what their includers call them
	hmap, taken = {}, {}
	for h in HEADERS:
		n = header_name(h)
		if n in taken:
			sys.exit('bundle: header name clash: %s (%s, %s)'
				% (n, taken[n], h))
		taken[n] = h
		base = os.path.basename(h)
		if base != n:
			hmap[base] = n
	if BR + '/src/config.h' in taken.values():
		del hmap['config.h']	# (Manx's own config.h keeps its name)

	listing = []
	for n, src in sorted(taken.items()):
		with open(os.path.join(ROOT, src), errors='replace') as f:
			text = rewrite(f.read(), src, hmap)
		with open(os.path.join(out, 'h', n), 'w') as f:
			f.write(text)
		listing.append('h/%s %s' % (n, src))

	# C files: unique 8.3 names, one compile line each
	lines = ['# Manx for Helios: source this from ' + HDIR,
		'cd ' + HDIR]
	used = {}
	for group, srcs in GROUPS:
		for src in srcs:
			n = c_name(src)
			if n in used:
				sys.exit('bundle: C name clash: %s (%s, %s)'
					% (n, used[n], src))
			used[n] = src
			with open(os.path.join(ROOT, src), errors='replace') as f:
				text = rewrite(f.read(), src, hmap)
			with open(os.path.join(out, 'c', n + '.c'), 'w') as f:
				f.write(text)
			listing.append('c/%s.c %s' % (n, src))
			lines.append('c -c %s -I%s/h -o %s/%s.o c/%s.c >& log/%s.log'
				% (FLAGS, HDIR, group, n, n, n))
	# BearSSL's inline functions, once
	used['brinline'] = 'brinline.c'
	with open(os.path.join(out, 'c', 'brinline.c'), 'w') as f:
		f.write(BRINLINE)
	listing.append('c/brinline.c (bundle.py)')
	lines.append('c -c %s -I%s/h -o o/brinline.o c/brinline.c >& log/brinline.log'
		% (FLAGS, HDIR))
	for src in ASM:
		n = c_name(src)
		if n in used:
			sys.exit('bundle: name clash: %s (%s, %s)' % (n, used[n], src))
		used[n] = src
		shutil.copy(os.path.join(ROOT, src), os.path.join(out, 'c', n + '.s'))
		listing.append('c/%s.s %s' % (n, src))
		lines.append('c -c -T5 -o o/%s.o c/%s.s >& log/%s.log' % (n, n, n))
	lines += [
		'echo compiled > log/done.txt',
		'c -o xmanx ox/*.o o/*.o %s >& log/xmanx.log' % LINK_X,
		'c -o ufetch ou/*.o o/*.o %s >& log/ufetch.log' % LINK,
		'c -o manxtrust om/*.o o/*.o %s >& log/trust.log' % LINK,
		'c -o test_sigkat ot/*.o o/*.o %s >& log/test.log' % LINK,
		'echo linked > log/linked.txt',
		'# back home: root\'s startx is found there, through . in the path',
		'cd']
	for l in lines:
		if len(l) > 200:
			sys.exit('bundle: line too long for the Helios shell: ' + l)
	with open(os.path.join(out, 'build.csh'), 'w') as f:
		f.write('\n'.join(lines) + '\n')
	with open(os.path.join(out, 'files.txt'), 'w') as f:
		f.write('\n'.join(listing) + '\n')
	print('bundle: %d C files, %d headers in %s' % (
		sum(len(s) for _, s in GROUPS), len(taken), out))


main()
