#!/bin/bash
# The B-tree builder against Apple: see BUILD.bazel. Needs macOS with hdiutil, newfs_apfs, fsck_apfs,
# apfs_prepare_cryptex and passwordless sudo (the read-write check writes into root-owned directories).
set -o pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
R=/System/Library/Filesystems/apfs.fs/Contents/Resources
check=$PWD/$1
m1=$PWD/$2
work=$(mktemp -d /tmp/apfs_w2.XXXXXX)
DIRS=200
FILES=100
DATA_UUID=D1B1D053-D0A6-496C-8625-A947A3DA863E
DATA_NAME=Switch-iOS-Data
TIME_NS=1795000000000000000
TARGET_BYTES=$((8 << 30))
attached=()
vols=()
disk=
failures=0

say() { printf '%s\n' "$*"; }
fail() {
    say "FAIL: $*"
    failures=$((failures + 1))
}
die() {
    say "FAIL: $*"
    exit 1
}

# Sets $disk to the image's device and $vols to the APFS volumes the kernel synthesised from it.
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
    return 0
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
        diskutil unmountDisk force "/dev/$d" >/dev/null 2>&1
        hdiutil detach -force "/dev/$d" >/dev/null 2>&1
    done
    attached=()
}

cleanup() {
    detach_all
    if [[ $work == /tmp/apfs_w2.* && -d $work ]]; then
        rm -rf -- "$work"
    fi
}
trap cleanup EXIT

fsck_clean() {
    local image=$1 log=$2 want=$3 rc oks bad
    fsck_apfs -n "$image" >"$log" 2>&1
    rc=$?
    oks=$(grep -c 'appears to be OK' "$log")
    bad=$(grep -iE '^(warning|error)|corrupt|invalid|fail' "$log" | grep -v 'not a valid disk name/path')
    [[ $rc == 0 && $oks == "$want" && -z $bad ]]
}

host=$(hostname)
macos=$(sw_vers -productVersion)
say "host=$host macos=$macos work=$work"
sudo -n true || die "passwordless sudo is required"
mkdir -p "$work/mnt" "$work/mnt2"

say "== macOS's own trees for $DIRS x $FILES files, then sealed by apfs_prepare_cryptex"
apple=$work/apple.img
truncate -s 1g "$apple"
attach "$apple" || die "attach the blank image"
newfs_apfs -e -v Apple "/dev/$disk" >/dev/null || die "newfs_apfs"
detach_all
attach "$apple" || die "attach Apple's volume"
diskutil mount -mountOptions nobrowse -mountPoint "$work/mnt" "${vols[0]}" >/dev/null || die "mount Apple's volume"
"$check" make-tree "$work/mnt" --dirs "$DIRS" --files "$FILES" || die "make-tree"
diskutil unmount "$work/mnt" >/dev/null || die "unmount Apple's volume"
detach_all
"$check" compare "$apple" --require extentref || fail "macOS's plain volume"
cp -c "$apple" "$work/sealed.img"
attach "$work/sealed.img" || die "attach the copy to seal"
if ! "$R/apfs_prepare_cryptex" -p "/dev/${vols[0]}" >"$work/seal.log" 2>&1; then
    cat "$work/seal.log"
    die "apfs_prepare_cryptex"
fi
"$R/apfs_checkseal" "/dev/${vols[0]}" >/dev/null 2>&1 || fail "apfs_checkseal on Apple's seal"
detach_all
"$check" compare "$work/sealed.img" --require fext,extentref || fail "macOS's sealed volume"
rm -f -- "$apple" "$work/sealed.img"

say "== our Data volume with the same files, through M1's container layer"
src=$work/source.img
truncate -s 256m "$src"
attach "$src" || die "attach the blank source"
newfs_apfs -e -v System "/dev/$disk" >/dev/null || die "newfs_apfs the source"
detach_all
system_uuid=$("$m1" inspect "$src" | sed -n 's/^volume 0 .* uuid \([0-9A-F-]*\) .*/\1/p')
[[ -n $system_uuid ]] || die "inspect the source"
"$m1" set-role "$src" --uuid "$system_uuid" --role 1 >/dev/null || die "set-role"
out=$work/out.img
add() {
    "$check" add-volume --source "$src" --target "$1" --target-bytes "$TARGET_BYTES" --system-uuid "$system_uuid" \
        --uuid "$DATA_UUID" --name "$DATA_NAME" --time-ns "$TIME_NS" --dirs "$DIRS" --files "$FILES"
}
add "$out" || die "add-volume"
add "$work/again.img" >/dev/null || die "add-volume, second build"
if cmp -s "$out" "$work/again.img"; then
    say "two builds: byte-identical"
