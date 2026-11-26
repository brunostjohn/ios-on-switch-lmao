#!/bin/bash
# Darwin oracle for switch_apfs_seal: builds a synthetic APFS volume with regular files, kept and replaced hard links,
# symlinks, an empty file, an embedded xattr, a stream xattr, and fork-compressed files, seals it with
# apfs_prepare_cryptex, rebuilds it with switch_apfs_seal_tool applying a delta, and checks the result against
# hdiutil, apfs_checkseal, fsck_apfs, diskutil info -plist, apfs_tool, a read-only owners mount, determinism,
# SealFill, negative controls, and a deep multi-level tree.
set -o pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
R=/System/Library/Filesystems/apfs.fs/Contents/Resources
seal_tool=$PWD/$1
read_tool=$PWD/$2
apfs_tool=$PWD/$3
tree_list=$PWD/$4
work=$(mktemp -d /tmp/apfs_seal.XXXXXX)
PIN_UUID=D8B9F68E-F191-4FCD-9F89-D88A322E9B4D
DEEP_UUID=ADB8FD34-DA7E-4538-9AAF-4F05EBB09057
TIME_NS=1795000000000000000
failures=0
vols=()
disk=

# shellcheck source=/dev/null
source "$(dirname "$0")/../apfs_m1/images.sh"

cleanup() {
    local m
    for m in mnt mnt-rebuilt mnt-deep; do
        diskutil unmount force "$work/$m" >/dev/null 2>&1
    done
    detach_all
    if [[ $work == /tmp/apfs_seal.* && -d $work ]]; then
        sudo -n rm -rf -- "$work"
    fi
}
trap cleanup EXIT

