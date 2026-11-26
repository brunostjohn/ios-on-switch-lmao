#!/bin/bash
# The writer's Data and Preboot on a stand-in for Apple's container. macOS's kernel fills a newfs_apfs System volume's
# /private/var with every kind of record the copy carries: hard links with names inside and outside the subtree,
# whole-file and rewritten clones (one of a file outside), compressed files with decmpfs embedded, as a stream and
# with a resource fork, a clone of each compressed file, sparse files, an ACL, streamed and embedded xattrs, symlinks, flags,
# several owners, Unicode names and enough files for index levels. switch_apfs_m1 build then copies /private/var into
# Data and writes Preboot with one seeded image.
#
# Checks: two builds byte-identical; our reader's allocation audit; Preboot's image in one extent; Data's records
# against the source's for what a mount can't show (switch_apfs_m1 fidelity); System's walk unchanged; fsck_apfs;
# all three volumes attach; Data, mounted read-only with owners, equals the source's /private/var through the kernel
# in path, owner, mode, flags, size, all four times, xattr names and values, content and hard-link groups
# (tree_compare.py), and our reader's walk of Data equals the kernel's (W1's read_compare.sh);
# the Preboot image reads back its seed's bytes; a read-write mount that clones, writes and unlinks keeps fsck clean;
# the power-cut suite, with every phase-boundary state attached. The suite's finished starting state has an 8,192-block
# descriptor area, too large for the low free run the new areas take, so only phase B can clear its checkpoint.
# Negative controls: a wrong root child count fails fsck, a wrong main free-queue limit attaches no volume, and a
# changed owner fails the comparison.
#
# Needs macOS with hdiutil, newfs_apfs, fsck_apfs and passwordless sudo.
set -o pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
tool=$PWD/$1
reader=$PWD/$2
lister=$PWD/$3
compare=$PWD/$4
mount_list=$PWD/$5
read_compare=$PWD/$6
work=$(mktemp -d /tmp/apfs_w4.XXXXXX)
DATA_UUID=D1B1D053-D0A6-496C-8625-A947A3DA863E
PREBOOT_UUID=4AA4A761-5138-439D-9A30-D4138F6887FA
TIME_NS=1795000000000000000
TARGET_BYTES=$((4 << 30))
IMAGE=cryptex.dmg
IMAGE_BYTES=$((48 << 20))
IMAGE_SEED=7
failures=0
vols=()
disk=
# shellcheck source=/dev/null
source "$(dirname "$0")/images.sh"

cleanup() {
    local m
    for m in mnt mnt-src mnt-boot; do
        diskutil unmount force "$work/$m" >/dev/null 2>&1
    done
    detach_all
    if [[ $work == /tmp/apfs_w4.* && -d $work ]]; then
        sudo -n rm -rf -- "$work"
    fi
}
trap cleanup EXIT

