#!/bin/bash

function die() {
	exit 1
}

ROOT="`(cd "\`dirname \"$0\"\`"; pwd)`"

KUDOS="$ROOT/.."
OSIMG="$ROOT/knoppix.iso"
KERNEL=2.6.12
VMIMG="$ROOT/kkfsd.vm"

[ -f "$OSIMG" ] || die "Need a Knoppix CD image!"
[ -f "$VMIMG" ] || die "Need a QEMU VM image!"

(pushd "`dirname "$0"`" > /dev/null
 export PATH="`pwd`:$PATH"
 popd > /dev/null
 cd $KUDOS && KERNEL=$KERNEL make kernel) || die "Compile error!"

TMPDIR=`mktemp -d ${TMP:-/tmp}/kkfsd.XXXXXX` || "Cannot create temporary directory!"
trap "echo \"Cleaning up image in $TMPDIR\"; rm -rf $TMPDIR" EXIT
echo "Building image in $TMPDIR"

mkdir $TMPDIR/image
(cd "$KUDOS" && tar cf $TMPDIR/image/kkfsd.tar . --exclude emukern --exclude obj)
#gzip --best -n $TMPDIR/image/kkfsd.tar

cat > $TMPDIR/image/init.sh << EOF
#!/bin/bash
mkdir /ramdisk/kkfsd
ln -s /ramdisk/kkfsd /kfs
cd /ramdisk/kkfsd
date \`date +%m%d%H%M%Y.%S -r /mnt/kkfsd.tar*\` > /dev/null
echo -n "Extracting CD image... "
#tar xzf /mnt/kkfsd.tar.gz
tar xf /mnt/kkfsd.tar
echo "done."
cat > init.sh << NEOF
#!/bin/bash
umount /mnt
insmod kfs/kkfsd.ko
rm \\\$0
NEOF
chmod 755 init.sh
exec ./init.sh
EOF
chmod 755 $TMPDIR/image/init.sh

mkisofs -R -o $TMPDIR/kkfsd.img $TMPDIR/image || die "Cannot create CD image!"

dd if=/dev/zero of=$TMPDIR/hda.img bs=1M count=32 2> /dev/null
dd if=$TMPDIR/kkfsd.img of=$TMPDIR/hda.img conv=notrunc
[ "`du -b $TMPDIR/hda.img | awk '{print $1}'`" != "33554432" ] && die "KudOS directory ($KUDOS) is too large!"

qemu -hda $TMPDIR/hda.img -cdrom $OSIMG -boot d -k en-us -monitor stdio -loadvm "$VMIMG"
if [ "`tr -d \\000 < $TMPDIR/hda.img | head -n 1`" == "KFS" ]
then
	echo "Found KFS signature on hard disk"
fi
