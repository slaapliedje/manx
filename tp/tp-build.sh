#!/bin/sh
# tp-build.sh - on the TT: build tpsig.btl, Manx's program for the
# ATW800/2's T425, with the INMOS toolset (icc runs on the slot-1 T800
# through iserver). Run it in a directory holding tp/tpsig.c and tls's
# mont.[ch], sigmath.[ch], sigmath_curves.h, tpproto.h and tpjob.c.
# Plain ASV sh; a link hiccup is tried again, a compiler's error is not.
PATH=/usr/local/lib/transputer/bin:$PATH
export PATH

step() {
	n=0
	while [ $n -lt 6 ]
	do
		if "$@" > step.log 2>&1
		then
			grep -v '^Warning' step.log
			return 0
		fi
		cat step.log
		if egrep '^(Error|Serious|Fatal)-' step.log > /dev/null
		then
			echo "FAILED: $*"
			exit 1
		fi
		n=`expr $n + 1`
		echo "  attempt $n failed"
		sleep 10
	done
	echo "FAILED: $*"
	exit 1
}

rm -f *.tco tpsig.lku tpsig.btl
for f in mont sigmath tpjob tpsig
do
	echo "icc $f.c `date +%T`"
	step icc $f.c -t425
done
echo "ilink `date +%T`"
step ilink mont.tco sigmath.tco tpjob.tco tpsig.tco -t425 -f startup.lnk -o tpsig.lku
echo "icollect `date +%T`"
step icollect tpsig.lku -t -o tpsig.btl
ls -l tpsig.btl