# Every kind of record the copy carries, in /private/var, with a few originals outside it.
populate() {
    local mp=$1 v=$1/private/var big long i
    sudo -n mkdir -p "$v/mobile/Library/Preferences" "$v/root" "$v/hl/sub" "$v/cl" "$v/c" "$v/x/deep/er/still" \
        "$v/many" "$v/names" "$v/empty-dir" "$mp/other" || return 1
    sudo -n chown -R "$me" "$mp/private" "$mp/other" || return 1
    printf 'prefs\n' >"$v/mobile/Library/Preferences/com.example.plist"
    for i in $(seq 1 2500); do
        : >"$v/many/f$i"
    done
    for i in $(seq 1 300); do
        printf 'data %s\n' "$i" >"$v/x/d$i"
    done
    printf 'linked\n' >"$v/hl/a"
    ln "$v/hl/a" "$v/hl/b"
    ln "$v/hl/a" "$v/hl/sub/c"
    printf 'outside\n' >"$mp/other/orig"
    ln "$mp/other/orig" "$v/hl/one-in"
    printf 'two in\n' >"$mp/other/x2"
    ln "$mp/other/x2" "$v/hl/two-in-1"
    ln "$mp/other/x2" "$v/hl/two-in-2"
    head -c 2097152 /dev/urandom >"$v/cl/x"
    cp -c "$v/cl/x" "$v/cl/y" || return 1
    cp -c "$v/cl/x" "$v/cl/z" || return 1
    printf X | dd of="$v/cl/z" bs=1 seek=5000 conv=notrunc 2>/dev/null
    head -c 300000 /dev/urandom >"$mp/other/big"
    cp -c "$mp/other/big" "$v/cl/from-outside" || return 1
    ditto --hfsCompression "$work/web2" "$v/c/words" || return 1
    cp -c "$v/c/words" "$v/c/words-clone" || return 1
    yes abcdefgh | head -c 20000 >"$work/small"
    ditto --hfsCompression "$work/small" "$v/c/small" || return 1
    cp -c "$v/c/small" "$v/c/small-clone" || return 1
    yes tiny | head -c 600 >"$work/tiny"
    ditto --hfsCompression "$work/tiny" "$v/c/tiny" || return 1
    cp -c "$v/c/tiny" "$v/c/tiny-clone" || return 1
    dd if=/dev/urandom of="$v/x/sparse" bs=4096 count=2 seek=700 2>/dev/null
    dd if=/dev/urandom of="$v/x/sparse-tail" bs=4096 count=1 seek=3 2>/dev/null
    truncate -s 1000000 "$v/x/sparse-tail"
    cp -c "$v/x/sparse" "$v/cl/sparse-clone" || return 1
    xattr -w com.example.small value "$v/x/d3"
    big=$(head -c 9000 /dev/urandom | base64)
    xattr -w com.example.big "$big" "$v/x/d4"
    cp -c "$v/x/d4" "$v/cl/d4-clone" || return 1
    chmod +a "everyone deny delete" "$v/x/d5"
    chmod +a "user:root allow read,write" "$v/hl/a"
    ln -s ../x/d1 "$v/x/link"
    long=$(printf '%0300d' 0 | tr 0 y)
    ln -s "$long" "$v/x/long-link"
    : >"$v/names/café-nfc"
    : >"$v/names/$(printf 'nai\xcc\x88ve-nfd')"
    : >"$v/names/$(printf 'tab\tname')"
    printf 'hangul\n' >"$v/names/$(printf '\xed\x95\x9c\xea\xb8\x80')"
    chmod 4755 "$v/x/d7"
    chflags hidden "$v/x/d8"
    sudo -n chown -R 501:501 "$v/mobile" || return 1
    sudo -n chown 0:0 "$v/root" "$v/x/d9" || return 1
    sudo -n chown 64:64 "$v/x/d10" || return 1
    sudo -n chflags uchg "$v/x/d6" || return 1
    sudo -n chflags schg "$v/x/d11" || return 1
    sudo -n touch -t 200102030405.06 "$v/x/d12" || return 1
}

build() {
    "$tool" build --source "$src" --target "$1" --target-bytes "$TARGET_BYTES" --system-uuid "$system_uuid" \
        --data-uuid "$DATA_UUID" --data-name SwitchDataTrial --data-features 0x2 --data-rocompat 0x0c \
        --data-incompat 0x200 --preboot-uuid "$PREBOOT_UUID" --preboot-name SwitchPrebootTrial \
        --preboot-features 0x2 --preboot-rocompat 0 --preboot-incompat 0 --unicode 17.0 --time-ns "$TIME_NS" \
        --preboot-file "$IMAGE:$IMAGE_BYTES:$IMAGE_SEED" "${@:2}"
}

host=$(hostname)
macos=$(sw_vers -productVersion)
uid=$(id -u)
gid=$(id -g)
me=$uid:$gid
say "host=$host macos=$macos work=$work"
sudo -n true || die "passwordless sudo is required"
mkdir -p "$work/mnt" "$work/mnt-src" "$work/mnt-boot" "$work/cuts"
cp /usr/share/dict/web2 "$work/web2" || die "copy the word list"
chmod 644 "$work/web2" || die "copy the word list"

src=$work/source.img
truncate -s 512m "$src"
attach "$src" || die "attach the blank source"
newfs_apfs -e -v System "/dev/$disk" >/dev/null || die "newfs_apfs"
detach_all
attach "$src" || die "attach the source"
diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt" "${vols[0]}" >/dev/null || die "mount the source"
populate "$work/mnt" || die "populate the source"
diskutil unmount "$work/mnt" >/dev/null || die "unmount the source"
detach_all
system_uuid=$("$tool" inspect "$src" | sed -n 's/^volume 0 .* uuid \([0-9A-F-]*\) .*/\1/p')
[[ -n $system_uuid ]] || die "inspect the source"
"$tool" set-role "$src" --uuid "$system_uuid" --role 1 >/dev/null || die "set-role"
if ! fsck_clean "$src" "$work/fsck-source.txt" 2; then
    cat "$work/fsck-source.txt"
    die "fsck of the source"
fi

out=$work/out.img
say "== build"
build "$out" || die "build"
build "$work/again.img" >/dev/null || die "the second build"
if cmp -s "$out" "$work/again.img"; then
    say "two builds: byte-identical"
