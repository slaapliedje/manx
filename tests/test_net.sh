#!/bin/sh
# test_net.sh UFETCH HOST - run ufetch against tests/netserver.py on HOST
# and check every case. Runs on the host and, with a pushed ufetch and
# HOST = the PC's address, on the TT (plain sh: no bash there).
U=$1
H=${2:-127.0.0.1}
fails=0
tmp=${TMPDIR:-/tmp}/ubnet.$$

# check NAME EXPECT-IN-SUMMARY EXPECT-BODY URL...
check() {
	name=$1; want=$2; body=$3; shift 3
	$U -o $tmp.body "$@" > $tmp.err 2>&1
	if grep "$want" $tmp.err > /dev/null && { [ -z "$body" ] || [ "`tr -d "\\r" < $tmp.body`" = "$body" ]; }
	then
		echo "ok   $name"
	else
		echo "FAIL $name"; sed 's/^/     /' $tmp.err
		fails=`expr $fails + 1`
	fi
}

check "content-length"  "^200 .*5 B"            "hello"          http://$H:8081/len
check "chunked"         "text/html; iso-8859-1" "<html>chunked"  http://$H:8081/chunked
check "until close"     "^200 .*11 B"           "until close"    http://$H:8081/close
check "redirects"       "2 redirect"            "hello"          http://$H:8081/redir
check "redirect loop"   "too many redirects"    ""               http://$H:8081/loop
check "slow head"       "^200"                  "slow"           http://$H:8081/slowhead
check "404"             "^404"                  ""               http://$H:8081/nothing
check "keep-alive"      "connection reused"     "123"            http://$H:8081/count http://$H:8081/count http://$H:8081/count
check "big chunked"     "300000 B"              ""               http://$H:8081/big
check "gopher menu"     "text/x-gopher-menu"    ""               gopher://$H:7070/
check "gopher text"     "text/plain"            "gopher text"    gopher://$H:7070/0/file.txt
check "gopher search"   "^200"                  ""               "gopher://$H:7070/7/search?68030"
check "no server"       "can't connect"         ""               http://$H:1/
check "bad scheme"      "unsupported"           ""               ftp://$H/x
rm -f $tmp.body $tmp.err
echo "$fails failure(s)"
[ $fails = 0 ]
