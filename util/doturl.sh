#!/bin/sh
# Usage: doturl <url> <format> <...>
# About: grabs the dot file at url and runs dots, printing in format format,

if [ $# -ne 2 ]; then
	exit 1
fi

DOTURL=$1
DOTFMT=$2

DOTFILE=`mktemp` || exit 1
IMGFILE=`mktemp` || exit 1

echo "Content-type: image/$DOTFMT"
echo ""

wget -q $DOTURL -O $DOTFILE || (rm -f $IMGFILE $DOTFILE; exit 1)

dot -T$DOTFMT $DOTFILE -o $IMGFILE

cat $IMGFILE

rm -f $IMGFILE $DOTFILE
