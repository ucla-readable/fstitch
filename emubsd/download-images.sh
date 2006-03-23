#!/bin/bash

VM=freebsd_fsck.vm.gz
wget -c http://kudos.cs.ucla.edu/emubsd/${VM}
wget -c http://kudos.cs.ucla.edu/emubsd/freebsd.zqcow

[ -f ${VM} ] && gunzip -f ${VM}

