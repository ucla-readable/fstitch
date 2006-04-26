#!/bin/bash

. /home/adlr/testing/vars.sh

OUTFILE=$OUTDIR/del.txt

set -x

(
cat<<EOF
#!/bin/bash

rm -rf /mnt/test/linux-2.6.15
$CHECKOUTDIR/obj/unix-user/user/fsync /mnt/test/

EOF
) > "run.sh"
chmod 0755 run.sh

(
cat<<EOF
#!/bin/bash

set -x

insmod $CHECKOUTDIR/kfs/kkfsd.ko
mount -t kfs kfs:/ /mnt/test

sleep 10
/usr/bin/time -f \%e -a -o $OUTFILE ./run.sh

reboot

EOF
) > "sudo_test.sh"

sudo /bin/bash ./sudo_test.sh
