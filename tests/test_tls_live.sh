#!/bin/sh
# test_tls_live.sh UFETCH - live TLS checks (needs the internet):
# - every one of the badssl servers must be refused, with early requests
#   (the default) and without (-E)
# - a second connection in one process must resume the session (a resumed
#   handshake sends no certificate: stale state from the first connection
#   once made it look like a full one)
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
# -n: no connection reuse, so the redirect and the second URL each need
# a new connection, which should resume
out=`$U -n https://en.wikipedia.org/ https://en.wikipedia.org/ 2>&1`
if [ "`echo "$out" | grep -c 'resumed handshake'`" = 2 ]
then
	echo "ok   in-process session resumption"
else
	echo "FAIL in-process session resumption"
	echo "$out" | sed 's/^/     /'
	fails=`expr $fails + 1`
fi
echo "$fails failure(s)"
[ $fails = 0 ]