else
    fail "two builds differ"
fi
rm -f -- "$work/again.img"
"$check" compare "$out" --volume "$DATA_NAME" --require fs,extentref,omap || fail "our trees, read back"
if fsck_clean "$out" "$work/fsck.txt" 3; then
    say "fsck_apfs -n: 3 'appears to be OK', no warnings"
else
    cat "$work/fsck.txt"
    die "fsck_apfs -n"
fi

attach "$out" -readonly || die "attach our container"
data=$(volume_slice "$DATA_UUID")
system=$(volume_slice "$system_uuid")
say "attached $disk: volumes ${vols[*]}, Data $data, System $system"
[[ ${#vols[@]} == 2 && -n $data && -n $system ]] || fail "both volumes attach"
if diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt" "$data" >/dev/null; then
    "$check" verify "$work/mnt" --dirs "$DIRS" --files "$FILES" || fail "the read-back"
    found=$(find "$work/mnt" -mindepth 1 -print | grep -c .)
    owner=$(stat -f '%u:%g %Sp' "$work/mnt/d1/file-7")
    say "find: $found entries; d1/file-7 $owner"
    diskutil unmount "$work/mnt" >/dev/null
else
    fail "mount Data read-only"
fi
diskutil mount readOnly -mountOptions nobrowse -mountPoint "$work/mnt2" "$system" >/dev/null || fail "mount System"
diskutil unmount "$work/mnt2" >/dev/null 2>&1
detach_all

say "== the kext changing our trees: a read-write mount of a copy"
cp -c "$out" "$work/rw.img"
attach "$work/rw.img" || die "attach read-write"
data=$(volume_slice "$DATA_UUID")
if diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt" "$data" >/dev/null; then
    sudo -n /bin/rm -rf "$work/mnt/d3" "$work/mnt/long"
    sudo -n /bin/mkdir "$work/mnt/new"
    for i in $(seq 1 300); do
        sudo -n /bin/cp "$work/mnt/d4/file-$((i % FILES))" "$work/mnt/new/copy-$i"
    done
    sudo -n /bin/mv "$work/mnt/d5" "$work/mnt/new/moved"
    sync
    found=$(find "$work/mnt/new" -mindepth 1 -maxdepth 1 -print | grep -c .)
    say "read-write: removed d3 and long, copied 300 files, moved d5; $found in new"
    diskutil unmount "$work/mnt" >/dev/null || fail "unmount read-write"
else
    fail "mount Data read-write"
fi
detach_all
if fsck_clean "$work/rw.img" "$work/fsck-rw.txt" 3; then
    inspected=$("$m1" inspect "$work/rw.img" | head -1)
    say "after the read-write mount: fsck clean; $inspected"
else
    cat "$work/fsck-rw.txt"
    fail "fsck after a read-write mount"
fi
rm -f -- "$work/rw.img"

say "== negative control: one byte of Data's last fs leaf flipped"
leaf=$("$check" last-fs-leaf "$out" "$DATA_NAME") || die "find a leaf"
cp -c "$out" "$work/neg.img"
byte=$(dd if="$work/neg.img" bs=1 skip=$((leaf * 4096 + 200)) count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
octal=$(printf '%03o' $((byte ^ 0xff)))
# shellcheck disable=SC2059 # the format is the escape that writes one raw byte
printf "\\$octal" | dd of="$work/neg.img" bs=1 seek=$((leaf * 4096 + 200)) conv=notrunc 2>/dev/null
if fsck_clean "$work/neg.img" "$work/fsck-neg.txt" 3; then
    fail "fsck_apfs passed a flipped byte in leaf $leaf"
else
    finding=$(grep -iE '^(error|warning)' "$work/fsck-neg.txt" | grep -v 'disk name' | head -1)
    say "leaf $leaf flipped: fsck_apfs: $finding"
fi

say "failures: $failures"
[[ $failures == 0 ]]
