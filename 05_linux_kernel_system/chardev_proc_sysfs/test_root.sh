#!/usr/bin/env bash
set -euo pipefail

make
sudo insmod os_lab_driver.ko capacity=4096
trap 'sudo rmmod os_lab_driver || true' EXIT

sleep 1
test -e /dev/os_lab_char
printf 'hello from os lab\n' | sudo tee /dev/os_lab_char >/dev/null
cat /dev/os_lab_char
cat /proc/os_lab_info
cat /sys/class/os_lab/os_lab_char/stats
printf '1\n' | sudo tee /sys/class/os_lab/os_lab_char/reset >/dev/null
cat /sys/class/os_lab/os_lab_char/stats

sudo rmmod os_lab_driver
trap - EXIT
echo "PASS: character device, /proc, and /sys interfaces all responded."
