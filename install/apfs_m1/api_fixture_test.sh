#!/bin/bash
# What W5–W8 will make through the writer's API, beside a copy of /private/var, checked through macOS:
# - whole-file clones, a hard link, a streamed xattr and a symlink, and new content for copied files (a plain file, a
#   compressed clone, a whole-file clone) pass fsck_apfs as built, and again after macOS writes to them read-write;
#   after a remount, a write to one clone hasn't reached the other;
# - macOS's own clones of our copied compressed clones carry the clone-mapping size ours do, and deleting one of them
#   or writing to another leaves the rest reading their own bytes;
# - what the writer must refuse fails the build: two xattrs naming one stream, one name twice up to normalisation, and
#   a fill that writes less than its extent;
# - a finished container whose block 0 lags behind its newest checkpoint leaves no checkpoint of its own after any
#   power cut and rerun, and every state the cut suite keeps attaches no volume.
#
# Needs macOS with hdiutil, newfs_apfs, fsck_apfs and passwordless sudo.
set -o pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
tool=$PWD/$1
reader=$PWD/$2
work=$(mktemp -d /tmp/apfs_api.XXXXXX)
DATA_UUID=D1B1D053-D0A6-496C-8625-A947A3DA863E
PREBOOT_UUID=4AA4A761-5138-439D-9A30-D4138F6887FA
TIME_NS=1795000000000000000
failures=0
vols=()
disk=
# shellcheck source=/dev/null
source "$(dirname "$0")/images.sh"

cleanup() {
    diskutil unmount force "$work/mnt" >/dev/null 2>&1
    detach_all
    if [[ $work == /tmp/apfs_api.* && -d $work ]]; then
        sudo -n rm -rf -- "$work"
    fi
}
trap cleanup EXIT

build() {
    "$tool" build --source "$src" --target "$1" --target-bytes $((4 << 30)) --system-uuid "$system_uuid" \
        --data-uuid "$DATA_UUID" --data-name Switch-iOS-Data --data-features 0x2 --data-rocompat 0x0c \
        --data-incompat 0x200 --preboot-uuid "$PREBOOT_UUID" --preboot-name Switch-iOS-Preboot \
        --preboot-features 0x2 --preboot-rocompat 0 --preboot-incompat 0 --unicode 17.0 --time-ns "$TIME_NS" \
        --preboot-file "img:$((4 << 20)):7" "${@:2}"
}

# mount_data <image> rw|ro
mount_data() {
    local slice
    if [[ $2 == ro ]]; then
        attach "$1" -readonly || return 1
        slice=$(volume_slice "$DATA_UUID") || return 1
        diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt" "$slice" >/dev/null
    else
        attach "$1" || return 1
        slice=$(volume_slice "$DATA_UUID") || return 1
        diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt" "$slice" >/dev/null
    fi
}

unmount_data() {
    diskutil unmount "$work/mnt" >/dev/null
    detach_all
}

# Every private id's clone mappings, as "private_id count physical_size"; fails if one id has two sizes.
mapping_sizes() {
    "$reader" records "$1" --volume "$DATA_UUID" | awk '
        $1 == "other" && $3 == 14 {
            id = substr($4, 25, 16)
            if (id in size && size[id] != $5) { mixed = 1 }
            size[id] = $5
            n[id]++
        }
        END {
            for (id in n) { print id, n[id], size[id] }
            exit mixed
        }'
}

refused() {
    local name=$1 want=$2 out=$work/$1.img last
    if build "$out" --sample "$name" >"$work/$name.txt" 2>&1; then
        fail "$name: the build was accepted"
        fsck_apfs -n "$out" 2>&1 | grep -iE '^(warning|error)|corrupt|appears to be OK' | head -6
        return
    fi
    last=$(tail -1 "$work/$name.txt")
    if grep -qi "$want" "$work/$name.txt"; then
        say "$name: refused: $last"
    else
        fail "$name: failed without naming $want: $last"
    fi
    rm -f -- "$out"
}

# Four bytes at an offset, in hex.
show() {
    dd if="$1" bs=1 skip="$2" count=4 2>/dev/null | xxd -p
}

