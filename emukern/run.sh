#!/bin/bash

function die() {
	echo -e "$@" >&2
	exit 1
}

ROOT="`(cd "\`dirname \"$0\"\`"; pwd)`"

FSTITCH="$ROOT/.."
OSIMG="$ROOT/knoppix_mini.iso"
KERNEL=2.6.12
VMIMG="$ROOT/kfstitchd_mini.vm"

[ -f "$OSIMG" ] || die "Need a Knoppix CD image!"
[ -f "$VMIMG" ] || die "Need a QEMU VM image!"

(pushd "`dirname "$0"`" > /dev/null
 export PATH="`pwd`:$PATH"
 popd > /dev/null
 cd $FSTITCH && KERNEL=$KERNEL make kernel) || die "Compile error!"

TMPDIR=`mktemp -d ${TMP:-/tmp}/kfstitchd.XXXXXX` || "Cannot create temporary directory!"
trap "echo \"Cleaning up image in $TMPDIR\"; rm -rf $TMPDIR" EXIT
echo "Building image in $TMPDIR"

mkdir $TMPDIR/image
(cd "$FSTITCH" && tar cf $TMPDIR/image/kfstitchd.tar --exclude emukern --exclude emubsd --exclude obj --exclude scratch .)
#gzip --best -n $TMPDIR/image/kfstitchd.tar

cat > $TMPDIR/image/init.sh << EOF
#!/bin/bash
mkdir /ramdisk/kfstitchd
ln -s /ramdisk/kfstitchd /fscore
cd /ramdisk/kfstitchd
date \`date +%m%d%H%M%Y.%S -r /mnt/kfstitchd.tar*\` > /dev/null
echo -n "Extracting CD image... "
#tar xzf /mnt/kfstitchd.tar.gz
tar xf /mnt/kfstitchd.tar
echo "done."
cat > init.sh << NEOF
#!/bin/bash
umount /mnt
insmod fscore/kfstitchd.ko
[ -f /proc/kfstitchd_debug ] && (cat /proc/kfstitchd_debug | nc 10.0.2.2 15166) &
if grep -q patchgroup /proc/devices; then
[ -f /dev/patchgroup ] || mknod /dev/patchgroup b 223 0
chmod 666 /dev/patchgroup; fi
mount fstitch:/ /mnt -t fstitch
mkdir /mnt/dev
mount fstitch:/dev /mnt/dev -t fstitch
rm \\\$0
NEOF
chmod 755 init.sh
exec ./init.sh
EOF
chmod 755 $TMPDIR/image/init.sh

mkisofs -R -o $TMPDIR/kfstitchd.img $TMPDIR/image || die "Cannot create CD image!"

dd if=/dev/zero of=$TMPDIR/hda.img bs=1M count=32 2> /dev/null
dd if=$TMPDIR/kfstitchd.img of=$TMPDIR/hda.img conv=notrunc
[ "`du -b $TMPDIR/hda.img | awk '{print $1}'`" != "33554432" ] && die "KudOS directory ($FSTITCH) is too large!\nTry putting large files in a directory named scratch to exclude them."

qemu -hda $TMPDIR/hda.img -cdrom $OSIMG -boot d -k en-us -serial stdio -loadvm "$VMIMG"
if [ "`tr -d \\000 < $TMPDIR/hda.img | head -n 1`" == "KFS" ]
then
	echo "Found KFS signature on hard disk"
fi
