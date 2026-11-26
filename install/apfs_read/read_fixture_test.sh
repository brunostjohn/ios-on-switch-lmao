#!/bin/bash
# The reader on volumes Apple's own tools wrote. newfs_apfs makes two; macOS's kernel fills them with every record kind
# the writer copies: hard links, symlinks, embedded and stream xattrs, an ACL, compressed files, flags, owners, a
# Unicode and a tab in names, and enough files for index levels. One also gets a sparse file, a clone and a small
# compressed file; apfs_prepare_cryptex seals the other, which refuses those three.
#
# For each: the walk equals macOS's read through a read-only mount with owners (read_compare.sh, every file hashed),
# the allocation audit passes, and on the sealed one every node hash and every uncompressed stream's file-info hash
# checks. Negative controls on copies: a sealed node byte, a sealed data byte, and two bitmap bits swapped.
#
# Needs macOS with hdiutil, newfs_apfs, apfs_prepare_cryptex and passwordless sudo.
set -o pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
tool=$PWD/$1
lister=$PWD/$2
compare=$PWD/$3
R=/System/Library/Filesystems/apfs.fs/Contents/Resources
work=$(mktemp -d /tmp/apfs_read.XXXXXX)
disk=
vol=
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

attach() {
    local plist
    plist=$(hdiutil attach -plist -nomount -imagekey diskimage-class=CRawDiskImage "$1") || return 1
    disk=$(printf '%s\n' "$plist" | grep -o '/dev/disk[0-9]*' | head -1 | cut -c6-)
    vol=$(printf '%s\n' "$plist" | grep -o '/dev/disk[0-9]*s[0-9]*' | cut -c6- | sort -u | tail -1)
    [[ -n $disk ]]
}

detach() {
    if [[ -n $disk ]]; then
        hdiutil detach -force "/dev/$disk" >/dev/null 2>&1
    fi
    disk=
}

cleanup() {
    diskutil unmount force "$work/mnt" >/dev/null 2>&1
    detach
    if [[ $work == /tmp/apfs_read.* && -d $work ]]; then
        rm -rf -- "$work"
    fi
}
trap cleanup EXIT

populate() {
    local mp=$1 sealable=$2 i long big
    mkdir -p "$mp/many" "$mp/data" "$mp/deep/a/b/c" "$mp/private/var/mobile/Library" "$mp/empty"
    for i in $(seq 1 3000); do
        : >"$mp/many/f$i"
    done
    for i in $(seq 1 150); do
        printf 'data %s\n' "$i" >"$mp/data/d$i"
    done
    dd if=/dev/urandom of="$mp/deep/a/b/c/blob" bs=4096 count=300 2>/dev/null
    ln "$mp/data/d1" "$mp/deep/hard1"
    ln "$mp/data/d1" "$mp/deep/a/hard2"
    ln -s ../data/d2 "$mp/deep/link"
    long=$(printf '%0300d' 0 | tr 0 x)
    ln -s "$long" "$mp/deep/long-link"
    xattr -w com.example.small value "$mp/data/d3"
    big=$(head -c 9000 /dev/urandom | base64)
    xattr -w com.example.big "$big" "$mp/data/d4"
    ditto --hfsCompression "$work/web2" "$mp/words" || return 1
    chmod +a "everyone deny delete" "$mp/data/d9"
    : >"$mp/café-nfc"
    : >"$mp/$(printf 'nai\xcc\x88ve-nfd')"
    : >"$mp/$(printf 'tab\tname')"
    printf 'mobile\n' >"$mp/private/var/mobile/Library/prefs"
    if [[ $sealable == no ]]; then
        yes abcdefgh | head -c 20000 >"$work/small"
        ditto --hfsCompression "$work/small" "$mp/small" || return 1
        dd if=/dev/urandom of="$mp/sparse" bs=4096 count=1 seek=1000 2>/dev/null
        cp -c "$mp/deep/a/b/c/blob" "$mp/clone" || return 1
        cp -c "$mp/deep/a/b/c/blob" "$mp/clone-changed" || return 1
        printf x | dd of="$mp/clone-changed" bs=1 seek=5000 conv=notrunc 2>/dev/null
    fi
    sudo -n chown -R 501:20 "$mp/private/var/mobile" || return 1
    sudo -n chown 0:0 "$mp/data/d8" || return 1
    chmod 4755 "$mp/data/d7"
    chflags hidden "$mp/data/d5"
    chflags uchg "$mp/data/d6"
}

step() {
    "$@" >"$work/step.txt" 2>&1 || {
        say "step failed: $*"
        cat "$work/step.txt"
        return 1
    }
}

# Makes $work/$1.img, fills it, and seals it when $2 is "sealed".
make_fixture() {
    local name=$1 kind=$2 image=$work/$1.img sealable=no
    if [[ $kind == sealed ]]; then
        sealable=yes
    fi
    step truncate -s 96m "$image" || return 1
    step attach "$image" || return 1
    step newfs_apfs -e -v "Fixture-$name" "/dev/$disk" || return 1
    detach
    step attach "$image" || return 1
    step diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt" "$vol" || return 1
    step populate "$work/mnt" "$sealable" || return 1
    step diskutil unmount "$work/mnt" || return 1
    if [[ $kind == sealed ]]; then
        step "$R/apfs_prepare_cryptex" -p "/dev/$vol" || return 1
        step "$R/apfs_checkseal" "/dev/$vol" || return 1
    fi
    detach
}

