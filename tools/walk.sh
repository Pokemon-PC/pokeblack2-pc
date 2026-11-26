#!/bin/sh
# walk DIR N [KEY]: N steps of 30 frames for PB2_CTRL (tools/ctrl.py): hold DIR
# 16 frames (one tile), release, tap KEY (default B; A also fights battles and
# reads text). Keys are released between steps: the battle menu only appears
# when no key is held.
d=$1; n=$2; k=${3:-B}; i=0
while [ $i -lt $n ]; do echo "k $d 16"; echo "k - 4"; echo "k $k 4"; echo "k - 6"; i=$((i+1)); done