else
    fail "two builds differ"
fi
rm -f -- "$work/again.img"
"$reader" info "$out" || fail "our reader"
if "$reader" audit "$out" >"$work/audit.txt"; then
    summary=$(head -4 "$work/audit.txt" | tr '\n' ';')
    say "audit: $summary"
else
    cat "$work/audit.txt"
    fail "the allocation audit"
fi
"$reader" audit "$out" --list >"$work/claims.txt" || fail "list the claims"
extents=$(awk '$1 == "claim" && $4 ~ /^Data\(2,/ { print $3 }' "$work/claims.txt" | tr '\n' ' ')
if [[ $extents == "$((IMAGE_BYTES / 4096)) " ]]; then
    say "Preboot's image: one extent of $((IMAGE_BYTES / 4096)) blocks"
else
    fail "Preboot's image is in extents of $extents blocks"
fi
if "$tool" fidelity --source "$src" --copy "$out" --system-uuid "$system_uuid" --data-uuid "$DATA_UUID" \
    >"$work/fidelity.txt"; then
    summary=$(head -1 "$work/fidelity.txt")
    say "record for record: $summary"
else
    cat "$work/fidelity.txt"
    fail "Data's records differ from the source's"
fi
"$reader" walk "$src" --volume "$system_uuid" >"$work/system-before.txt" 2>/dev/null || fail "walk the source's System"
"$reader" walk "$out" --volume "$system_uuid" >"$work/system-after.txt" 2>/dev/null || fail "walk the output's System"
if cmp -s "$work/system-before.txt" "$work/system-after.txt"; then
    count=$(wc -l <"$work/system-after.txt" | tr -d ' ')
    say "System: unchanged, $count entries, every file hashed"
else
    fail "System's walk changed"
fi
if fsck_clean "$out" "$work/fsck.txt" 4; then
    say "fsck_apfs -n: 4 'appears to be OK', no warnings"
else
    cat "$work/fsck.txt"
    fail "fsck_apfs -n"
fi

say "== the kernel's read"
attach "$src" -readonly || die "attach the source read-only"
diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt-src" "${vols[0]}" >/dev/null \
    || die "mount the source"
