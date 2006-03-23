#!/bin/bash

qemu -hda emubsd/freebsd.zqcow -hdb obj/unix-user/fs/ufs.img \
	-loadvm emubsd/freebsd_fsck.vm $@ 