host=$(hostname)
macos=$(sw_vers -productVersion)
uid=$(id -u)
gid=$(id -g)
say "host=$host macos=$macos work=$work"
sudo -n true || die "passwordless sudo is required"
mkdir -p "$work/mnt" "$work/cuts"
src=$work/source.img
truncate -s 512m "$src"
attach "$src" || die "attach the blank source"
newfs_apfs -e -v System "/dev/$disk" >/dev/null || die "newfs_apfs"
detach_all
attach "$src" || die "attach the source"
diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt" "${vols[0]}" >/dev/null || die "mount the source"
v=$work/mnt/private/var
sudo -n mkdir -p "$v/d" "$v/c" || die "make /private/var"
sudo -n chown -R "$uid:$gid" "$work/mnt/private" || die "own /private/var"
printf 'plain\n' >"$v/d/plain"
head -c 100000 /dev/urandom >"$work/orig"
cp "$work/orig" "$v/d/orig"
cp -c "$v/d/orig" "$v/d/clone" || die "clone"
cp /usr/share/dict/web2 "$work/web2"
ditto --hfsCompression "$work/web2" "$v/c/words" || die "compress"
cp -c "$v/c/words" "$v/c/words-clone" || die "clone a compressed file"
yes abcdefgh | head -c 20000 >"$work/small"
ditto --hfsCompression "$work/small" "$v/c/small" || die "compress"
cp -c "$v/c/small" "$v/c/small-clone" || die "clone a compressed file"
diskutil unmount "$work/mnt" >/dev/null || die "unmount the source"
detach_all
system_uuid=$("$tool" inspect "$src" | sed -n 's/^volume 0 .* uuid \([0-9A-F-]*\) .*/\1/p')
[[ -n $system_uuid ]] || die "inspect the source"
"$tool" set-role "$src" --uuid "$system_uuid" --role 1 >/dev/null || die "set-role"

say "== clones, a hard link, a streamed xattr and a symlink"
out=$work/clones.img
build "$out" --sample clones | grep -E 'clone mappings' || fail "build with API clones"
if fsck_clean "$out" "$work/fsck-clones.txt" 4; then
    say "as built: fsck_apfs 4 OK, no warnings"
else
    grep -iE '^(warning|error)|appears to be OK' "$work/fsck-clones.txt" | sort | uniq -c | head -8
    fail "fsck_apfs of the built image"
fi
mapping_sizes "$out" >"$work/mappings-built.txt" || fail "a private id with two clone-mapping sizes as built"
sizes=$(tr '\n' ';' <"$work/mappings-built.txt")
say "clone mappings as built (private id, files, size): $sizes"
if mount_data "$out" rw; then
    m=$work/mnt
    sudo -n /bin/sh -c "printf ZZZZ | dd of='$m/api/y' bs=1 seek=100 conv=notrunc 2>/dev/null" || fail "write to y"
    x=$(show "$m/api/x" 100)
    y=$(show "$m/api/y" 100)
    say "while mounted, after writing y: x at 100 $x, y at 100 $y"
    sudo -n /bin/rm "$m/api/a1" || fail "rm a1"
    sudo -n /bin/sh -c "printf more >>'$m/api/b2'" || fail "append to b2"
    sudo -n /bin/rm "$m/api/h1" || fail "unlink a hard link"
    sudo -n /bin/cp -c "$m/c/words" "$m/c/words-3" || fail "macOS clones a copied compressed clone"
    sudo -n /bin/cp -c "$m/c/small" "$m/c/small-3" || fail "macOS clones a copied compressed clone"
    sudo -n /bin/rm "$m/c/words-clone" || fail "rm a copied compressed clone"
    sudo -n /bin/sh -c "printf more >>'$m/c/small-clone'" || fail "append to a copied compressed clone"
    sudo -n /bin/mkdir "$m/new" || fail "mkdir"
    sync
    unmount_data
else
    fail "mount Data read-write"
    detach_all
fi
if fsck_clean "$out" "$work/fsck-clones-rw.txt" 4; then
    say "after macOS wrote: fsck_apfs 4 OK, no warnings"
else
    grep -iE '^(warning|error)|appears to be OK' "$work/fsck-clones-rw.txt" | sort | uniq -c | head -8
    fail "fsck_apfs after macOS wrote"
fi
mapping_sizes "$out" >"$work/mappings-rw.txt" || fail "macOS's clone mappings differ in size from ours"
sizes=$(tr '\n' ';' <"$work/mappings-rw.txt")
say "clone mappings after macOS cloned (private id, files, size): $sizes"
if mount_data "$out" ro; then
    m=$work/mnt
    x=$(show "$m/api/x" 100)
    y=$(show "$m/api/y" 100)
    a2=$(show "$m/api/a2" 0)
    a2_size=$(stat -f %z "$m/api/a2")
    b1=$(show "$m/api/b1" 0)
    b1_size=$(stat -f %z "$m/api/b1")
    b2_size=$(stat -f %z "$m/api/b2")
    h2=$(cat "$m/api/h2")
    got="x $x y $y a2 $a2 $a2_size b1 $b1 $b1_size b2 $b2_size h2 $h2"
    say "after a remount: $got"
    [[ $got == "x 64656667 y 5a5a5a5a a2 01010101 4096 b1 02020202 4096 b2 4100 h2 hard" ]] \
        || fail "the clones after a remount"
    small_clone=$(tail -c 4 "$m/c/small-clone")
    if cmp -s "$m/c/words" "$work/web2" && cmp -s "$m/c/words-3" "$work/web2" && cmp -s "$m/c/small" "$work/small" \
        && cmp -s "$m/c/small-3" "$work/small" && [[ ! -e $m/c/words-clone && $small_clone == more ]]; then
        say "copied compressed clones after rm, append and clone: the others read their own bytes"
    else
        fail "copied compressed clones after macOS wrote"
    fi
    unmount_data
