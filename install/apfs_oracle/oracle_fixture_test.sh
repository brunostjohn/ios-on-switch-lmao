#!/bin/bash
# Full Darwin remote APFS oracle test (DESIGN.md Section 6): builds synthetic OS + sealed cryptex fixtures,
# runs the full 3-volume pipeline (W4+W5+W6+W7a+W8 SealFill + FileFill), runs all 10 verification checks,
# and runs all 5 negative controls. Needs macOS with hdiutil, newfs_apfs, fsck_apfs, apfs_prepare_cryptex,
# apfs_checkseal, and passwordless sudo.
set -o pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
R=/System/Library/Filesystems/apfs.fs/Contents/Resources

tool=$PWD/$1
reader=$PWD/$2
apfs_tool=$PWD/$3
tree_diff=$PWD/$4
helper=$PWD/$5

work=$(mktemp -d /tmp/switch_apfs_oracle.XXXXXX)
DATA_UUID=D1B1D053-D0A6-496C-8625-A947A3DA863E
PREBOOT_UUID=4AA4A761-5138-439D-9A30-D4138F6887FA
CRYPTEX_UUID=D8B9F68E-F191-4FCD-9F89-D88A322E9B4D
TIME_NS=1795000000000000000
TARGET_BYTES=$((2 << 30))
VERBATIM_BYTES=$((4 << 20))

failures=0
vols=()
disk=

# shellcheck source=/dev/null
source "$(dirname "$0")/../apfs_m1/images.sh"

cleanup() {
    local m
    for m in mnt-sys mnt-data mnt-boot mnt-cryptex; do
        diskutil unmount force "$work/$m" >/dev/null 2>&1
    done
    detach_all
    if [[ $work == /tmp/switch_apfs_oracle.* && -d $work ]]; then
        sudo -n rm -rf -- "$work"
    fi
}
trap cleanup EXIT

run_compose() {
    local out_img=$1
    shift
    "$tool" compose \
        --source "$src" \
        --target "$out_img" \
        --target-bytes "$TARGET_BYTES" \
        --system-uuid "$system_uuid" \
        --data-uuid "$DATA_UUID" \
        --data-name Switch-iOS-Data \
        --preboot-uuid "$PREBOOT_UUID" \
        --preboot-name Switch-iOS-Preboot \
        --unicode 17.0 \
        --time-ns "$TIME_NS" \
        --delta-dir "$work/delta" \
        --overlay-dir "$work/overlay" \
        --bootstrap-dir "$work/bootstrap" \
        --build 24B119 \
        --firmware 27.2 \
        --shader-seed-dir "$work/seed" \
        --sealed-cryptex "name=rebuilt-27.dmg,source=$cryptex_src,uuid=$CRYPTEX_UUID,manifest=$work/cryptex-delta/cryptex.manifest.json,vol_name=OracleCryptex" \
        --verbatim-cryptex "name=verbatim-17.dmg,path=$verbatim_img,sha256=$verbatim_sha" \
        "$@"
}

host=$(hostname)
macos=$(sw_vers -productVersion)
uid=$(id -u)
gid=$(id -g)
say "host=$host macos=$macos work=$work"
sudo -n true || die "passwordless sudo is required"
mkdir -p "$work/mnt-sys" "$work/mnt-data" "$work/mnt-boot" "$work/mnt-cryptex" \
    "$work/delta" "$work/overlay" "$work/bootstrap" "$work/seed" "$work/cryptex-delta" \
    "$work/extracted" "$work/cuts"

cp /usr/share/dict/web2 "$work/web2" || die "copy web2"
chmod 644 "$work/web2" || die "chmod web2"

say "== build synthetic OS source container"
src=$work/source.img
truncate -s 256m "$src"
attach "$src" || die "attach blank source"
newfs_apfs -e -v System "/dev/$disk" >/dev/null || die "newfs_apfs System"
detach_all

