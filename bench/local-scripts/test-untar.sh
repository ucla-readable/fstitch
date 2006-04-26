#!/bin/bash

. /home/adlr/testing/vars.sh

OUTFILE=$OUTDIR/untar.txt

set -x

(
cat<<EOF
#!/bin/bash

tar -x -C /mnt/test -f /home/leiz/linux-2.6.15.tar
$CHECKOUTDIR/obj/unix-user/user/fsync /mnt/test/

EOF
) > "run.sh"
chmod 0755 run.sh

(
cat<<EOF
#!/bin/bash

set -x

# reset the filesystem
dd if=$UFSCLEAN of=/dev/sdb

insmod $CHECKOUTDIR/kfs/kkfsd.ko
mount -t kfs kfs:/ /mnt/test

cat ~leiz/linux-2.6.15.tar > /dev/null

sleep 10
/usr/bin/time -f \%e -a -o $OUTFILE ./run.sh

#reboot

EOF
) > "sudo_test.sh"

sudo /bin/bash ./sudo_test.sh