else
    fail "mount Data read-only"
    detach_all
fi
rm -f -- "$out"

say "== new content for copied files"
out=$work/replace.img
if build "$out" --sample replace >"$work/replace.txt" 2>&1; then
    if fsck_clean "$out" "$work/fsck-replace.txt" 4; then
        say "as built: fsck_apfs 4 OK, no warnings"
    else
        grep -iE '^(warning|error)|appears to be OK' "$work/fsck-replace.txt" | head -6
        fail "fsck_apfs of the replaced content"
    fi
    if mount_data "$out" rw; then
        m=$work/mnt
        words=$(yes 'replaced c/words' | head -n 2000 | shasum -a 256 | cut -c1-16)
        plain=$(cat "$m/d/plain")
        sum=$(shasum -a 256 <"$m/c/words" | cut -c1-16)
        flags=$(stat -f %Sf "$m/c/words")
        got="$plain $sum $flags"
        if cmp -s "$m/c/words-clone" "$work/web2"; then
            got+=" words-clone-intact"
        fi
        if cmp -s "$m/d/clone" "$work/orig"; then
            got+=" clone-intact"
        fi
        say "replaced: $got"
        [[ $got == "replaced d/plain $words - words-clone-intact clone-intact" ]] || fail "the replaced files"
        sudo -n /bin/sh -c "printf ZZZZ | dd of='$m/d/clone' bs=1 seek=10 conv=notrunc 2>/dev/null" || fail "write"
        sudo -n /bin/sh -c "printf more >>'$m/c/words'" || fail "append"
        sync
        unmount_data
    else
        fail "mount Data read-write"
        detach_all
    fi
    if fsck_clean "$out" "$work/fsck-replace-rw.txt" 4; then
        say "after macOS wrote: fsck_apfs 4 OK, no warnings"
    else
        grep -iE '^(warning|error)|appears to be OK' "$work/fsck-replace-rw.txt" | head -6
        fail "fsck_apfs of the replaced content after macOS wrote"
    fi
    if mount_data "$out" ro; then
        orig=$(head -c 3 "$work/mnt/d/orig")
        clone=$(show "$work/mnt/d/clone" 10)
        tail=$(tail -c 4 "$work/mnt/c/words")
        got="$orig $clone $tail"
        say "after a remount: $got"
        [[ $got == "rep 5a5a5a5a more" ]] || fail "the replaced files after a remount"
        unmount_data
    else
        fail "mount Data read-only"
        detach_all
    fi
else
    last=$(tail -1 "$work/replace.txt")
    fail "the build with replaced content: $last"
fi
rm -f -- "$out"

say "== refusals"
refused shared-xattr "xattr"
refused nfd-twin "already holds"
refused short-fill "fill"

say "== a finished container whose block 0 lags"
"$tool" power-cuts --source "$src" --target-bytes $((4 << 30)) --system-uuid "$system_uuid" \
    --data-uuid "$DATA_UUID" --data-name Switch-iOS-Data --data-features 0x2 --data-rocompat 0x0c \
    --data-incompat 0x200 --preboot-uuid "$PREBOOT_UUID" --preboot-name Switch-iOS-Preboot --preboot-features 0x2 \
    --preboot-rocompat 0 --preboot-incompat 0 --unicode 17.0 --time-ns "$TIME_NS" \
    --preboot-file "img:$((4 << 20)):7" --seed 2611 --per-phase 2 --save "$work/cuts" \
    --prior-areas 8192:27748 --prior-lag >"$work/cuts.txt" || fail "the power-cut suite"
grep -v '^ok:' "$work/cuts.txt" | cut -c1-300
grep -q 'cleared by only the invalidation of its descriptor area' "$work/cuts.txt" \
    || fail "the live checkpoint is cleared by something other than phase B"
for image in "$work"/cuts/*.img; do
    name=$(basename "$image" .img)
    if [[ $name == complete ]] && ! fsck_clean "$image" "$work/fsck-complete.txt" 4; then
        fail "fsck_apfs of the complete image"
    fi
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

say "failures: $failures"
[[ $failures == 0 ]]
