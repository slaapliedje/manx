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
	if grep "$want" $tmp.err > /dev/null && { [ -z "$body" ] || [ "`tr -d '\\015' < $tmp.body`" = "$body" ]; }
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
check "redirect keeps #" "len#sec "              "hello"          "http://$H:8081/redir#sec"
check "redirect loop"   "too many redirects"    ""               http://$H:8081/loop
check "slow head"       "^200"                  "slow"           http://$H:8081/slowhead
check "404"             "^404"                  ""               http://$H:8081/nothing
check "keep-alive"      "connection reused"     "123"            http://$H:8081/count http://$H:8081/count http://$H:8081/count
check "big chunked"     "300000 B"              ""               http://$H:8081/big
G="compressed compressed compressed hello"
check "gzip"            "^200"                  "$G"             http://$H:8081/gzip
check "deflate (zlib)"  "^200"                  "$G"             http://$H:8081/deflate
check "deflate (raw)"   "^200"                  "$G"             http://$H:8081/rawdeflate
check "gzip chunked"    "^200"                  "$G"             http://$H:8081/gzip-chunked
check "gzip cut short"  "cut short"             ""               http://$H:8081/gzip-cut
check "cookie on redirect" "^200" "method=GET cookie=sid=abc123; pref=x type=- body=" http://$H:8081/setcookie
C="method=GET cookie=sid=abc123; pref=x type=- body="
check "cookie sent again" "^200" "$C$C" http://$H:8081/setcookie http://$H:8081/echo
check "post"            "^200"   "method=POST cookie=- type=application/x-www-form-urlencoded body=q=68030&x=1" -d 'q=68030&x=1' http://$H:8081/echo
check "post, 303"       "^200"   "method=GET cookie=- type=- body=" -d 'a=b' http://$H:8081/post303
check "post, 307"       "^200"   "method=POST cookie=- type=application/x-www-form-urlencoded body=a=b" -d 'a=b' http://$H:8081/post307
check "gopher menu"     "text/x-gopher-menu"    ""               gopher://$H:7070/
check "gopher text"     "text/plain"            "gopher text"    gopher://$H:7070/0/file.txt
check "gopher search"   "^200"                  ""               "gopher://$H:7070/7/search?68030"
check "no server"       "can't connect"         ""               http://$H:1/
check "bad scheme"      "unsupported"           ""               ftp://$H/x
rm -f $tmp.body $tmp.err
echo "$fails failure(s)"
[ $fails = 0 ]