uuid_of() {
    "$tool" info "$1" | sed -n 's/^volume 0 .* uuid \([0-9A-F-]*\) .*/\1/p'
}

# check_fixture <name> <compressed files at least> <shared blocks at least>
check_fixture() {
    local name=$1 image=$work/$1.img uuid n shared summary kinds
    uuid=$(uuid_of "$image")
    say "== $name: volume $uuid"
    "$tool" info "$image" || fail "$name: info"
    if "$tool" audit "$image" >"$work/$name-audit.txt"; then
        summary=$(head -2 "$work/$name-audit.txt" | tr '\n' ';')
        say "audit: $summary"
    else
        cat "$work/$name-audit.txt"
        fail "$name: audit"
    fi
    if out=$("$tool" walk "$image" --volume 0 --hash-every 0 --check-file-info 2>&1 >/dev/null); then
        say "walk: $out"
    else
        fail "$name: walk --check-file-info: $out"
    fi
    "$compare" "$tool" "$lister" "$image" "$uuid" 1 "$work/cmp-$name" || fail "$name: the walk differs from the mount"
    n=$(awk -F'\t' '$11 ~ /^c[0-9]+:/' "$work/cmp-$name/walk.txt" | wc -l | tr -d ' ')
    kinds=$(awk -F'\t' '$11 ~ /^c[0-9]+:/ { sub(/:.*/, "", $11); print $11 }' "$work/cmp-$name/walk.txt" \
        | sort | uniq -c | tr -s ' \n' ' ')
    say "compressed: $n ($kinds)"
    ((n >= $2)) || fail "$name: $n compressed files, $2 expected"
    shared=$(sed -n 's/.* shared \([0-9]*\)$/\1/p' "$work/$name-audit.txt")
    ((shared >= $3)) || fail "$name: $shared shared blocks, $3 expected"
    # Apple's own count leaves the volume superblock out.
    awk '/^volume / && $5 != $7 + 1 { bad = 1 } END { exit bad }' "$work/$name-audit.txt" \
        || fail "$name: the audit's blocks aren't fs_alloc_count + 1"
}

expect_fail() {
    local what=$1 out
    shift
    if out=$("$@" 2>&1 >/dev/null); then
        fail "control did not bite: $what"
    else
        say "control: $what fails: ${out##*$'\n'}"
    fi
}

host=$(hostname)
macos=$(sw_vers -productVersion)
say "host=$host macos=$macos work=$work"
sudo -n true || die "passwordless sudo is required"
mkdir -p "$work/mnt"
# ditto can't read the sealed system's copy; M1 met the same.
cp /usr/share/dict/web2 "$work/web2" || die "copy the word list"
chmod 644 "$work/web2" || die "copy the word list"
make_fixture plain plain || die "make the plain fixture"
make_fixture sealed sealed || die "make the sealed fixture"
check_fixture plain 2 290
check_fixture sealed 1 0

sealed=$work/sealed.img
bad=$work/bad.img
node=$("$tool" audit "$sealed" --list | awk '$1 == "claim" && $4 ~ /^FsTree/ { print $2 }' | sed -n 2p)
cp "$sealed" "$bad"
printf '\x5a' | dd of="$bad" bs=1 seek=$((node * 4096 + 1000)) conv=notrunc 2>/dev/null
expect_fail "a sealed fs-tree node byte" "$tool" walk "$bad" --volume 0 --hash-every 0
d10=$(awk -F'\t' '$1 == "/data/d10" { print $3 }' "$work/cmp-sealed/walk.txt")
data=$("$tool" audit "$sealed" --list | awk -v s="$d10" '$1 == "claim" && $4 == "Data(0," && $5 == s"," { print $2 }')
cp "$sealed" "$bad"
printf '\x5a' | dd of="$bad" bs=1 seek=$((data * 4096 + 2)) conv=notrunc 2>/dev/null
"$tool" walk "$bad" --volume 0 --hash-every 0 >/dev/null 2>&1 || fail "a data byte stops the walk"
expect_fail "a sealed data byte, against its file info" "$tool" walk "$bad" --volume 0 --hash-every 0 --check-file-info

plain=$work/plain.img
bitmap=$("$tool" info "$plain" | sed -n 's/.*chunk bitmaps \[\([0-9]*\).*/\1/p')
blocks=$("$tool" info "$plain" | sed -n 's/^container .* blocks \([0-9]*\) .*/\1/p')
used=$("$tool" audit "$plain" --list | awk '$1 == "claim" && $4 ~ /^FsTree/ { print $2; exit }')
cp "$plain" "$bad"
/usr/bin/python3 -I - "$bad" "$bitmap" "$used" "$blocks" <<'EOF' || fail "swap two bitmap bits"
import sys
path, bitmap, used, blocks = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
with open(path, 'r+b') as f:
    f.seek(bitmap * 4096)
    bits = bytearray(f.read(4096))
    free = next(i for i in range(blocks - 1, 0, -1) if not bits[i // 8] >> (i % 8) & 1)
    bits[used // 8] &= ~(1 << used % 8)
    bits[free // 8] |= 1 << free % 8
    f.seek(bitmap * 4096)
    f.write(bits)
EOF
expect_fail "two bitmap bits swapped" "$tool" audit "$bad"
"$tool" audit "$bad" | grep '^problem' | head -3

say "failures: $failures"
[[ $failures == 0 ]]
