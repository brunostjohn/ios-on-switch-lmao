#!/bin/bash
# The writer on a stand-in for Apple's container: a newfs_apfs System volume with real files, then the Data volume
# added by switch_apfs_m1 at 16 GiB (the source's checkpoint areas kept, and resized) and at 1 TiB (kept).
#
# Each output is checked in userland first (our reader, dpp's reader, fsck_apfs -n on the file) and attached only
# after those pass, so the kernel never probes an image fsck rejects. Then: both volumes attach, each mounts read-only
# with owners, /hello reads back 0:0 0644, System's files and owners are as they were, and a read-write mount of a
# clone writes files and passes fsck again. Negative controls: a main free-queue limit off by one attaches no volume;
# a wrong child count on Data's root fails fsck.
#
# Needs macOS with hdiutil, newfs_apfs, fsck_apfs and passwordless sudo (the fixture and the read-write check write
# root-owned files).
set -o pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
tool=$PWD/$1
check=$PWD/$2
work=$(mktemp -d /tmp/apfs_m1.XXXXXX)
DATA_UUID=D1B1D053-D0A6-496C-8625-A947A3DA863E
DATA_NAME=SwitchDataTrial
TIME_NS=1795000000000000000
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

entries() {
    find "$1" -mindepth 1 -maxdepth 1 -print | sed 's|.*/||' | sort | tr '\n' ' '
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

cleanup() {
    detach_all
    if [[ $work == /tmp/apfs_m1.* && -d $work ]]; then
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

src=$work/source.img
truncate -s 256m "$src"
attach "$src" || die "attach the blank source"
newfs_apfs -e -v System "/dev/$disk" >/dev/null || die "newfs_apfs"
detach_all
attach "$src" || die "attach the source"
[[ ${#vols[@]} == 1 ]] || die "the source volume did not appear"
diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt" "${vols[0]}" >/dev/null || die "mount the source"
mkdir -p "$work/mnt/private/var/mobile/Library/Preferences" "$work/mnt/usr/bin"
say hosts >"$work/mnt/private/etc-hosts"
dd if=/dev/urandom of="$work/mnt/usr/bin/tool" bs=1024 count=1025 2>/dev/null
cp /usr/share/dict/web2 "$work/words.src" || die "copy the word list"
chmod 644 "$work/words.src"
ditto --hfsCompression "$work/words.src" "$work/mnt/words" || die "ditto --hfsCompression"
flags=$(stat -f %Sf "$work/mnt/words")
[[ $flags == *compressed* ]] || die "the word list isn't compressed"
ln -s words "$work/mnt/link"
xattr -w com.example.tag value "$work/mnt/words" || die "xattr"
sudo -n chown -R 0:0 "$work/mnt/usr"
sudo -n chown -R 501:501 "$work/mnt/private/var/mobile"
words_sum=$(shasum -a 256 <"$work/mnt/words")
tool_sum=$(shasum -a 256 <"$work/mnt/usr/bin/tool")
diskutil unmount "$work/mnt" >/dev/null || die "unmount the source"
detach_all

system_uuid=$("$tool" inspect "$src" | sed -n 's/^volume 0 .* uuid \([0-9A-F-]*\) .*/\1/p')
[[ -n $system_uuid ]] || die "inspect the source"
"$tool" set-role "$src" --uuid "$system_uuid" --role 1 || die "set-role"
"$tool" inspect "$src" || die "inspect the source after set-role"
if ! fsck_clean "$src" "$work/fsck-source.txt" 2; then
    cat "$work/fsck-source.txt"
    die "fsck of the source"
fi
system_listing=$("$check" "$src" | grep "^volume .* System ")
[[ -n $system_listing ]] || die "dpp reads the source"
say "source System: $system_listing"

read_only_checks() {
    local name=$1 data system st hello owner words_now tool_now listing
    data=$(volume_slice "$DATA_UUID")
    system=$(volume_slice "$system_uuid")
    say "attached $disk: volumes ${vols[*]}, Data $data, System $system"
    if [[ ${#vols[@]} != 2 || -z $data || -z $system ]]; then
        fail "$name: both volumes attach"
        return
    fi
    diskutil info "$data" | grep -iE 'Volume Name|Volume UUID|Role|Read-Only'
    diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt" "$data" >/dev/null \
        || fail "$name: mount Data"
    st=$(stat -f '%u:%g %Sp %z' "$work/mnt/hello" 2>&1)
    hello=$(cat "$work/mnt/hello")
    listing=$(entries "$work/mnt")
    say "Data: $listing/hello $st $hello"
    [[ $st == "0:0 -rw-r--r-- 6" && $hello == hello ]] || fail "$name: /hello on the mount"
    diskutil unmount "$work/mnt" >/dev/null
    diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt2" "$system" >/dev/null \
        || fail "$name: mount System"
    owner=$(stat -f '%u:%g' "$work/mnt2/usr/bin/tool")
    words_now=$(shasum -a 256 <"$work/mnt2/words")
    tool_now=$(shasum -a 256 <"$work/mnt2/usr/bin/tool")
    listing=$(entries "$work/mnt2")
    say "System: $listing/usr/bin/tool $owner"
    [[ $words_now == "$words_sum" && $tool_now == "$tool_sum" ]] || fail "$name: System's files"
    [[ $owner == 0:0 ]] || fail "$name: System's owners"
    diskutil unmount "$work/mnt2" >/dev/null
}

read_write_checks() {
    local name=$1 data inspected listing
    if ! attach "$work/rw.img"; then
        fail "$name: attach read-write"
        return
    fi
    data=$(volume_slice "$DATA_UUID")
    if diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt" "$data" >/dev/null; then
        sudo -n /bin/cp "$work/mnt/hello" "$work/mnt/world"
        sudo -n /bin/mkdir "$work/mnt/dir"
        sudo -n /bin/cp "$work/mnt/hello" "$work/mnt/dir/copy"
        sync
        listing=$(entries "$work/mnt")
        say "read-write Data: $listing"
        diskutil unmount "$work/mnt" >/dev/null || fail "$name: unmount read-write"
    else
        fail "$name: mount Data read-write"
    fi
    detach_all
    if fsck_clean "$work/rw.img" "$work/fsck-$name-rw.txt" 3; then
        inspected=$("$tool" inspect "$work/rw.img" | head -1)
        say "after the read-write mount: fsck clean; $inspected"
    else
        cat "$work/fsck-$name-rw.txt"
        fail "$name: fsck after a read-write mount"
    fi
}

run() {
    local name=$1 bytes=$2 areas=$3 out seen system_seen
    out=$work/$name.img
    say "== $name: $bytes bytes, areas $areas"
    if ! "$tool" add-volume --source "$src" --target "$out" --target-bytes "$bytes" --system-uuid "$system_uuid" \
        --uuid "$DATA_UUID" --name "$DATA_NAME" --role 0x40 --features 0x2 --rocompat 0x0c --incompat 0x200 \
        --unicode 17.0 --areas "$areas" --time-ns "$TIME_NS"; then
        fail "$name: add-volume"
        return
    fi
    "$tool" inspect "$out" || fail "$name: our reader"
    if ! seen=$("$check" "$out" --file "$DATA_NAME:hello"); then
        fail "$name: dpp's reader"
        return
    fi
    say "$seen"
    [[ $seen == *"$DATA_NAME:/hello 0:0 100644 nlink 1 \"hello\\n\""* ]] || fail "$name: dpp's /hello"
    system_seen=$(printf '%s\n' "$seen" | grep "^volume .* System ")
    [[ $system_seen == "$system_listing" ]] || fail "$name: dpp's System listing"
    if ! fsck_clean "$out" "$work/fsck-$name.txt" 3; then
        cat "$work/fsck-$name.txt"
        fail "$name: fsck_apfs -n"
        return
    fi
    say "fsck_apfs -n: 3 'appears to be OK', no warnings"
    if attach "$out" -readonly; then
        read_only_checks "$name"
    else
        fail "$name: attach"
    fi
    detach_all
    cp -c "$out" "$work/rw.img"
    read_write_checks "$name"
    rm -f -- "$work/rw.img"
}

run keep-16g $((16 << 30)) keep
run resized-16g $((16 << 30)) resized
run keep-1t $((1 << 40)) keep

say "== negative controls, on keep-16g"
cp -c "$work/keep-16g.img" "$work/neg-queue.img"
"$tool" mutate "$work/neg-queue.img" queue-limit >/dev/null || die "mutate queue-limit"
if fsck_clean "$work/neg-queue.img" "$work/fsck-neg-queue.txt" 3; then
    attach "$work/neg-queue.img" -readonly
    say "main queue limit off by one: fsck passes; ${#vols[@]} volumes attach"
    [[ ${#vols[@]} == 0 ]] || fail "a wrong queue limit still attached volumes"
    detach_all
else
    say "main queue limit off by one: fsck_apfs rejects it"
fi
cp -c "$work/keep-16g.img" "$work/neg-leaf.img"
"$tool" mutate "$work/neg-leaf.img" data-leaf --uuid "$DATA_UUID" >/dev/null || die "mutate data-leaf"
if fsck_clean "$work/neg-leaf.img" "$work/fsck-neg-leaf.txt" 3; then
    fail "fsck_apfs passed a wrong child count"
else
    errors=$(grep -iE '^(error|warning)' "$work/fsck-neg-leaf.txt" | grep -v 'disk name' | head -2 | tr '\n' ' ')
    say "wrong child count: fsck_apfs: $errors"
fi

say "failures: $failures"
[[ $failures == 0 ]]
