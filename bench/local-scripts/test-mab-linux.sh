#!/bin/bash

. /home/adlr/testing/vars.sh

cd /home/adlr

set -x

(
cat<<EOF
#!/bin/bash

set -x

#dd if=$UFSCLEAN of=/dev/sdb
rm -rf /mnt/test/*
#insmod $CHECKOUTDIR/kfs/kkfsd.ko
#mount -t kfs kfs:/ /mnt/test
mkdir /mnt/test/mabwd
cd /mnt/test/mabwd

sleep 4

make -f /home/adlr/ab/original/Makefile >& $OUTDIR/mablogfile-linux
cat $OUTDIR/mablogfile-linux >> $OUTDIR/mablogfile-linux.all

rm -rf /mnt/test/*

reboot

EOF
) > "sudo_test.sh"

sudo /bin/bash ./sudo_test.sh
