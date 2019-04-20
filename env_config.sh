#!/usr/bin/env bash

echo "Making all to all ping binary"
make

echo "Ensuring executable script"
chmod +x bridged_allping.sh
