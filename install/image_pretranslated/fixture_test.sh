#!/bin/bash
# End-to-end macOS APFS fixture check for switch_image + switch_image_pretranslated + switch_apfs_write.
# Needs macOS with hdiutil, newfs_apfs, fsck_apfs, ditto and passwordless sudo.
set -o pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

tool=$PWD/$1
m1=$PWD/$2
reader=$PWD/$3
work=$(mktemp -d /tmp/switch_image_fixture.XXXXXX)
DATA_UUID=D1B1D053-D0A6-496C-8625-A947A3DA863E
PREBOOT_UUID=4AA4A761-5138-439D-9A30-D4138F6887FA
TIME_NS=1795000000000000000
TARGET_BYTES=$((2 << 30))
CRYPTEX_BYTES=$((4 << 20))
failures=0
vols=()
attached=()
disk=

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

cleanup() {
    local m
    for m in mnt-sys mnt-data mnt-boot; do
        diskutil unmount force "$work/$m" >/dev/null 2>&1
    done
    detach_all
    if [[ $work == /tmp/switch_image_fixture.* && -d $work ]]; then
        sudo -n rm -rf -- "$work"
    fi
}
trap cleanup EXIT

sudo -n true || die "passwordless sudo is required"
mkdir -p "$work/mnt-sys" "$work/mnt-data" "$work/mnt-boot" "$work/delta/files"

src=$work/source.img
truncate -s 256m "$src"
attach "$src" || die "attach blank source"
newfs_apfs -e -v System "/dev/$disk" >/dev/null || die "newfs_apfs"
detach_all

attach "$src" || die "attach source"
diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt-sys" "${vols[0]}" >/dev/null || die "mount source"

mp=$work/mnt-sys
v=$mp/private/var
uid=$(id -u)
gid=$(id -g)
sudo -n mkdir -p "$v/mobile" "$v/db" "$v/staged_system_apps/App.app" "$mp/usr/libexec" "$mp/usr/sbin" || die "mkdir"
sudo -n chown -R "$uid:$gid" "$mp/private" "$mp/usr" || die "chown temp"

printf 'original plain var file\n' >"$v/db/plain"
yes 'compressible var payload line' | head -c 32768 >"$work/comp_src"
ditto --hfsCompression "$work/comp_src" "$v/db/compressed" || die "compress var file"
ditto --hfsCompression "$work/comp_src" "$v/db/keep_compressed" || die "compress untouched var file"
printf 'original hardlinked var file\n' >"$v/db/hl1"
ln "$v/db/hl1" "$v/db/hl2"
printf 'original staged app bytes\n' >"$v/staged_system_apps/App.app/App"

yes 'compressible system tool' | head -c 24576 >"$work/tool_src"
ditto --hfsCompression "$work/tool_src" "$mp/usr/libexec/tool" || die "compress system tool"
printf 'original system hardlink\n' >"$mp/usr/libexec/linked_a"
ln "$mp/usr/libexec/linked_a" "$mp/usr/sbin/linked_b"

/usr/bin/python3 -I - "$mp" "$work/delta" <<'PY' || die "create delta"
import hashlib
import json
import os
import sys
from pathlib import Path

mp = Path(sys.argv[1])
delta = Path(sys.argv[2])

replacements = [
    ("/private/var/db/plain", b"translated plain var file\n", []),
    ("/private/var/db/compressed", b"translated uncompressed var file\n", []),
    ("/private/var/db/hl1", b"translated hardlinked var file\n", []),
    ("/private/var/db/hl2", b"translated hardlinked var file\n", []),
    ("/private/var/staged_system_apps/App.app/App", b"translated staged app binary\n", ["44" * 20]),
    ("/usr/libexec/tool", b"translated system tool binary\n", ["11" * 20]),
    ("/usr/libexec/linked_a", b"translated system hardlink binary\n", ["22" * 20]),
    ("/usr/sbin/linked_b", b"translated system hardlink binary\n", ["22" * 20]),
]