attach "$src" || die "attach source"
diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt-sys" "${vols[0]}" >/dev/null || die "mount source"

mp=$work/mnt-sys
v=$mp/private/var
sudo -n mkdir -p "$v/mobile/Library/Preferences" "$v/db" "$v/staged_system_apps/AppleApp.app" \
    "$mp/usr/libexec" "$mp/usr/sbin" || die "mkdir source"
sudo -n chown -R "$uid:$gid" "$mp/private" "$mp/usr" || die "chown source"

printf 'prefs-seed\n' >"$v/mobile/Library/Preferences/com.example.plist"
printf 'original plain var file\n' >"$v/db/plain"
yes 'compressible var payload line' | head -c 32768 >"$work/comp_src"
ditto --hfsCompression "$work/comp_src" "$v/db/compressed" || die "compress var file"
ditto --hfsCompression "$work/comp_src" "$v/db/keep_compressed" || die "compress kept var file"
printf 'original hardlinked var file\n' >"$v/db/hl1"
ln "$v/db/hl1" "$v/db/hl2"
printf 'kept hardlinked var file\n' >"$v/db/kept_hl1"
ln "$v/db/kept_hl1" "$v/db/kept_hl2"
printf 'setuid binary payload\n' >"$v/db/setuid_bin"
printf 'xattr carrier\n' >"$v/db/with_xattrs"
xattr -w com.example.small smallval "$v/db/with_xattrs" || die "small xattr"
big_xattr=$(head -c 6000 /dev/urandom | base64)
xattr -w com.example.big "$big_xattr" "$v/db/with_xattrs" || die "big xattr"
printf 'acl carrier\n' >"$v/db/with_acl"
chmod +a "everyone deny delete" "$v/db/with_acl" || die "chmod +a"
ln -s plain "$v/db/sym"
printf 'original staged app bytes\n' >"$v/staged_system_apps/AppleApp.app/AppleApp"

yes 'compressible system tool' | head -c 24576 >"$work/tool_src"
ditto --hfsCompression "$work/tool_src" "$mp/usr/libexec/tool" || die "compress system tool"
printf 'original system hardlink\n' >"$mp/usr/libexec/linked_a"
ln "$mp/usr/libexec/linked_a" "$mp/usr/sbin/linked_b"

/usr/bin/python3 -I "$helper" prepare-inputs \
    --sys-mount "$mp" \
    --delta-dir "$work/delta" \
    --overlay-dir "$work/overlay" \
    --bootstrap-dir "$work/bootstrap" \
    --seed-dir "$work/seed" || die "prepare-inputs"

chmod 0644 "$v/db/plain" "$v/db/keep_compressed" "$v/db/kept_hl1" "$v/db/with_xattrs" "$v/db/with_acl" \
    "$v/mobile/Library/Preferences/com.example.plist"
chmod 0755 "$v/db/compressed" "$v/staged_system_apps/AppleApp.app/AppleApp" "$mp/usr/libexec/tool"
chmod 0640 "$v/db/hl1"
chmod 4755 "$v/db/setuid_bin"
chmod 0775 "$mp/usr/libexec/linked_a"

sudo -n chown -R 501:501 "$v/mobile" "$v/staged_system_apps" || die "chown mobile"
sudo -n chown 501:501 "$v/db/kept_hl1" "$v/db/with_xattrs" || die "chown 501"
sudo -n chown 0:0 "$v/db" "$v/db/plain" "$v/db/compressed" "$v/db/keep_compressed" "$v/db/setuid_bin" \
    "$v/db/with_acl" "$mp/usr" "$mp/usr/libexec" "$mp/usr/sbin" "$mp/usr/libexec/tool" || die "chown root"
sudo -n chown 64:64 "$v/db/hl1" || die "chown 64"
sudo -n chown 0:80 "$mp/usr/libexec/linked_a" || die "chown admin"
sudo -n chflags hidden "$v/db/compressed" "$mp/usr/libexec/tool" || die "chflags hidden"

