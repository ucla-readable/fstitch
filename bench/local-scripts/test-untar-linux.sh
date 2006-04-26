#!/bin/bash

. /home/adlr/testing/vars.sh

OUTFILE=$OUTDIR/untar-linux.txt

set -x

(
cat<<EOF
#!/bin/bash

tar -x -C /mnt/test -f /home/leiz/linux-2.6.15.tar
sync

EOF
) > "run.sh"
chmod 0755 run.sh

(
cat<<EOF
#!/bin/bash

set -x

# reset the filesystem
#dd if=$UFSCLEAN of=/dev/sdb
rm -rf /mnt/test/*

#insmod $CHECKOUTDIR/kfs/kkfsd.ko
#mount -t kfs kfs:/ /mnt/test

cat ~leiz/linux-2.6.15.tar > /dev/null

sleep 4
/usr/bin/time -f \%e -a -o $OUTFILE ./run.sh

reboot

EOF
) > "sudo_test.sh"

sudo /bin/bash ./sudo_test.sh