files = []
for vpath, out_bytes, cdhash in replacements:
    in_bytes = (mp / vpath.lstrip("/")).read_bytes()
    rel = "files" + vpath
    dest = delta / rel
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(out_bytes)
    files.append({
        "path": vpath,
        "file": rel,
        "sha256_in": hashlib.sha256(in_bytes).hexdigest(),
        "sha256_out": hashlib.sha256(out_bytes).hexdigest(),
        "cdhash": cdhash,
    })

(delta / "os.manifest.json").write_text(json.dumps({"role": "os", "translator": "fixture", "files": files}, indent=1))
(delta / "os.cdhash-pairs").write_text(f"{'33' * 20} {'44' * 20}\n")
PY

chmod 0644 "$v/db/plain"
chmod 0755 "$v/db/compressed" "$v/db/keep_compressed" "$v/staged_system_apps/App.app/App" "$mp/usr/libexec/tool"
chmod 0640 "$v/db/hl1"
chmod 0775 "$mp/usr/libexec/linked_a"

sudo -n chown 501:501 "$v/mobile" "$v/staged_system_apps" "$v/staged_system_apps/App.app" "$v/staged_system_apps/App.app/App" || die "chown mobile"
sudo -n chown 0:0 "$v/db" "$v/db/plain" "$v/db/compressed" "$v/db/keep_compressed" "$mp/usr" "$mp/usr/libexec" "$mp/usr/sbin" "$mp/usr/libexec/tool" || die "chown root"
sudo -n chown 64:64 "$v/db/hl1" || die "chown hl"
sudo -n chown 0:80 "$mp/usr/libexec/linked_a" || die "chown admin"
sudo -n chflags hidden "$v/db/compressed" "$mp/usr/libexec/tool" || die "chflags hidden"

diskutil unmount "$work/mnt-sys" >/dev/null || die "unmount source"
detach_all

system_uuid=$("$m1" inspect "$src" | sed -n 's/^volume 0 .* uuid \([0-9A-F-]*\) .*/\1/p')
[[ -n $system_uuid ]] || die "inspect source"
"$m1" set-role "$src" --uuid "$system_uuid" --role 1 >/dev/null || die "set-role"
fsck_clean "$src" "$work/fsck-src.txt" 2 || {
    cat "$work/fsck-src.txt"
    die "source fsck"
}

cryptex=$work/test-cryptex.dmg
"$m1" gen --bytes "$CRYPTEX_BYTES" --seed 42 >"$cryptex" || die "generate cryptex"
cryptex_sha=$(shasum -a 256 <"$cryptex" | cut -c1-64)

out=$work/out.img
"$tool" compose --source "$src" --target "$out" --target-bytes "$TARGET_BYTES" \
    --system-uuid "$system_uuid" --data-uuid "$DATA_UUID" --preboot-uuid "$PREBOOT_UUID" \
    --unicode 17.0 --time-ns "$TIME_NS" --delta-dir "$work/delta" \
    --cryptex "test-cryptex.dmg:$cryptex:$cryptex_sha" --manifest-out "$work/returned-manifest.json" \
    || die "compose"

"$tool" compose --source "$src" --target "$work/out2.img" --target-bytes "$TARGET_BYTES" \
    --system-uuid "$system_uuid" --data-uuid "$DATA_UUID" --preboot-uuid "$PREBOOT_UUID" \
    --unicode 17.0 --time-ns "$TIME_NS" --delta-dir "$work/delta" \
    --cryptex "test-cryptex.dmg:$cryptex:$cryptex_sha" >/dev/null || die "second compose"
cmp -s "$out" "$work/out2.img" || fail "two builds differ"
rm -f -- "$work/out2.img"

"$reader" audit "$out" >"$work/audit.txt" || {
    cat "$work/audit.txt"
    fail "allocation audit"
}
"$reader" audit "$out" --list >"$work/claims.txt" || fail "audit list"
extents=$(awk '$1 == "claim" && $4 ~ /^Data\(2,/ { print $3 }' "$work/claims.txt" | tr '\n' ' ')
[[ $extents == "$((CRYPTEX_BYTES / 4096)) " ]] || fail "Preboot cryptex extents: $extents"

if fsck_clean "$out" "$work/fsck-out.txt" 4; then
    say "fsck_apfs -n: 4 OK, no warnings"
else
    cat "$work/fsck-out.txt"
    fail "fsck_apfs -n"
fi