sudo -n /usr/bin/python3 -I "$tree_diff" dump "$mp" --out "$work/source-sys.json" || die "dump source System"
diskutil unmount "$mp" >/dev/null || die "unmount source"
detach_all

system_uuid=$("$tool" inspect "$src" | sed -n 's/^volume 0 .* uuid \([0-9A-F-]*\) .*/\1/p')
[[ -n $system_uuid ]] || die "inspect source"
"$tool" set-role "$src" --uuid "$system_uuid" --role 1 >/dev/null || die "set-role System"
fsck_clean "$src" "$work/fsck-src.txt" 2 || {
    cat "$work/fsck-src.txt"
    die "source fsck"
}

say "== build synthetic sealed source cryptex + verbatim cryptex"
cryptex_src=$work/cryptex-27.img
truncate -s 64m "$cryptex_src"
attach "$cryptex_src" || die "attach blank cryptex"
newfs_apfs -e -v OracleCryptex "/dev/$disk" >/dev/null || die "newfs_apfs cryptex"
detach_all

attach "$cryptex_src" || die "attach cryptex"
diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt-cryptex" "${vols[0]}" >/dev/null \
    || die "mount cryptex"
cm=$work/mnt-cryptex
sudo -n mkdir -p "$cm/bin" "$cm/c" "$cm/hl" "$cm/x" || die "mkdir cryptex"
sudo -n chown -R "$uid:$gid" "$cm/bin" "$cm/c" "$cm/hl" "$cm/x" || die "chown cryptex"
printf 'original cryptex app\n' >"$cm/bin/app"
ditto --hfsCompression "$work/web2" "$cm/c/words-kept" || die "compress cryptex kept"
ditto --hfsCompression "$work/web2" "$cm/c/words-rep" || die "compress cryptex rep"
printf 'kept cryptex hardlink\n' >"$cm/hl/kept1"
ln "$cm/hl/kept1" "$cm/hl/kept2"
ln -s ../bin/app "$cm/x/sym"
printf 'xattr file\n' >"$cm/x/with_xattr"
xattr -w com.example.stream "$big_xattr" "$cm/x/with_xattr" || die "cryptex stream xattr"
sudo -n chown 0:0 "$cm/bin/app" "$cm/c/words-rep" || die "chown cryptex root"
sudo -n chown 501:20 "$cm/hl/kept1" "$cm/x/with_xattr" || die "chown cryptex 501:20"
sudo -n chmod 4755 "$cm/bin/app" || die "chmod cryptex setuid"
sudo -n chflags hidden "$cm/hl/kept1" || die "chflags cryptex hidden"

/usr/bin/python3 -I "$helper" prepare-cryptex-delta \
    --cryptex-mount "$cm" \
    --delta-dir "$work/cryptex-delta" || die "prepare-cryptex-delta"

diskutil unmount "$cm" >/dev/null || die "unmount cryptex"
"$R/apfs_prepare_cryptex" -p "/dev/${vols[0]}" >"$work/src-seal.log" 2>&1 || {
    cat "$work/src-seal.log"
    die "apfs_prepare_cryptex"
}
"$R/apfs_checkseal" "/dev/${vols[0]}" >/dev/null 2>&1 || die "apfs_checkseal source cryptex"
detach_all

verbatim_img=$work/verbatim-17.dmg
head -c "$VERBATIM_BYTES" /dev/urandom >"$verbatim_img"
verbatim_sha=$(shasum -a 256 <"$verbatim_img" | awk '{print $1}')

say "== check 9: build twice and assert byte-identical"
out1=$work/out1.img
out2=$work/out2.img
run_compose "$out1" \
    --manifest-out "$work/returned-manifest.json" \
    --expected-data-dump "$work/expected-data.json" \
    --expected-preboot-dump "$work/expected-preboot.json" || die "compose 1"
