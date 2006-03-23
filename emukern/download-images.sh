#!/bin/bash

VM=kkfsd_mini.vm.gz
#wget -c http://kudos.cs.ucla.edu/emukern/${VM}
wget -c http://kudos.cs.ucla.edu/emukern/knoppix_mini.iso

[ -f ${VM} ] && gunzip -f ${VM}
