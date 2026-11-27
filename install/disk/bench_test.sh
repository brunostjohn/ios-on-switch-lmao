#!/bin/bash
# Write speed through switch_disk at 1, 4 and 8 MiB requests, flush included, onto a raw disk image attached to this
# Mac, and dd's speed onto the same node for comparison. It measures the disk-image driver and this Mac's disk, not a
# card.
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
tool=$PWD/$1
mib=${2:-1024}
work=$(mktemp -d /tmp/switch_disk_bench.XXXXXX)
disk=
cleanup() {
    if [[ -n $disk ]]; then
        hdiutil detach -force "/dev/$disk" >/dev/null 2>&1 || true
    fi
    if [[ $work == /tmp/switch_disk_bench.* && -d $work ]]; then
        rm -rf -- "$work"
    fi
}
trap cleanup EXIT

host=$(hostname)
macos=$(sw_vers -productVersion)
echo "host=$host macos=$macos"
mkfile -n "$((mib + 64))m" "$work/card.img"
disk=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$work/card.img" | awk '{print $1; exit}' | cut -c6-)
echo "attached /dev/$disk"
"$tool" bench "/dev/$disk" --mib "$mib" --requests 1,4,8 --repeat 3
for bs in 1 8; do
    dd if=/dev/zero of="/dev/r$disk" bs="${bs}m" count=$((mib / bs)) 2>&1 | tail -1 | sed "s/^/dd bs=${bs}m: /"
done