run_compose "$out2" >/dev/null || die "compose 2"
if cmp -s "$out1" "$out2"; then
    say "check 9 PASS: two full 3-volume builds are byte-identical"
else
    fail "check 9: two builds differ"
fi
rm -f -- "$out2"

say "== check 8: switch_apfs_read_tool audit + exact fs_alloc_count"
if "$reader" audit "$out1" >"$work/audit.txt"; then
    audit_hdr=$(head -4 "$work/audit.txt" | tr '\n' ';')
    say "audit: $audit_hdr"
    if awk '/^volume [12] / && $5 != $7 + 1 { bad = 1 } /^blocks / && ($4 != $6 || $8 != 0) { bad = 1 } END { exit bad }' "$work/audit.txt"; then
        say "check 8 PASS: 0 problems, used==owned, exact fs_alloc_count"
    else
        cat "$work/audit.txt"
        fail "check 8: fs_alloc_count or used/owned mismatch"
    fi
else
    cat "$work/audit.txt"
    fail "check 8: switch_apfs_read_tool audit failed"
fi

say "== check 7: Preboot cryptexes are 1 contiguous uncompressed extent"
if "$tool" verify-preboot-extents \
    --image "$out1" \
    --preboot-uuid "$PREBOOT_UUID" \
    --extract-dir "$work/extracted" \
    --expect-count 2 >"$work/extents.txt"; then
    cat "$work/extents.txt"
    say "check 7 PASS: both Preboot cryptexes are 1 contiguous uncompressed extent"
else
    cat "$work/extents.txt"
    fail "check 7: verify-preboot-extents failed"
fi

say "== check 6a: offline .index and .originals verification"
if "$tool" verify-index \
    --image "$out1" \
    --system-uuid "$system_uuid" \
    --data-uuid "$DATA_UUID" \
    --expect-originals \
    --dump-tsv "$work/index.tsv"; then
    say "check 6a PASS: offline .index + .originals verified"
else
    fail "check 6a: verify-index failed"
fi

say "== check 4: apfs_tool spaceman_ok + geometry equality"
pins_geom=$("$tool" verify-pins \
    --image "$out1" \
    --system-uuid "$system_uuid" \
    --data-uuid "$DATA_UUID" \
    --preboot-uuid "$PREBOOT_UUID" \
    --expected-blocks "$((TARGET_BYTES / 4096))") || fail "check 4: verify-pins failed"
apfs_out=$("$apfs_tool" "$out1" 0 2>&1) || fail "check 4: apfs_tool failed: $apfs_out"
say "verify-pins: $pins_geom"
say "apfs_tool:   $apfs_out"
read -r _ p_blocks p_pool_base p_pool_blocks _ <<<"$pins_geom"
read -r a_blocks _ a_pool_base a_pool_blocks <<<"$apfs_out"
if [[ $p_blocks == "$a_blocks" && $p_pool_base == "$a_pool_base" && $p_pool_blocks == "$a_pool_blocks" ]]; then
    say "check 4 PASS: apfs_tool spaceman_ok and geometry matches ($a_blocks $a_pool_base $a_pool_blocks)"
else
    fail "check 4: geometry mismatch pins=($p_blocks,$p_pool_base,$p_pool_blocks) apfs_tool=($a_blocks,$a_pool_base,$a_pool_blocks)"
fi

say "== check 2: fsck_apfs -n on 3-volume container"
if fsck_clean "$out1" "$work/fsck-out.txt" 4; then
    say "check 2 PASS: fsck_apfs -n 4 OK, 0 warnings"
else
    cat "$work/fsck-out.txt"
    fail "check 2: fsck_apfs -n failed"
fi

