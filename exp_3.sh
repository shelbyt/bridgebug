#!/usr/bin/env bash

echo "Experiment 2: 0.1 (100ms) delay, 2 containers, 1 run"
echo

make clean
make delay_allping
bash bridged_allping.sh
