#!/bin/bash
# What the image-file tests share: attaching a raw image without mounting it, finding a volume's slice by UUID, and
# fsck_apfs that passes only on the expected number of clean volumes. Sourced; the caller sets failures, vols and
# disk.
set -o pipefail

say() { printf '%s\n' "$*"; }
fail() {
    say "FAIL: $*"
    failures=$((failures + 1))
}
die() {
    say "FAIL: $*"
    exit 1
}

attach() {
    local image=$1 plist found s
    shift
    disk=
    vols=()
    plist=$(hdiutil attach -plist -nomount -imagekey diskimage-class=CRawDiskImage "$@" "$image" 2>/dev/null) || return 1
    disk=$(printf '%s\n' "$plist" | grep -o '/dev/disk[0-9]*' | head -1 | cut -c6-)
    attached+=("$disk")
    found=$(printf '%s\n' "$plist" | grep -o '/dev/disk[0-9]*s[0-9]*' | cut -c6- | sort -u)
    while IFS= read -r s; do
        if [[ -n $s ]]; then
            vols+=("$s")
        fi
    done <<<"$found"
}

volume_slice() {
    local s
    for s in ${vols[@]+"${vols[@]}"}; do
        if diskutil info "$s" | grep -q "Volume UUID: *$1"; then
            say "$s"
            return 0
        fi
    done
    return 1
}

detach_all() {
    local d
    for d in ${attached[@]+"${attached[@]}"}; do
        hdiutil detach -force "/dev/$d" >/dev/null 2>&1
    done
    attached=()
}

fsck_clean() {
    local image=$1 log=$2 want=$3 rc oks bad
    fsck_apfs -n "$image" >"$log" 2>&1
    rc=$?
    oks=$(grep -c 'appears to be OK' "$log")
    bad=$(grep -iE '^(warning|error)|corrupt|invalid|fail' "$log" | grep -v 'not a valid disk name/path')
    [[ $rc == 0 && $oks == "$want" && -z $bad ]]
}
