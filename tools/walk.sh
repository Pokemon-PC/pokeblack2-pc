#!/bin/sh
# walk DIR N [KEY]: N steps of 30 frames holding DIR, tapping KEY (default B)
d=$1; n=$2; k=${3:-B}; i=0
while [ $i -lt $n ]; do echo "k ${d}${k} 4"; echo "k $d 26"; i=$((i+1)); done
