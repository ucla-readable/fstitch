#!/bin/bash

VM=freebsd_fsck.vm.gz
wget -c http://featherstitch.cs.ucla.edu/emubsd/${VM}
wget -c http://feathersttich.cs.ucla.edu/emubsd/freebsd.zqcow

[ -f ${VM} ] && gunzip -f ${VM}

