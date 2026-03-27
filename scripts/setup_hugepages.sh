#!/bin/bash

# Porth-IO: HugePage Reservation Script
# This must be run as root on the target Linux server.
# It reserves 1GB of memory (512 x 2MB pages) that the kernel cannot touch.

if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root. Try: sudo ./scripts/setup_hugepages.sh" 
   exit 1
fi

echo "[Porth-IO] Reserving 512 HugePages (approx 1GB of RAM)..."

# Reserve 512 pages of 2MB each
sysctl -w vm.nr_hugepages=512

# Verify the reservation by checking the kernel's memory info
grep HugePages /proc/meminfo

echo ""
echo "[Porth-IO] Memory optimized. Your system is ready for the Shuttle."
echo "Note: This setting will reset after a reboot unless added to /etc/sysctl.conf"