attach "$out" -readonly || die "attach the output"
say "attached $disk: ${vols[*]}"
[[ ${#vols[@]} == 3 ]] || fail "three volumes attach"
data=$(volume_slice "$DATA_UUID") || fail "Data attaches"
preboot=$(volume_slice "$PREBOOT_UUID") || fail "Preboot attaches"
diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt" "$data" >/dev/null || fail "mount Data"
diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt-boot" "$preboot" >/dev/null \
    || fail "mount Preboot"
sudo -n /usr/bin/python3 -I "$lister" "$work/mnt-src/private/var" | tee "$work/source.tsv" >/dev/null \
    || fail "list the source"
sudo -n /usr/bin/python3 -I "$lister" "$work/mnt" | tee "$work/data.tsv" >/dev/null || fail "list Data"
/usr/bin/python3 -I "$compare" "$work/source.tsv" "$work/data.tsv" || fail "Data differs from /private/var"
root=$(stat -f '%u:%g %Sp' "$work/mnt")
[[ $root == "0:0 drwxr-xr-x" ]] || fail "Data's root is $root"
image=$work/mnt-boot/Switch-Staging/$IMAGE
st=$(stat -f '%u:%g %Sp %z' "$image" 2>&1)
want=$("$tool" gen --bytes "$IMAGE_BYTES" --seed "$IMAGE_SEED" | shasum -a 256 | cut -c1-64)
got=$(shasum -a 256 <"$image" | cut -c1-64)
dirs=$(stat -f '%u:%g %Sp' "$work/mnt-boot" "$work/mnt-boot/Switch-Staging" | tr '\n' ' ')
say "Preboot: $dirs$st"
[[ $st == "0:0 -rw-r--r-- $IMAGE_BYTES" && $want == "$got" ]] || fail "Preboot's image"
"$reader" walk "$out" --volume "$PREBOOT_UUID" --hash-every 0 >/dev/null 2>&1 || fail "our reader walks Preboot"
for m in mnt mnt-src mnt-boot; do
    diskutil unmount "$work/$m" >/dev/null
done
detach_all

if "$read_compare" "$reader" "$mount_list" "$out" "$DATA_UUID" 1 "$work/read-compare" >"$work/read-compare.txt"; then
    summary=$(grep -E '^(walk|hashed|identical)' "$work/read-compare.txt" | tr '\n' ';')
    say "our reader's walk of Data equals the kernel's: $summary"
else
    cat "$work/read-compare.txt"
    fail "our reader's walk of Data differs from the kernel's"
fi

awk -F'\t' -v OFS='\t' 'NR == 2 { $4 = "4242:4242" } { print }' "$work/data.tsv" >"$work/data-wrong.tsv"
if /usr/bin/python3 -I "$compare" "$work/source.tsv" "$work/data-wrong.tsv" >/dev/null; then
    fail "the comparison passed a changed owner"
else
    say "control: a changed owner fails the comparison"
fi

say "== read-write"
cp -c "$out" "$work/rw.img"
if attach "$work/rw.img"; then
    data=$(volume_slice "$DATA_UUID")
    if diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt" "$data" >/dev/null; then
        sudo -n /bin/cp -c "$work/mnt/cl/y" "$work/mnt/cl/w" || fail "clone on the copy"
        sudo -n /bin/sh -c "printf more >>'$work/mnt/cl/x'" || fail "write a clone on the copy"
        sudo -n /bin/rm "$work/mnt/hl/b" || fail "unlink a hard link on the copy"
        sudo -n /bin/mkdir "$work/mnt/new" || fail "mkdir on the copy"
        sync
        diskutil unmount "$work/mnt" >/dev/null || fail "unmount read-write"
    else
        fail "mount Data read-write"
    fi
    detach_all
    if fsck_clean "$work/rw.img" "$work/fsck-rw.txt" 4; then
        say "after a read-write mount that cloned, wrote, unlinked and made a directory: fsck clean"
    else
        cat "$work/fsck-rw.txt"
        fail "fsck after a read-write mount"
    fi
else
    fail "attach read-write"
fi
rm -f -- "$work/rw.img"

say "== power cuts"
"$tool" power-cuts --source "$src" --target-bytes "$TARGET_BYTES" --system-uuid "$system_uuid" \
    --data-uuid "$DATA_UUID" --data-name SwitchDataTrial --data-features 0x2 --data-rocompat 0x0c \
    --data-incompat 0x200 --preboot-uuid "$PREBOOT_UUID" --preboot-name SwitchPrebootTrial --preboot-features 0x2 \
    --preboot-rocompat 0 --preboot-incompat 0 --unicode 17.0 --time-ns "$TIME_NS" \
    --preboot-file "$IMAGE:$((4 << 20)):$IMAGE_SEED" --seed 2611 --per-phase 4 --save "$work/cuts" \
    --prior-areas 8192:27748 >"$work/cuts.txt" || fail "the power-cut suite"
grep -v '^ok:' "$work/cuts.txt"
grep -q 'cleared by only the invalidation of its descriptor area' "$work/cuts.txt" \
    || fail "the finished starting state doesn't test phase B's zeroing of an older descriptor area"
ok=$(grep -c '^ok:' "$work/cuts.txt")
say "$ok cuts ok"
for image in "$work"/cuts/*.img; do
    name=$(basename "$image" .img)
    attach "$image" -readonly || {
        say "$name: attach refused"
        continue
    }
    say "$name: ${#vols[@]} volumes attach"
    if [[ $name == complete ]]; then
        [[ ${#vols[@]} == 3 ]] || fail "the complete image attaches ${#vols[@]} volumes"
    else
        [[ ${#vols[@]} == 0 ]] || fail "$name attaches ${#vols[@]} volumes before block 0"
    fi
    detach_all
    rm -f -- "$image"
done

say "== negative controls"
cp -c "$out" "$work/neg-leaf.img"
"$tool" mutate "$work/neg-leaf.img" data-leaf --uuid "$DATA_UUID" >/dev/null || die "mutate data-leaf"
if fsck_clean "$work/neg-leaf.img" "$work/fsck-neg-leaf.txt" 4; then
    fail "fsck_apfs passed a wrong child count"
else
    first=$(grep -iE '^(error|warning)' "$work/fsck-neg-leaf.txt" | grep -v 'disk name' | head -1)
    say "wrong child count: fsck_apfs: $first"
fi
cp -c "$out" "$work/neg-queue.img"
"$tool" mutate "$work/neg-queue.img" queue-limit >/dev/null || die "mutate queue-limit"
attach "$work/neg-queue.img" -readonly
say "main queue limit off by one: ${#vols[@]} volumes attach"
[[ ${#vols[@]} == 0 ]] || fail "a wrong queue limit still attached volumes"
detach_all

say "failures: $failures"
[[ $failures == 0 ]]
