#!/bin/sh
# uubench a number of WB cache sizes

NRUNS=2

function usage() {
	echo "Usage: `basename \"$0\"` <OUTDIR> <NBLIST>"
}

function log() {
	NB=$1
	TEST=$2
	TIME=`grep ^avg-kfsd-$TEST "$OUTDIR"/time-$NB.log | awk '{print $2}'`
	echo "$NB $TIME"
}

if [ $# -lt 2 ]; then usage "$0" 2>&1; exit 1; fi

if [ -f times.log ]; then echo "A times.log still exists" 2>&1; exit 1; fi

OUTDIR="$1"
NBLIST="$2"

[ -d "$OUTDIR" ] || mkdir "$OUTDIR" || exit 1

for NB in $NBLIST
do
	echo "======== $NB blocks"

	NWBBLOCKS=$NB bench/uubench.sh $NRUNS || break

	mv time.log "$OUTDIR"/time-$NB.log
	log $NB tar >> "$OUTDIR"/time_tar
	log $NB rm  >> "$OUTDIR"/time_rm
	sync
done
