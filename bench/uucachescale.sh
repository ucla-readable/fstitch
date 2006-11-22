#!/bin/sh
# uubench a number of WB cache sizes

NRUNS=2

function usage() {
	echo "Usage: `basename \"$0\"` <OUTDIR> <k1|k2> <NBLIST>"
}

function log() {
	NB=$1
	TEST=$2
	TIME=`grep ^avg-kfsd-$TEST "$OUTDIR"/time-$NB.log | awk '{print $2}'`
	echo "$NB $TIME"
}

if [ $# -lt 3 ]; then usage "$0" 2>&1; exit 1; fi

if [ -f times.log ]; then echo "A times.log still exists" 2>&1; exit 1; fi

OUTDIR="$1"
KDIR="$2"
NBLIST="$3"

[ -d "$OUTDIR" ] || mkdir "$OUTDIR" || exit 1

for NB in $NBLIST
do
	echo "======== $NB blocks"

	NWBBLOCKS=$NB bench/uubench.sh $KDIR $NRUNS || break

	mv time.log "$OUTDIR"/time-$NB.log
	log $NB tar >> "$OUTDIR"/time_tar
	log $NB rm  >> "$OUTDIR"/time_rm
	sync
done
