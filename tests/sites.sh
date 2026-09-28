#!/bin/sh
# sites.sh UFETCH - the Phase 1 real-site campaign: each site twice, in
# separate processes (a cold visit, then a repeat that should resume the
# session or know the leaf). Summary lines only.
U=$1
for url in \
	http://example.com/ \
	https://example.com/ \
	https://www.google.com/ \
	https://en.wikipedia.org/wiki/Motorola_68030 \
	https://news.ycombinator.com/ \
	https://lite.cnn.com/ \
	https://text.npr.org/ \
	https://github.com/ \
	https://letsencrypt.org/ \
	https://www.gnu.org/ \
	gopher://gopher.floodgap.com/
do
	for visit in 1 2
	do
		echo "=== $visit $url `date +%T`"
		$U $url 2>&1
	done
done
echo "SITES DONE `date +%T`"
