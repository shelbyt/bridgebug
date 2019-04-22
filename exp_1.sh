#!/usr/bin/env bash

echo "Experiment 1: No delay, 2 containers, 1 run"
echo

make clean
make nodelay_allping
bash bridged_allping.sh
