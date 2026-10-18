#!/bin/sh
# fetch_html5lib.sh [DIR] - the HTML5 tree-construction tests (html5lib's
# .dat files, now kept in web-platform-tests) into DIR (default build/wpt):
# a sparse clone of html/syntax/parsing only. They stay out of git.
dir=${1:-build/wpt}
if [ -d "$dir/.git" ]; then
	git -C "$dir" pull -q --depth 1
else
	git clone -q --depth 1 --filter=blob:none --sparse \
		https://github.com/web-platform-tests/wpt "$dir" &&
	git -C "$dir" sparse-checkout set html/syntax/parsing
fi
