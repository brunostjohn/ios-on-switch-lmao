#!/bin/bash
# Compares switch_apfs_read_tool's walk of one volume with macOS's own read of it, mounted read-only with owners:
# every entry's path, inode, owner, mode, size, flags, link count, mtime and xattr names, and the content of one file
# in N. Needs macOS and passwordless sudo. The image is attached read-only and detached again.
#
#   read_compare.sh <tool> <mount_list.py> <image> <volume uuid> <hash every> <work dir>
set -o pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
tool=$1
lister=$2
image=$3
uuid=$4
every=$5
work=$6
disk=
mounted=

cleanup() {
    if [[ -n $mounted ]]; then
        diskutil unmount force "$mounted" >/dev/null 2>&1
    fi
    if [[ -n $disk ]]; then
        hdiutil detach -force "/dev/$disk" >/dev/null 2>&1
    fi
    mounted=
    disk=
}
trap cleanup EXIT

finish() {
    cleanup
    exit "$1"
}

lines() {
    wc -l <"$1" | tr -d ' '
}

mkdir -p "$work/mnt" || finish 1
if ! plist=$(hdiutil attach -plist -readonly -nomount -imagekey diskimage-class=CRawDiskImage "$image" 2>/dev/null); then
    printf 'attach %s failed\n' "$image"
    finish 1
fi
disk=$(printf '%s\n' "$plist" | grep -o '/dev/disk[0-9]*' | head -1 | cut -c6-)
slices=$(printf '%s\n' "$plist" | grep -o '/dev/disk[0-9]*s[0-9]*' | cut -c6- | sort -u)
slice=
for s in $slices; do
    if diskutil info "$s" | grep -qi "Volume UUID: *$uuid"; then
        slice=$s
    fi
done
if [[ -z $slice ]]; then
    printf 'volume %s not found in %s\n' "$uuid" "$image"
    finish 1
fi
diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt" "$slice" >/dev/null || finish 1
mounted=$work/mnt
mount | grep " on $work/mnt "

start=$SECONDS
if ! sudo -n /usr/bin/python3 -I "$lister" "$work/mnt" "$every" | LC_ALL=C sort >"$work/mount.txt"; then
    printf 'listing the mount failed\n'
    finish 1
fi
n=$(lines "$work/mount.txt")
printf 'mount: %s entries in %s s\n' "$n" $((SECONDS - start))
start=$SECONDS
if ! "$tool" walk "$image" --volume "$uuid" --hash-every "$every" | LC_ALL=C sort >"$work/walk.txt"; then
    printf 'switch_apfs_read_tool walk failed\n'
    finish 1
fi
n=$(lines "$work/walk.txt")
printf 'walk: %s entries in %s s\n' "$n" $((SECONDS - start))
awk -F'\t' '$2 == "f" && $11 != "-"' "$work/walk.txt" >"$work/hashed.txt"
awk -F'\t' '$2 == "f" && $11 ~ /^c[0-9]+:/' "$work/walk.txt" >"$work/compressed.txt"
hashed=$(lines "$work/hashed.txt")
compressed=$(lines "$work/compressed.txt")
printf 'hashed: %s files, %s of them compressed\n' "$hashed" "$compressed"
if cmp -s "$work/mount.txt" "$work/walk.txt"; then
    sum=$(shasum -a 256 <"$work/walk.txt" | cut -c1-16)
    printf 'identical: %s\n' "$sum"
    finish 0
fi
diff "$work/mount.txt" "$work/walk.txt" >"$work/diff.txt"
n=$(grep -c '^[<>]' "$work/diff.txt")
printf 'DIFFERENT: %s lines differ\n' "$n"
head -40 "$work/diff.txt"
finish 1
