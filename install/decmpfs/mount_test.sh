#!/bin/bash
# Every compressed file of a member's volume, decompressed by switch_decmpfs, against macOS's read of it through a
# read-only mount of the same image, owners off (switch_translate_tool decmpfs-check). Needs macOS with hdiutil.
#
#   mount_test.sh TOOL IMAGE [IPSW_PINS]     with IPSW_PINS, the OS image's System volume; else the only volume
set -uo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
tool=$PWD/$1
image=$PWD/$2
uuid=
if [[ $# -ge 3 ]]; then
    uuid=$(plutil -extract volumes.os_volume_uuid raw -o - "$PWD/$3") || exit 1
fi
root=$(mktemp -d /tmp/decmpfs_mount.XXXXXX)
disk=

cleanup() {
    if [[ -n $disk ]]; then
        hdiutil detach -force "$disk" >/dev/null 2>&1
    fi
    rmdir -- "$root" 2>/dev/null
}
trap cleanup EXIT

plist=$(hdiutil attach -readonly -nobrowse -noverify -noautofsck -owners off -mountroot "$root" -plist "$image") || {
    echo "FAIL: hdiutil attach"
    exit 1
}
disk=$(printf '%s\n' "$plist" | grep -o '/dev/disk[0-9]*' | head -1)
mounts=$(printf '%s\n' "$plist" | grep -A1 '<key>mount-point</key>' | grep -o '<string>.*</string>' | sed 's/<[^>]*>//g')
mnt=
while read -r m; do
    if [[ -z $uuid ]] || diskutil info "$m" | grep -qi "Volume UUID: *$uuid"; then
        mnt=$m
    fi
done <<<"$mounts"
if [[ -z $mnt ]]; then
    echo "FAIL: no mounted volume ${uuid:-at all}"
    exit 1
fi
"$tool" decmpfs-check --image "$image" --mount "$mnt" ${uuid:+--volume "$uuid"}
