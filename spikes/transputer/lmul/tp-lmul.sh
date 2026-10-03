#!/bin/sh
# tp-lmul.sh PROC LINK - on the TT, in a directory holding tpmont.c and
# tpmontvec.h: compile tpmont with icc (on the slot-1 T800, through
# iserver) for PROC (t800, t425), link, collect, and run it on the
# transputer at LINK (/dev/link0: TRAM slot 1; /dev/link1: the FPGA's T425).
# Plain ASV sh (see ../tp-build.sh); link hiccups are retried.
PATH=/usr/local/lib/transputer/bin:$PATH
export PATH
P=${1:-t800}
L=${2:-/dev/link0}

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
		n=`expr $n + 1`
		echo "  attempt $n failed"
		sleep 10
	done
	echo "FAILED: $*"
	exit 1
}

echo "#define TP_NAME \"$P at $L (icc)\"" > tpcfg.h
echo "#define TP_LMUL 1" >> tpcfg.h
rm -f tpmont.tco tpmont.lku tpmont.btl
echo "icc `date +%T`"
step icc tpmont.c -$P
echo "ilink `date +%T`"
step ilink tpmont.tco -$P -f startup.lnk -o tpmont.lku
echo "icollect `date +%T`"
step icollect tpmont.lku -t -o tpmont.btl
echo "run `date +%T`"
iserver -sb tpmont.btl -sl $L
echo "exit $? `date +%T`"
