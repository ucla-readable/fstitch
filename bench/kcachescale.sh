#!/bin/sh
# kbench a number of WB cache sizes

NRUNS=2

function usage() {
	echo "Usage: `basename \"$0\"` <MAKEUSER> <OUTDIR> <ufs|ext2> <NBLIST>"
}

function log() {
	NB=$1
	TEST=$2
	AVG=`grep ^avg-$TEST "$OUTDIR"/time-$NB.log | tail -1 | awk '{print $2}'`
	MIN=`grep ^min-$TEST "$OUTDIR"/time-$NB.log | tail -1 | awk '{print $2}'`
	MAX=`grep ^max-$TEST "$OUTDIR"/time-$NB.log | tail -1 | awk '{print $2}'`
	echo "$NB $AVG $MIN $MAX"
}

if [ $# -lt 3 ]; then usage "$0" 2>&1; exit 1; fi

if [ -f times.log ]; then echo "A times.log still exists" 2>&1; exit 1; fi
if [ "`whoami`" != "root" ]; then echo "Must run as root" 2>&1; exit 1; fi

MAKEUSER="$1"
OUTDIR="$2"
FS="$3"
NBLIST="$4"

[ -d "$OUTDIR" ] || su "$MAKEUSER" -c mkdir "$OUTDIR" || exit 1
su "$MAKEUSER" -c touch "$OUTDIR"/time_tar "$OUTDIR"/time_rm || exit 1

for NB in $NBLIST
do
	echo "======== $NB blocks"

	NWBBLOCKS=$NB bench/kbench.sh "$MAKEUSER" $FS $NRUNS || break

	mv time.log "$OUTDIR"/time-$NB.log
	log $NB tar >> "$OUTDIR"/time_tar
	log $NB rm  >> "$OUTDIR"/time_rm
	sync
done
