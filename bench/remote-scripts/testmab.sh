#!/bin/bash

echo You are about to run the MAB test on kudos-pb. It is running with this kernel:
ssh adlr@kudos-pb.cs.ucla.edu uname -a
echo and this checkout:
ssh adlr@kudos-pb.cs.ucla.edu 'cat /home/adlr/testing/vars.sh | grep CHECKOUTDIR='
echo
echo how many runs do you want to do?
read times

echo running $times times...


for ((a=2; a <= times ; a++))
do
ssh adlr@kudos-pb.cs.ucla.edu /home/adlr/testing/test-mab.sh && sleep 120
done

ssh adlr@kudos-pb.cs.ucla.edu /home/adlr/testing/test-mab.sh

echo; echo

echo tests done.