if "$tool" compose --source "$src" --target "$work/bad.img" --target-bytes "$TARGET_BYTES" \
    --system-uuid "$system_uuid" --data-uuid "$DATA_UUID" --preboot-uuid "$PREBOOT_UUID" \
    --unicode 17.0 --time-ns "$TIME_NS" --delta-dir "$work/delta" \
    --cryptex "test-cryptex.dmg:$cryptex:0000000000000000000000000000000000000000000000000000000000000000" \
    >/dev/null 2>&1; then
    fail "wrong cryptex SHA-256 was accepted"
else
    say "control: wrong cryptex SHA-256 refused"
fi
rm -f -- "$work/bad.img"

attach "$out" -readonly || die "attach output"
[[ ${#vols[@]} == 3 ]] || die "expected 3 volumes, got ${#vols[@]}"
sys_slice=$(volume_slice "$system_uuid") || die "find System"
data_slice=$(volume_slice "$DATA_UUID") || die "find Data"
boot_slice=$(volume_slice "$PREBOOT_UUID") || die "find Preboot"

diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt-sys" "$sys_slice" >/dev/null || die "mount System"
diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt-data" "$data_slice" >/dev/null || die "mount Data"
diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt-boot" "$boot_slice" >/dev/null || die "mount Preboot"

"$tool" decode-index "$work/mnt-data/Switch/pretranslated/.index" \
    --originals "$work/mnt-data/Switch/pretranslated/.originals" >"$work/decoded.tsv" || fail "decode-index"

sudo -n /usr/bin/python3 -I - "$work/mnt-sys" "$work/mnt-data" "$work/mnt-boot" "$work/decoded.tsv" \
    "$work/returned-manifest.json" "$cryptex_sha" "$CRYPTEX_BYTES" <<'PY' || fail "mounted verification"
import hashlib
import json
import os
import stat
import struct
import sys
from pathlib import Path

sys_root = Path(sys.argv[1])
data_root = Path(sys.argv[2])
boot_root = Path(sys.argv[3])
decoded_path = Path(sys.argv[4])
returned_manifest = Path(sys.argv[5]).read_bytes()
cryptex_sha = sys.argv[6]
cryptex_bytes = int(sys.argv[7])

UF_HIDDEN = 0x8000
UF_COMPRESSED = 0x20
SF_IMMUTABLE = 0x20000

def check_stat(path, uid, gid, mode, flags=None):
    st = os.lstat(path)
    assert (st.st_uid, st.st_gid) == (uid, gid), f"{path}: owner {(st.st_uid, st.st_gid)} != {(uid, gid)}"
    assert (st.st_mode & 0o7777) == mode, f"{path}: mode {st.st_mode & 0o7777:04o} != {mode:04o}"
    if flags is not None:
        assert st.st_flags == flags, f"{path}: flags {st.st_flags:#x} != {flags:#x}"
    return st

check_stat(data_root, 0, 0, 0o755)
check_stat(data_root / "mobile/Library", 501, 501, 0o755)
check_stat(data_root / "mobile/Library/Preferences", 501, 501, 0o755)

check_stat(data_root / "db/plain", 0, 0, 0o644, 0)
assert (data_root / "db/plain").read_bytes() == b"translated plain var file\n"

comp_st = check_stat(data_root / "db/compressed", 0, 0, 0o755, UF_HIDDEN)
assert (comp_st.st_flags & UF_COMPRESSED) == 0
assert (data_root / "db/compressed").read_bytes() == b"translated uncompressed var file\n"

keep_st = os.lstat(data_root / "db/keep_compressed")
assert (keep_st.st_flags & UF_COMPRESSED) != 0

hl1 = check_stat(data_root / "db/hl1", 64, 64, 0o640, 0)
hl2 = check_stat(data_root / "db/hl2", 64, 64, 0o640, 0)
assert hl1.st_ino == hl2.st_ino and hl1.st_nlink == 2
assert (data_root / "db/hl1").read_bytes() == b"translated hardlinked var file\n"

check_stat(data_root / "staged_system_apps/App.app/App", 501, 501, 0o755, 0)
assert (data_root / "staged_system_apps/App.app/App").read_bytes() == b"original staged app bytes\n"

check_stat(data_root / "Switch", 0, 0, 0o755, 0)
pt_root = data_root / "Switch/pretranslated"
manifest_bytes = (data_root / "Switch/pretranslated.json").read_bytes()
check_stat(data_root / "Switch/pretranslated.json", 0, 0, 0o644, SF_IMMUTABLE)
assert manifest_bytes == returned_manifest
parsed_manifest = json.loads(manifest_bytes.decode())
assert manifest_bytes == (json.dumps(parsed_manifest, indent=1) + "\n").encode()

check_stat(pt_root / ".index", 0, 0, 0o644, SF_IMMUTABLE)
check_stat(pt_root / ".originals", 0, 0, 0o644, SF_IMMUTABLE)

tool_st = check_stat(pt_root / "usr/libexec/tool", 0, 0, 0o755, UF_HIDDEN | SF_IMMUTABLE)
assert (pt_root / "usr/libexec/tool").read_bytes() == b"translated system tool binary\n"

hl_a_st = check_stat(pt_root / "usr/libexec/linked_a", 0, 80, 0o775, SF_IMMUTABLE)
assert (pt_root / "usr/libexec/linked_a").read_bytes() == b"translated system hardlink binary\n"
assert not (pt_root / "usr/sbin/linked_b").exists()

app_pt_st = check_stat(pt_root / "private/var/staged_system_apps/App.app/App", 0, 0, 0o755, SF_IMMUTABLE)
assert (pt_root / "private/var/staged_system_apps/App.app/App").read_bytes() == b"translated staged app binary\n"

actual_dirs = sorted(os.lstat(d).st_ino for d in [pt_root, *(p for p in pt_root.rglob("*") if p.is_dir())])
for d in [pt_root, *(p for p in pt_root.rglob("*") if p.is_dir())]:
    check_stat(d, 0, 0, 0o755, SF_IMMUTABLE)

idx_files = {}
idx_dirs = []
idx_origs = []
for line in decoded_path.read_text().splitlines():
    parts = line.split("\t")
    if parts[0] == "file":
        idx_files[parts[1]] = (int(parts[2]), int(parts[3]), parts[4])
    elif parts[0] == "dir":
        idx_dirs.append(int(parts[1]))
    elif parts[0] == "orig":
        idx_origs.append((parts[1], parts[2]))

assert sorted(idx_dirs) == actual_dirs, f"dir ids mismatch: {idx_dirs} != {actual_dirs}"
expected_data_ids = {
    "/usr/libexec/tool": tool_st.st_ino,
    "/usr/libexec/linked_a": hl_a_st.st_ino,
    "/usr/sbin/linked_b": hl_a_st.st_ino,
    "/private/var/staged_system_apps/App.app/App": app_pt_st.st_ino,
}
assert set(idx_files) == set(expected_data_ids)
for path, want_data_id in expected_data_ids.items():
    got_sys_id, got_data_id, _ = idx_files[path]
    want_sys_id = os.lstat(sys_root / path.lstrip("/")).st_ino
    assert got_sys_id == want_sys_id, f"{path}: system id {got_sys_id} != {want_sys_id}"
    assert got_data_id == want_data_id, f"{path}: data id {got_data_id} != {want_data_id}"

assert idx_origs == [("33" * 20, "44" * 20)], f"originals mismatch: {idx_origs}"

raw_idx = (pt_root / ".index").read_bytes()
magic, ver, count, dcount, _, res = struct.unpack_from("<4sIIIII", raw_idx)
assert (magic, ver, count, dcount, res) == (b"SWOV", 2, 4, len(actual_dirs), 0)

check_stat(boot_root, 0, 0, 0o755)
check_stat(boot_root / "Switch-Staging", 0, 0, 0o755)
cryptex_path = boot_root / "Switch-Staging/test-cryptex.dmg"
c_st = check_stat(cryptex_path, 0, 0, 0o644)
assert c_st.st_size == cryptex_bytes
assert hashlib.sha256(cryptex_path.read_bytes()).hexdigest() == cryptex_sha

print("mounted verification passed: all inodes, owners, modes, flags, .index, .originals, manifest, and Preboot OK")
PY

say "failures: $failures"
[[ $failures == 0 ]]