check_sealed_image() {
    local label=$1 image=$2 want_uuid=$3 want_name=$4 audit_summary walk_summary
    say "== check $label ($image)"
    if "$read_tool" audit "$image" >"$work/$label-audit.txt"; then
        audit_summary=$(head -2 "$work/$label-audit.txt" | tr '\n' ';')
        say "audit: $audit_summary"
    else
        cat "$work/$label-audit.txt"
        fail "$label: switch_apfs_read audit"
    fi
    awk '/^volume / && $5 != $7 + 1 { bad = 1 } END { exit bad }' "$work/$label-audit.txt" \
        || fail "$label: owned != fs_alloc_count + 1"
    if "$read_tool" walk "$image" --volume 0 --hash-every 0 --check-file-info >/dev/null 2>"$work/$label-walk.txt"; then
        walk_summary=$(<"$work/$label-walk.txt")
        say "reader file-info: $walk_summary"
    else
        cat "$work/$label-walk.txt"
        fail "$label: reader file-info check"
    fi
    if out=$("$apfs_tool" "$image" 0 2>&1); then
        say "apfs_tool: $out"
    else
        fail "$label: apfs_tool failed: $out"
    fi
    if fsck_clean "$image" "$work/$label-fsck.txt" 2; then
        say "fsck_apfs -n: 2 OK, no warnings"
    else
        cat "$work/$label-fsck.txt"
        fail "$label: fsck_apfs -n"
    fi
    if ! attach "$image" -readonly; then
        fail "$label: hdiutil attach -readonly failed"
        return 1
    fi
    if [[ ${#vols[@]} != 1 ]]; then
        fail "$label: expected 1 volume on attach, got ${#vols[@]} (${vols[*]})"
        detach_all
        return 1
    fi
    local vol=${vols[0]}
    if "$R/apfs_checkseal" "/dev/$vol" >"$work/$label-checkseal.txt" 2>&1; then
        say "apfs_checkseal: OK"
    else
        cat "$work/$label-checkseal.txt"
        fail "$label: apfs_checkseal failed"
    fi
    diskutil info -plist "$vol" >"$work/$label-info.plist" || fail "$label: diskutil info -plist"
    /usr/bin/python3 -I - "$work/$label-info.plist" "$want_uuid" "$want_name" <<'PY' || fail "$label: diskutil plist check"
import plistlib, sys
with open(sys.argv[1], 'rb') as f:
    info = plistlib.load(f)
want_uuid, want_name = sys.argv[2].upper(), sys.argv[3]
assert info.get('Sealed') in (True, 'Yes'), f"Sealed={info.get('Sealed')!r}"
assert info.get('VolumeUUID', '').upper() == want_uuid, f"UUID={info.get('VolumeUUID')!r} != {want_uuid}"
assert info.get('VolumeName') == want_name, f"Name={info.get('VolumeName')!r} != {want_name}"
print(f"diskutil plist: Sealed={info.get('Sealed')} UUID={info['VolumeUUID']} Name={info['VolumeName']}")
PY
    detach_all
}

host=$(hostname)
macos=$(sw_vers -productVersion)
uid=$(id -u)
gid=$(id -g)
say "host=$host macos=$macos work=$work"
sudo -n true || die "passwordless sudo is required"
mkdir -p "$work/mnt" "$work/mnt-rebuilt" "$work/mnt-deep" "$work/delta/files/bin" "$work/delta/files/hl" "$work/delta/files/c"

cp /usr/share/dict/web2 "$work/web2" || die "copy web2"
chmod 644 "$work/web2" || die "chmod web2"

say "== build synthetic sealed source"
src=$work/source.img
truncate -s 96m "$src"
attach "$src" || die "attach blank source"
newfs_apfs -e -v CryptexTest "/dev/$disk" >/dev/null || die "newfs_apfs"
detach_all
attach "$src" || die "attach source"
diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt" "${vols[0]}" >/dev/null || die "mount source"

mp=$work/mnt
sudo -n mkdir -p "$mp/bin" "$mp/hl" "$mp/c" "$mp/x" "$mp/empty-dir" || die "mkdir"
sudo -n chown -R "$uid:$gid" "$mp/bin" "$mp/hl" "$mp/c" "$mp/x" "$mp/empty-dir" || die "chown"

printf 'original regular file\n' >"$mp/bin/app"
head -c 80000 /dev/urandom >"$mp/bin/big"
: >"$mp/bin/empty"
printf 'kept hard link payload\n' >"$mp/hl/kept1"
ln "$mp/hl/kept1" "$mp/hl/kept2"
printf 'replaced hard link payload\n' >"$mp/hl/rep1"
ln "$mp/hl/rep1" "$mp/hl/rep2"
ln -s ../bin/app "$mp/x/sym"
long=$(printf '%0300d' 0 | tr 0 z)
ln -s "$long" "$mp/x/long-sym"
printf 'has xattrs\n' >"$mp/x/with_xattrs"
xattr -w com.example.small dropped_on_seal "$mp/x/with_xattrs"
big_xattr=$(head -c 6000 /dev/urandom | base64)
printf '%s' "$big_xattr" >"$work/big_xattr.txt"
xattr -w com.example.big "$big_xattr" "$mp/x/with_xattrs"
ditto --hfsCompression "$work/web2" "$mp/c/words-kept" || die "compress kept"
ditto --hfsCompression "$work/web2" "$mp/c/words-rep" || die "compress rep"
sudo -n chown 0:0 "$mp/bin/app" "$mp/hl/rep1" "$mp/c/words-rep" || die "chown root"
sudo -n chown 501:20 "$mp/hl/kept1" "$mp/x/with_xattrs" || die "chown 501:20"
sudo -n chmod 4755 "$mp/bin/app" || die "chmod setuid"
sudo -n chflags hidden "$mp/hl/kept1" || die "chflags hidden"

sha_in_app=$(shasum -a 256 <"$mp/bin/app" | awk '{print $1}')
sha_in_rep=$(shasum -a 256 <"$mp/hl/rep1" | awk '{print $1}')
sha_in_words=$(shasum -a 256 <"$work/web2" | awk '{print $1}')
sha_kept_big=$(shasum -a 256 <"$mp/bin/big" | awk '{print $1}')
sha_kept_hl=$(shasum -a 256 <"$mp/hl/kept1" | awk '{print $1}')
sha_kept_xfile=$(shasum -a 256 <"$mp/x/with_xattrs" | awk '{print $1}')
sha_big_xattr=$(shasum -a 256 <"$work/big_xattr.txt" | awk '{print $1}')

diskutil unmount "$mp" >/dev/null || die "unmount source"
"$R/apfs_prepare_cryptex" -p "/dev/${vols[0]}" >"$work/src-seal.log" 2>&1 || {
    cat "$work/src-seal.log"
    die "apfs_prepare_cryptex source"
}
"$R/apfs_checkseal" "/dev/${vols[0]}" >/dev/null 2>&1 || die "apfs_checkseal source"
detach_all

say "== prepare replacement delta manifest"
head -c 70000 /dev/urandom >"$work/delta/files/bin/app"
printf 'translated hardlink bytes over 16KiB: ' >"$work/delta/files/hl/rep1"
head -c 20000 /dev/urandom >>"$work/delta/files/hl/rep1"
cp "$work/delta/files/hl/rep1" "$work/delta/files/hl/rep2"
printf 'uncompressed replacement for words-rep\n' >"$work/delta/files/c/words-rep"

sha_out_app=$(shasum -a 256 <"$work/delta/files/bin/app" | awk '{print $1}')
sha_out_rep=$(shasum -a 256 <"$work/delta/files/hl/rep1" | awk '{print $1}')
sha_out_words=$(shasum -a 256 <"$work/delta/files/c/words-rep" | awk '{print $1}')

cat >"$work/delta/cryptex.manifest.json" <<EOF
{
 "role": "app_cryptex",
 "translator": "test",
 "files": [
  {
   "path": "/bin/app",
   "file": "files/bin/app",
   "sha256_in": "$sha_in_app",
   "sha256_out": "$sha_out_app",
   "cdhash": []
  },
  {
   "path": "/hl/rep1",
   "file": "files/hl/rep1",
   "sha256_in": "$sha_in_rep",
   "sha256_out": "$sha_out_rep",
   "cdhash": []
  },
  {
   "path": "/hl/rep2",
   "file": "files/hl/rep2",
   "sha256_in": "$sha_in_rep",
   "sha256_out": "$sha_out_rep",
   "cdhash": []
  },
  {
   "path": "/c/words-rep",
   "file": "files/c/words-rep",
   "sha256_in": "$sha_in_words",
   "sha256_out": "$sha_out_words",
   "cdhash": []
  }
 ]
}
EOF

say "== rebuild with switch_apfs_seal_tool"
out1=$work/rebuilt1.img
out2=$work/rebuilt2.img
out_fill=$work/rebuilt-fill.img
"$seal_tool" seal --source "$src" --out "$out1" --uuid "$PIN_UUID" --time-ns "$TIME_NS" \
    --manifest "$work/delta/cryptex.manifest.json" || die "seal build 1"
"$seal_tool" seal --source "$src" --out "$out2" --uuid "$PIN_UUID" --time-ns "$TIME_NS" \
    --manifest "$work/delta/cryptex.manifest.json" >/dev/null || die "seal build 2"
"$seal_tool" seal --source "$src" --out "$out_fill" --uuid "$PIN_UUID" --time-ns "$TIME_NS" \
    --manifest "$work/delta/cryptex.manifest.json" --via-fill >/dev/null || die "seal build via-fill"

if cmp -s "$out1" "$out2"; then
    say "determinism: two builds are byte-identical"
else
    fail "two builds differ"
fi
if cmp -s "$out1" "$out_fill"; then
    say "SealFill: byte-identical to seal_to_file"
else
    fail "SealFill output differs from seal_to_file"
fi
rm -f -- "$out2" "$out_fill"

check_sealed_image "rebuilt" "$out1" "$PIN_UUID" "CryptexTest"

say "== mount read-only with owners and verify every entry"
attach "$out1" -readonly || die "attach rebuilt"
diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt-rebuilt" "${vols[0]}" >/dev/null \
    || die "mount rebuilt"
sudo -n /usr/bin/python3 -I "$tree_list" "$work/mnt-rebuilt" | tee "$work/rebuilt.tsv" >/dev/null \
    || fail "tree_list rebuilt"
/usr/bin/python3 -I - "$work/rebuilt.tsv" "$work/mnt-rebuilt" \
    "$sha_out_app" "$sha_out_rep" "$sha_out_words" "$sha_in_words" \
    "$sha_kept_big" "$sha_kept_hl" "$sha_kept_xfile" "$sha_big_xattr" "$long" <<'PY' || fail "mount verification"
import hashlib, os, sys

tsv, mp = sys.argv[1], sys.argv[2]
(
    sha_out_app, sha_out_rep, sha_out_words, sha_in_words,
    sha_kept_big, sha_kept_hl, sha_kept_xfile, sha_big_xattr, long_sym,
) = sys.argv[3:12]

rows = {}
with open(tsv, encoding='utf-8') as f:
    for line in f:
        cols = line.rstrip('\n').split('\t')
        rows[cols[0]] = {
            'kind': cols[1], 'ino': cols[2], 'owner': cols[3], 'mode': cols[4],
            'size': int(cols[5]), 'flags': int(cols[6], 16), 'nlink': int(cols[7]),
            'xattrs': cols[12], 'content': cols[13],
        }

def check(path, **want):
    assert path in rows, f"missing {path}"
    r = rows[path]
    for k, v in want.items():
        assert r[k] == v, f"{path} {k}: got {r[k]!r}, want {v!r}"

check('/bin/app', kind='f', owner='0:0', mode='4755', flags=0, nlink=1, content=sha_out_app, xattrs='-')
check('/bin/big', kind='f', nlink=1, content=sha_kept_big)
check('/bin/empty', kind='f', size=0, nlink=1, content=hashlib.sha256(b'').hexdigest())
check('/hl/kept1', kind='f', owner='501:20', flags=0x8000, nlink=2, content=sha_kept_hl)
check('/hl/kept2', kind='f', owner='501:20', flags=0x8000, nlink=2, content=sha_kept_hl)
assert rows['/hl/kept1']['ino'] == rows['/hl/kept2']['ino'], "kept hard-link group split"
check('/hl/rep1', kind='f', owner='0:0', nlink=2, content=sha_out_rep, xattrs='-')
check('/hl/rep2', kind='f', owner='0:0', nlink=2, content=sha_out_rep, xattrs='-')
assert rows['/hl/rep1']['ino'] == rows['/hl/rep2']['ino'], "replaced hard-link group split"
check('/x/sym', kind='l', nlink=1, content='../bin/app')
check('/x/long-sym', kind='l', nlink=1, content=long_sym)
check('/x/with_xattrs', kind='f', owner='501:20', content=sha_kept_xfile)
assert 'com.example.big=' + sha_big_xattr[:16] in rows['/x/with_xattrs']['xattrs'], rows['/x/with_xattrs']['xattrs']
assert 'com.example.small' not in rows['/x/with_xattrs']['xattrs'], rows['/x/with_xattrs']['xattrs']
# Kept compressed file still has UF_COMPRESSED (0x20) and decompresses to web2.
check('/c/words-kept', kind='f', flags=0x20, content=sha_in_words)
# Replaced compressed file lost UF_COMPRESSED and decmpfs/ResourceFork.
check('/c/words-rep', kind='f', owner='0:0', flags=0, content=sha_out_words, xattrs='-')
print(f"mount verified: {len(rows)} entries matched SHA-256, owners, modes, flags, hard links, xattrs, symlinks")
PY
diskutil unmount "$work/mnt-rebuilt" >/dev/null
detach_all

say "== negative controls"
cp -c "$out1" "$work/neg-data.img"
"$seal_tool" flip "$work/neg-data.img" --target data --path /bin/app || die "flip data"
attach "$work/neg-data.img" -readonly || die "attach neg-data"
if "$R/apfs_checkseal" "/dev/${vols[0]}" >/dev/null 2>&1; then
    fail "apfs_checkseal passed a flipped data byte"
else
    say "negative control (data byte flipped): apfs_checkseal failed as expected"
fi
detach_all
rm -f -- "$work/neg-data.img"

cp -c "$out1" "$work/neg-leaf.img"
"$seal_tool" flip "$work/neg-leaf.img" --target leaf || die "flip leaf"
attach "$work/neg-leaf.img" -readonly || die "attach neg-leaf"
if "$R/apfs_checkseal" "/dev/${vols[0]}" >/dev/null 2>&1; then
    fail "apfs_checkseal passed a flipped fs-tree leaf byte"
else
    say "negative control (fs leaf byte flipped): apfs_checkseal failed as expected"
fi
detach_all
rm -f -- "$work/neg-leaf.img"
rm -f -- "$out1" "$src"

say "== deep multi-level tree test (600+ files, >16 MiB fitted with 0 free blocks)"
deep_src=$work/deep-src.img
truncate -s 96m "$deep_src"
attach "$deep_src" || die "attach blank deep"
newfs_apfs -e -v DeepSrc "/dev/$disk" >/dev/null || die "newfs_apfs deep"
detach_all
attach "$deep_src" || die "attach deep"
diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt-deep" "${vols[0]}" >/dev/null || die "mount deep"
for d in {1..12}; do
    mkdir -p "$work/mnt-deep/d$d"
    for f in {1..55}; do
        printf 'dir %s file %s\n' "$d" "$f" >"$work/mnt-deep/d$d/file-$f"
    done
done
# 18 MiB blob forces fitted container > MIN_BLOCKS (4096) with 0 free blocks.
head -c $((18 << 20)) /dev/urandom >"$work/mnt-deep/big.bin"
big_sha=$(shasum -a 256 <"$work/mnt-deep/big.bin" | awk '{print $1}')
diskutil unmount "$work/mnt-deep" >/dev/null || die "unmount deep"
"$R/apfs_prepare_cryptex" -p "/dev/${vols[0]}" >"$work/deep-seal.log" 2>&1 || {
    cat "$work/deep-seal.log"
    die "apfs_prepare_cryptex deep"
}
detach_all

printf 'replaced in deep tree\n' >"$work/deep-rep.txt"
deep_out=$work/deep-out.img
"$seal_tool" seal --source "$deep_src" --out "$deep_out" --uuid "$DEEP_UUID" --name DeepSealed \
    --time-ns "$TIME_NS" --replace "/d1/file-1=$work/deep-rep.txt" >"$work/deep-build.txt" || die "seal deep"
cat "$work/deep-build.txt"
grep -q '0 free' "$work/deep-build.txt" || fail "expected 0 free blocks in >16 MiB fitted container"
grep -Eq '\([2-9] levels\)' "$work/deep-build.txt" || fail "expected multi-level fs tree in deep test"

check_sealed_image "deep" "$deep_out" "$DEEP_UUID" "DeepSealed"

attach "$deep_out" -readonly || die "attach deep rebuilt"
diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt-deep" "${vols[0]}" >/dev/null \
    || die "mount deep rebuilt"
got_rep=$(cat "$work/mnt-deep/d1/file-1")
got_big=$(shasum -a 256 <"$work/mnt-deep/big.bin" | awk '{print $1}')
count=$(find "$work/mnt-deep" -type f | wc -l | tr -d ' ')
big_ok=n
if [[ $got_big == "$big_sha" ]]; then
    big_ok=y
fi
[[ $got_rep == "replaced in deep tree" && $big_ok == y && $count == 661 ]] \
    || fail "deep mount check: rep=$got_rep big_ok=$big_ok count=$count"
say "deep mount verified: $count files, big.bin SHA-256 matched"
diskutil unmount "$work/mnt-deep" >/dev/null
detach_all
rm -f -- "$deep_src" "$deep_out"

say "== clones, empty owner replace, NFC/NFD lookup, shared extents, and Windows --replace"
clone_src=$work/clones-src.img
truncate -s 96m "$clone_src"
attach "$clone_src" || die "attach blank clones"
newfs_apfs -e -v CloneSrc "/dev/$disk" >/dev/null || die "newfs_apfs clones"
detach_all
attach "$clone_src" || die "attach clones"
diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt" "${vols[0]}" >/dev/null || die "mount clones"
sudo -n chown "$uid:$gid" "$work/mnt" || die "chown clones"
head -c 32768 /dev/urandom >"$work/mnt/reg_a"
cp -c "$work/mnt/reg_a" "$work/mnt/reg_b" || die "cp -c reg"
ditto --hfsCompression "$work/web2" "$work/mnt/comp_a" || die "ditto comp_a"
cp -c "$work/mnt/comp_a" "$work/mnt/comp_b" || die "cp -c comp"
head -c 32768 /dev/urandom >"$work/mnt/part_a"
cp -c "$work/mnt/part_a" "$work/mnt/part_b" || die "cp -c part"
printf 'PATCH' | dd of="$work/mnt/part_b" bs=1 count=5 conv=notrunc 2>/dev/null
/usr/bin/python3 -I - "$work/mnt" <<'PY' || die "create NFD entry"
import os, sys
with open(os.path.join(sys.argv[1], "cafe\u0301"), 'wb') as f:
    f.write(b"stored as nfd\n")
PY
sha_reg_b=$(shasum -a 256 <"$work/mnt/reg_b" | awk '{print $1}')
sha_part_a=$(shasum -a 256 <"$work/mnt/part_a" | awk '{print $1}')
sha_part_b=$(shasum -a 256 <"$work/mnt/part_b" | awk '{print $1}')
diskutil unmount "$work/mnt" >/dev/null || die "unmount clones"
"$R/apfs_prepare_cryptex" -p "/dev/${vols[0]}" >"$work/clones-seal.log" 2>&1 || {
    cat "$work/clones-seal.log"
    die "apfs_prepare_cryptex clones"
}
detach_all

: >"$work/empty.bin"
printf 'nfc replacement\n' >"$work/nfc-rep.bin"
sha_nfc_rep=$(shasum -a 256 <"$work/nfc-rep.bin" | awk '{print $1}')
nfc_path=$(/usr/bin/python3 -I -c 'import sys; sys.stdout.write("/caf\u00e9")')
clone_out=$work/clones-out.img
"$seal_tool" seal --source "$clone_src" --out "$clone_out" --uuid "$PIN_UUID" --time-ns "$TIME_NS" \
    --replace "/reg_a=$work/empty.bin" --replace "${nfc_path}=$work/nfc-rep.bin" >"$work/clones-build.txt" \
    || die "seal clones"
cat "$work/clones-build.txt"
check_sealed_image "clones" "$clone_out" "$PIN_UUID" "CloneSrc"
# Shared physical extents (comp_a/comp_b + part_a/part_b) must stay deduplicated (< 400 data blocks, not ~520).
clone_data_blks=$(sed -n 's/.*data \([0-9]*\) blocks.*/\1/p' "$work/clones-build.txt")
[[ -n $clone_data_blks && $clone_data_blks -lt 400 ]] \
    || fail "expected deduplicated shared extents (<400 data blocks), got ${clone_data_blks:-none}"

attach "$clone_out" -readonly || die "attach clones rebuilt"
diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt-rebuilt" "${vols[0]}" >/dev/null \
    || die "mount clones rebuilt"
/usr/bin/python3 -I - "$work/mnt-rebuilt" "$sha_reg_b" "$sha_in_words" "$sha_part_a" "$sha_part_b" "$sha_nfc_rep" <<'PY' || fail "clones mount verification"
import hashlib, os, sys
mp, sha_reg_b, sha_web2, sha_part_a, sha_part_b, sha_nfc_rep = sys.argv[1:7]
def digest(rel):
    with open(os.path.join(mp, rel), 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()
assert digest('reg_a') == hashlib.sha256(b'').hexdigest(), "reg_a not empty"
assert digest('reg_b') == sha_reg_b, "reg_b mismatch"
assert digest('comp_a') == sha_web2 and digest('comp_b') == sha_web2, "compressed clones mismatch"
assert digest('part_a') == sha_part_a and digest('part_b') == sha_part_b, "partial clones mismatch"
assert digest('cafe\u0301') == sha_nfc_rep, "NFC replacement mismatch"
print("clones + NFC mount verified")
PY
diskutil unmount "$work/mnt-rebuilt" >/dev/null
detach_all
rm -f -- "$clone_src" "$clone_out"

# Apple's own (cp -c a b; : > a) sealed image must also walk cleanly under switch_apfs_read_tool.
apple_trunc=$work/apple-trunc.img
truncate -s 64m "$apple_trunc"
attach "$apple_trunc" || die "attach blank apple-trunc"
newfs_apfs -e -v AppleTrunc "/dev/$disk" >/dev/null || die "newfs_apfs apple-trunc"
detach_all
attach "$apple_trunc" || die "attach apple-trunc"
diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt" "${vols[0]}" >/dev/null || die "mount apple-trunc"
sudo -n chown "$uid:$gid" "$work/mnt" || die "chown apple-trunc"
head -c 32768 /dev/urandom >"$work/mnt/a"
cp -c "$work/mnt/a" "$work/mnt/b" || die "cp -c apple-trunc"
sync
: >"$work/mnt/a"
diskutil unmount "$work/mnt" >/dev/null || die "unmount apple-trunc"
"$R/apfs_prepare_cryptex" -p "/dev/${vols[0]}" >/dev/null 2>&1 || die "apfs_prepare_cryptex apple-trunc"
detach_all
"$read_tool" walk "$apple_trunc" --volume 0 --hash-every 1 --check-file-info >/dev/null \
    || fail "switch_apfs_read_tool failed on Apple's own (cp -c a b; : > a)"
rm -f -- "$apple_trunc"

# Windows drive-letter --replace must parse past the colon (and fail only on opening the source/file, not invalid hex).
win_err=$("$seal_tool" seal --source /dev/null --out "$work/w.img" --uuid "$PIN_UUID" --time-ns "$TIME_NS" \
    --replace '/bin/app=C:\Users\test\app.bin' 2>&1 || true)
if grep -q 'invalid sha256 hex' <<<"$win_err"; then
    fail "Windows --replace path misparsed as sha256: $win_err"
fi

say "failures: $failures"
[[ $failures == 0 ]]
