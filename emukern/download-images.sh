#!/bin/bash

wget http://kudos.cs.ucla.edu/emukern/kkfsd.vm.gz
wget http://kudos.cs.ucla.edu/emukern/knoppix.iso

[ -f kkfsd.vm.gz ] && gunzip kkfsd.vm.gz
