#!/bin/bash

. /home/adlr/testing/vars.sh

cd /home/adlr

set -x

(
cat<<EOF
#!/bin/bash

set -x

dd if=$UFSCLEAN of=/dev/sdb
insmod $CHECKOUTDIR/kfs/kkfsd.ko
mount -t kfs kfs:/ /mnt/test
mkdir /mnt/test/mabwd
cd /mnt/test/mabwd

sleep 10

make -f /home/adlr/ab/original/Makefile >& $OUTDIR/mablogfile
cat $OUTDIR/mablogfile >> $OUTDIR/mablogfile.all

reboot

EOF
) > "sudo_test.sh"

sudo /bin/bash ./sudo_test.sh
