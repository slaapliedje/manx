#!/bin/sh
# tp-echo.sh - on the TT: build tpecho for the T425 with the reduced
# library (icc on the slot-1 T800), and boot and time it with tpboot.
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
		# a compiler's error is final; only link hiccups are worth again
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
rm -f tpecho.tco tpecho.lku tpecho.btl
echo "icc `date +%T`"
step icc tpecho.c -t425
echo "ilink `date +%T`"
step ilink tpecho.tco -t425 -f ${LNK:-startrd.lnk} -o tpecho.lku
echo "icollect `date +%T`"
step icollect tpecho.lku -t -o tpecho.btl
ls -l tpecho.btl
./tpboot tpecho.btl /dev/link1 $DUMP
