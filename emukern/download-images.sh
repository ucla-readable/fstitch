#!/bin/bash

VM=kfstitchd_mini.vm.gz
#wget -c http://featherstitch.cs.ucla.edu/emukern/${VM}
wget -c http://featherstitch.cs.ucla.edu/emukern/knoppix_mini.iso

[ -f ${VM} ] && gunzip -f ${VM}
