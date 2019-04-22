#!/usr/bin/env bash

echo "Experiment 2: No delay, 2 containers, multi-run (2) run"
echo

make clean
make nodelay_allping
bash bridged_allping.sh 2
