#!/bin/sh
# tp-build.sh - run on the TT, in the bundle directory made by mkbundle.sh:
# compile everything with icc on the slot-1 T800 (through iserver), link,
# collect, and run tpbench on the transputer. Needs sp1's toolset in
# /usr/local/lib/transputer. Progress and output go to stdout; icc's
# warnings (unused statics, undefined macros in #if) go to warn.log.
#
# Plain ASV sh on purpose: AMIX bash (under amx) died silently here, after
# "unwind_frame_discard" warnings, when a child failed inside a function
# with redirections. ASV sh has no test -nt, so rerunning skips every
# object that exists (a new bundle starts clean). A link hiccup ("iserver
# - protocol error") is retried (TRIES, default 6).
PATH=/usr/local/lib/transputer/bin:$PATH
export PATH
T800=${T800:--t800}
TRIES=${TRIES:-6}

# step NAME CMD...: run CMD, retrying; exit the script if it keeps failing
step() {
	name=$1
	shift
	n=0
	while [ $n -lt $TRIES ]
	do
		if "$@" > step.log 2>&1
		then
			ok=1
		else
			ok=0
		fi
		grep -v '^Warning' step.log
		cat step.log >> warn.log
		if [ $ok = 1 ]
		then
			return 0
		fi
		n=`expr $n + 1`
		echo "  attempt $n failed"
		sleep 10
	done
	echo "FAILED: $name"
	exit 1
}

echo "start `date`"
for f in *.c
do
	b=`basename $f .c`
	if [ -f $b.tco ]
	then
		continue
	fi
	echo "icc $f `date +%T`"
	step "icc $f" icc $f $T800
done
# the object list is too long for iserver's command line: an indirect
# file with startup.lnk's directives and libraries plus our objects
echo "ilink `date +%T`"
grep -v '^--' /usr/local/lib/transputer/libs/startup.lnk > tpbench.lnk
ls *.tco >> tpbench.lnk
step ilink ilink $T800 -f tpbench.lnk -o tpbench.lku
echo "icollect `date +%T`"
step icollect icollect tpbench.lku -t -o tpbench.btl
echo "run `date +%T`"
iserver -sb tpbench.btl -sl ${TRANSPUTER:-/dev/link0}
echo "exit $? `date +%T`"
