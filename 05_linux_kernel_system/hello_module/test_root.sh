#!/usr/bin/env bash
set -euo pipefail

make
sudo insmod os_hello.ko student="${1:-OS-student}"
sudo dmesg | tail -n 10
sudo rmmod os_hello
sudo dmesg | tail -n 10

echo "PASS: os_hello module loaded and removed. Check dmesg lines containing os_hello."
