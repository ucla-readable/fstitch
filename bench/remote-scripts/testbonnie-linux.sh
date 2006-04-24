#!/bin/bash

set -x

echo You are about to run the MAB test on kudos-pb. It is running with this kernel:
ssh adlr@kudos-pb.cs.ucla.edu uname -a
echo and this checkout:
ssh adlr@kudos-pb.cs.ucla.edu 'cat /home/adlr/testing/vars.sh | grep CHECKOUTDIR='
echo
echo how many runs do you want to do?
#read times
times=3

echo running $times times...


for ((a=1; a <= times ; a++))
do
ssh adlr@kudos-pb.cs.ucla.edu /home/adlr/testing/test-bonnie-linux.sh && sleep 120
done

echo; echo

echo tests done.
echo 'bonnie!' | growlnotify -s 'benchmarks done!'
