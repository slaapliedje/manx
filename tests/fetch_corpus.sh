#!/bin/sh
# fetch_corpus.sh [DIR] - download the pages of tests/corpus_urls.txt with
# curl into DIR (default build/corpus): N.html and N.hdr (the headers, for
# the charset). The pages are other people's: they stay out of git.
dir=${1:-build/corpus}
mkdir -p "$dir"
n=0
grep -v '^#' "$(dirname "$0")/corpus_urls.txt" | while read -r url
do
	[ -n "$url" ] || continue
	n=$((n + 1))
	f=$(printf '%s/%02d' "$dir" $n)
	if curl -sSL --compressed --max-time 30 -A "Mozilla/5.0 (X11; Linux) ub-corpus" \
		-D "$f.hdr" -o "$f.html" "$url"
	then
		echo "$url" > "$f.url"
		printf '%s %8d %s\n' "$(basename $f)" "$(wc -c < "$f.html")" "$url"
	else
		echo "FAILED $url"
		rm -f "$f.html" "$f.hdr"
	fi
done
