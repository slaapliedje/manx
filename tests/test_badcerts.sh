#!/bin/sh
# test_badcerts.sh UFETCH - every one of these servers must be refused,
# with early requests (the default) and without (-E). Needs the internet.
U=$1
fails=0
for mode in "" "-E"
do
	for h in expired wrong.host self-signed untrusted-root
	do
		if $U $mode https://$h.badssl.com/ > /dev/null 2>&1
		then
			echo "FAIL $h.badssl.com accepted ${mode:-early}"
			fails=`expr $fails + 1`
		else
			echo "ok   $h.badssl.com refused ${mode:-early}"
		fi
	done
	if $U $mode https://badssl.com/ > /dev/null 2>&1
	then
		echo "ok   badssl.com accepted ${mode:-early}"
	else
		echo "FAIL badssl.com (a good certificate) refused ${mode:-early}"
		fails=`expr $fails + 1`
	fi
done
echo "$fails failure(s)"
[ $fails = 0 ]