say "== check 3: extracted Preboot cryptexes (fsck_apfs + apfs_checkseal + apfs_tool + owners mount)"
rebuilt_cryptex=$work/extracted/rebuilt-27.dmg
extracted_verbatim=$work/extracted/verbatim-17.dmg
got_verb_sha=$(shasum -a 256 <"$extracted_verbatim" | awk '{print $1}')
[[ $got_verb_sha == "$verbatim_sha" ]] || fail "check 3: verbatim-17.dmg SHA-256 mismatch"
"$apfs_tool" "$rebuilt_cryptex" 0 >/dev/null || fail "check 3: apfs_tool failed on rebuilt cryptex"
if ! fsck_clean "$rebuilt_cryptex" "$work/fsck-cryptex.txt" 2; then
    cat "$work/fsck-cryptex.txt"
    fail "check 3: fsck_apfs failed on rebuilt cryptex"
fi
attach "$rebuilt_cryptex" -readonly || die "attach rebuilt cryptex"
[[ ${#vols[@]} == 1 ]] || fail "check 3: expected 1 cryptex volume, got ${#vols[@]}"
"$R/apfs_checkseal" "/dev/${vols[0]}" >"$work/checkseal.txt" 2>&1 || {
    cat "$work/checkseal.txt"
    fail "check 3: apfs_checkseal failed on rebuilt cryptex"
}
diskutil info -plist "${vols[0]}" >"$work/cryptex-info.plist" || fail "check 3: diskutil info cryptex"
/usr/bin/python3 -I "$helper" check-volume-plist \
    --plist "$work/cryptex-info.plist" \
    --uuid "$CRYPTEX_UUID" \
    --name OracleCryptex \
    --require-sealed || fail "check 3: cryptex plist mismatch"
diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt-cryptex" "${vols[0]}" >/dev/null \
    || fail "check 3: mount rebuilt cryptex"
sudo -n /usr/bin/python3 -I "$helper" verify-cryptex-mount \
    --mount "$work/mnt-cryptex" \
    --delta-dir "$work/cryptex-delta" \
    --words-file "$work/web2" || fail "check 3: mounted cryptex verification failed"
diskutil unmount "$work/mnt-cryptex" >/dev/null
detach_all
say "check 3 PASS: rebuilt cryptex fsck + apfs_checkseal + mount + verbatim SHA-256 OK"

say "== checks 1, 5, 6b: attach 3 volumes, verify UUIDs/names, deep tree_diff, mounted .index inodes"
attach "$out1" -readonly || die "attach output container"
if [[ ${#vols[@]} != 3 ]]; then
    fail "check 1: expected 3 volumes, got ${#vols[@]} (${vols[*]})"
fi
sys_slice=$(volume_slice "$system_uuid") || fail "check 1: System volume UUID not found"
data_slice=$(volume_slice "$DATA_UUID") || fail "check 1: Data volume UUID not found"
boot_slice=$(volume_slice "$PREBOOT_UUID") || fail "check 1: Preboot volume UUID not found"

diskutil info -plist "$sys_slice" >"$work/sys-info.plist"
diskutil info -plist "$data_slice" >"$work/data-info.plist"
diskutil info -plist "$boot_slice" >"$work/boot-info.plist"
/usr/bin/python3 -I "$helper" check-volume-plist --plist "$work/sys-info.plist" --uuid "$system_uuid" --name System \
    || fail "check 1: System plist"
/usr/bin/python3 -I "$helper" check-volume-plist --plist "$work/data-info.plist" --uuid "$DATA_UUID" \
    --name Switch-iOS-Data || fail "check 1: Data plist"
/usr/bin/python3 -I "$helper" check-volume-plist --plist "$work/boot-info.plist" --uuid "$PREBOOT_UUID" \
    --name Switch-iOS-Preboot || fail "check 1: Preboot plist"
say "check 1 PASS: all 3 volumes attached with exact pinned UUIDs and names"

diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt-sys" "$sys_slice" >/dev/null \
    || die "mount System"
diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt-data" "$data_slice" >/dev/null \
    || die "mount Data"
diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt-boot" "$boot_slice" >/dev/null \
    || die "mount Preboot"

sudo -n /usr/bin/python3 -I "$tree_diff" dump "$work/mnt-sys" --out "$work/actual-sys.json" || fail "dump System"
sudo -n /usr/bin/python3 -I "$tree_diff" dump "$work/mnt-data" --out "$work/actual-data.json" || fail "dump Data"
sudo -n /usr/bin/python3 -I "$tree_diff" dump "$work/mnt-boot" --out "$work/actual-boot.json" || fail "dump Preboot"

/usr/bin/python3 -I "$tree_diff" diff "$work/source-sys.json" "$work/actual-sys.json" --compare-mtime \
    || fail "check 5: System differs from source"
/usr/bin/python3 -I "$tree_diff" diff "$work/expected-data.json" "$work/actual-data.json" --compare-mtime \
    || fail "check 5: Switch-iOS-Data differs from plan"
/usr/bin/python3 -I "$tree_diff" diff "$work/expected-preboot.json" "$work/actual-boot.json" --compare-mtime \
    || fail "check 5: Switch-iOS-Preboot differs from plan"
say "check 5 PASS: tree_diff verified System, Switch-iOS-Data, and Switch-iOS-Preboot"

sudo -n /usr/bin/python3 -I "$helper" verify-mounted-index \
    --sys-mount "$work/mnt-sys" \
    --data-mount "$work/mnt-data" \
    --index-tsv "$work/index.tsv" || fail "check 6b: mounted .index verification failed"
say "check 6b PASS: mounted os.lstat().st_ino matched .index and .originals"

for m in mnt-sys mnt-data mnt-boot; do
    diskutil unmount "$work/$m" >/dev/null
done
detach_all

say "== check 10: interruption across phase boundaries (B, C, D, E)"
"$tool" power-cuts \
    --source "$src" \
    --target-bytes "$TARGET_BYTES" \
    --system-uuid "$system_uuid" \
    --data-uuid "$DATA_UUID" \
    --preboot-uuid "$PREBOOT_UUID" \
    --unicode 17.0 \
    --time-ns "$TIME_NS" \
    --delta-dir "$work/delta" \
    --overlay-dir "$work/overlay" \
    --bootstrap-dir "$work/bootstrap" \
    --build 24B119 \
    --firmware 27.2 \
    --shader-seed-dir "$work/seed" \
    --sealed-cryptex "name=rebuilt-27.dmg,source=$cryptex_src,uuid=$CRYPTEX_UUID,manifest=$work/cryptex-delta/cryptex.manifest.json,vol_name=OracleCryptex" \
    --verbatim-cryptex "name=verbatim-17.dmg,path=$verbatim_img,sha256=$verbatim_sha" \
    --out-dir "$work/cuts" || fail "check 10: power-cuts tool failed"

for cut_img in "$work"/cuts/*.img; do
    name=$(basename "$cut_img" .img)
    if attach "$cut_img" -readonly; then
        count=${#vols[@]}
        detach_all
    else
        count=0
    fi
    say "cut $name: $count volume(s) attach"
    if [[ $name == cut-phase-E-complete ]]; then
        [[ $count == 3 ]] || fail "check 10: $name attached $count volumes, expected 3"
        fsck_clean "$cut_img" "$work/fsck-cut-complete.txt" 4 || fail "check 10: fsck on complete cut"
    else
        [[ $count == 0 ]] || fail "check 10: $name attached $count volumes before block 0"
    fi
    rm -f -- "$cut_img"
done
say "check 10 PASS: all pre-block-0 cuts attach 0 volumes; phase E complete attaches all 3"

say "== negative controls (all 5)"
# 1. Free-queue limit off-by-one -> 0 volumes attach.
cp -c "$out1" "$work/neg-queue.img"
"$tool" tamper "$work/neg-queue.img" queue-limit || die "tamper queue-limit"
if attach "$work/neg-queue.img" -readonly; then
    q_vols=${#vols[@]}
    detach_all
else
    q_vols=0
fi
[[ $q_vols == 0 ]] || fail "neg 1: queue-limit off-by-one still attached $q_vols volumes"
say "neg 1 PASS: queue-limit off-by-one -> 0 volumes attach"
rm -f -- "$work/neg-queue.img"

# 2. 1 Data fs-tree leaf byte flipped -> fsck_apfs -n fails.
cp -c "$out1" "$work/neg-leaf.img"
"$tool" tamper "$work/neg-leaf.img" data-leaf --data-uuid "$DATA_UUID" || die "tamper data-leaf"
if fsck_clean "$work/neg-leaf.img" "$work/fsck-neg-leaf.txt" 4; then
    fail "neg 2: fsck_apfs passed flipped Data leaf byte"
else
    say "neg 2 PASS: flipped Data leaf byte failed fsck_apfs -n"
fi
rm -f -- "$work/neg-leaf.img"

# 3. 1 data byte of rebuilt Preboot cryptex flipped -> apfs_checkseal fails with rc 65.
cp -c "$out1" "$work/neg-cryptex.img"
"$tool" tamper "$work/neg-cryptex.img" preboot-cryptex-byte \
    --preboot-uuid "$PREBOOT_UUID" \
    --cryptex-name rebuilt-27.dmg \
    --cryptex-path /bin/app || die "tamper preboot-cryptex-byte"
mkdir -p "$work/neg-extracted"
"$tool" verify-preboot-extents \
    --image "$work/neg-cryptex.img" \
    --preboot-uuid "$PREBOOT_UUID" \
    --extract-dir "$work/neg-extracted" >/dev/null || die "extract tampered cryptex"
attach "$work/neg-extracted/rebuilt-27.dmg" -readonly || die "attach tampered cryptex"
"$R/apfs_checkseal" "/dev/${vols[0]}" >"$work/neg-checkseal.txt" 2>&1
seal_rc=$?
detach_all
if [[ $seal_rc == 65 ]]; then
    say "neg 3 PASS: flipped Preboot cryptex data byte -> apfs_checkseal rc 65"
else
    cat "$work/neg-checkseal.txt"
    fail "neg 3: expected apfs_checkseal rc 65, got $seal_rc"
fi
rm -f -- "$work/neg-cryptex.img"

# 4. 1 owner changed in expected tree dump -> tree_diff.py fails.
/usr/bin/python3 -I "$helper" mutate-dump-owner \
    --src "$work/expected-data.json" \
    --dst "$work/expected-data-wrong.json" \
    --path /db/plain || die "mutate-dump-owner"
if /usr/bin/python3 -I "$tree_diff" diff "$work/expected-data-wrong.json" "$work/actual-data.json" >/dev/null; then
    fail "neg 4: tree_diff passed changed owner"
else
    say "neg 4 PASS: changed owner failed tree_diff.py"
fi

# 5. Wrong Data UUID -> verify-pins fails and attach slice lookup fails.
cp -c "$out1" "$work/neg-uuid.img"
"$tool" tamper "$work/neg-uuid.img" wrong-data-uuid --data-uuid "$DATA_UUID" || die "tamper wrong-data-uuid"
if "$tool" verify-pins \
    --image "$work/neg-uuid.img" \
    --system-uuid "$system_uuid" \
    --data-uuid "$DATA_UUID" \
    --preboot-uuid "$PREBOOT_UUID" >/dev/null 2>&1; then
    fail "neg 5: verify-pins accepted wrong Data UUID"
else
    say "neg 5 PASS: wrong Data UUID refused by verify-pins"
fi
attach "$work/neg-uuid.img" -readonly || die "attach neg-uuid"
if volume_slice "$DATA_UUID" >/dev/null 2>&1; then
    fail "neg 5: pinned Data UUID still matched after wrong-data-uuid tamper"
fi
detach_all
rm -f -- "$work/neg-uuid.img"

say "failures: $failures"
[[ $failures == 0 ]]
