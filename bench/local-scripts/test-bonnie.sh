#!/bin/bash

. /home/adlr/testing/vars.sh

set -x

(
cat<<EOF
#!/bin/bash

set -x

dd if=$UFSCLEANBIGZ | gunzip | dd of=/dev/sdb bs=1M
insmod $CHECKOUTDIR/kfs/kkfsd.ko
mount -t kfs kfs:/ /mnt/test
mkdir /mnt/test/bonniewd
cd /mnt/test/bonniewd

sleep 4

/home/adlr/bonnie/Bonnie -s 1024M -html -m KFS >> $OUTDIR/bonnie.html

reboot

EOF
) > "sudo_test.sh"

sudo /bin/bash ./sudo_test.sh
