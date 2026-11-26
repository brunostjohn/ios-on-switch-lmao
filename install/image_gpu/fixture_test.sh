#!/bin/bash
# Darwin APFS fixture check for switch_image_gpu:
# stages a format 2 GPU bundle (including a >= 1 MiB file, # kext digest, and pref merges into existing
# Content::Source XML, binary, and decmpfs-compressed plists) into a real APFS container, runs fsck_apfs -n,
# mounts Switch-iOS-Data with owners, and checks parity with Python stage-tegra-gm20b-metal.py.
set -o pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

gpu_tool=$PWD/$1
m1_tool=$PWD/$2
work=$(mktemp -d /tmp/apfs_gpu.XXXXXX)
DATA_UUID=D1B1D053-D0A6-496C-8625-A947A3DA863E
KEXT_DIGEST=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
failures=0
attached=()
vols=()
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

cleanup() {
    diskutil unmount force "$work/mnt" >/dev/null 2>&1
    detach_all
    if [[ $work == /tmp/apfs_gpu.* && -d $work ]]; then
        sudo -n rm -rf -- "$work"
    fi
}
trap cleanup EXIT

check_stat() {
    local path=$1 want_owner=$2 want_mode=$3 got_owner got_mode
    got_owner=$(stat -f '%u:%g' "$path") || {
        fail "stat $path"
        return
    }
    got_mode=$(stat -f '%Mp%Lp' "$path") || {
        fail "stat $path"
        return
    }
    [[ $got_owner == "$want_owner" ]] || fail "$path owner=$got_owner want=$want_owner"
    [[ $got_mode == "$want_mode" ]] || fail "$path mode=$got_mode want=$want_mode"
}

sudo -n true || die "passwordless sudo is required"
mkdir -p "$work/mnt" "$work/gpu_stage" "$work/compressed_src"

src=$work/source.img
truncate -s 512m "$src"
attach "$src" || die "attach blank source"
newfs_apfs -e -v System "/dev/$disk" >/dev/null || die "newfs_apfs"
detach_all

attach "$src" || die "attach source"
diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt" "${vols[0]}" >/dev/null || die "mount source"
v=$work/mnt/private/var
sudo -n mkdir -p "$v/mobile/Library/Preferences" "$v/root/Library/Preferences" || die "mkdir /private/var"
sudo -n chown 501:501 "$v/mobile" "$v/mobile/Library" "$v/mobile/Library/Preferences"
sudo -n chmod 0755 "$v/mobile" "$v/mobile/Library" "$v/mobile/Library/Preferences"

/usr/bin/python3 - "$work" <<'PY'
import plistlib
from pathlib import Path

work = Path(__import__('sys').argv[1])
xml_bb = plistlib.dumps(
    {'ExistingMobileKey': 'keep-me', 'CAEnableDeepFramebuffer': True},
    fmt=plistlib.FMT_XML,
)
(work / 'mobile_bb.plist').write_bytes(xml_bb)

bin_global = plistlib.dumps(
    {'AppleLocale': 'en_US', 'CAEnableIOSurfaceCompression': True},
    fmt=plistlib.FMT_BINARY,
)
(work / 'mobile_global.plist').write_bytes(bin_global)

# Large repetitive XML plist (>= 20 KiB) so ditto --hfsCompression stores it with APFS decmpfs (UF_COMPRESSED).
cmp_root = plistlib.dumps(
    {'FromCompressedRoot': 'compressible-' * 2000, 'CAEnableFramebufferCompression': True},
    fmt=plistlib.FMT_XML,
)
(work / 'compressed_src/com.apple.backboardd.plist').write_bytes(cmp_root)
PY

sudo -n cp "$work/mobile_bb.plist" "$v/mobile/Library/Preferences/com.apple.backboardd.plist"
sudo -n chown 501:501 "$v/mobile/Library/Preferences/com.apple.backboardd.plist"
sudo -n chmod 0600 "$v/mobile/Library/Preferences/com.apple.backboardd.plist"

sudo -n cp "$work/mobile_global.plist" "$v/mobile/Library/Preferences/.GlobalPreferences.plist"
sudo -n chown 501:501 "$v/mobile/Library/Preferences/.GlobalPreferences.plist"
sudo -n chmod 0600 "$v/mobile/Library/Preferences/.GlobalPreferences.plist"

sudo -n ditto --hfsCompression \
    "$work/compressed_src/com.apple.backboardd.plist" \
    "$v/root/Library/Preferences/com.apple.backboardd.plist" || die "ditto --hfsCompression"
sudo -n chown 0:0 "$v/root/Library/Preferences/com.apple.backboardd.plist"
sudo -n chmod 0600 "$v/root/Library/Preferences/com.apple.backboardd.plist"

cmp_flags=$(stat -f '%f' "$v/root/Library/Preferences/com.apple.backboardd.plist")
if (((cmp_flags & 0x20) == 0)); then
    die "expected UF_COMPRESSED on source root/Library/Preferences/com.apple.backboardd.plist (flags=$cmp_flags)"
fi

diskutil unmount "$work/mnt" >/dev/null || die "unmount source"
detach_all

system_uuid=$("$m1_tool" inspect "$src" | sed -n 's/^volume 0 .* uuid \([0-9A-F-]*\) .*/\1/p')
[[ -n $system_uuid ]] || die "inspect source"
"$m1_tool" set-role "$src" --uuid "$system_uuid" --role 1 >/dev/null || die "set-role"

/usr/bin/python3 - "$work/gpu_stage" "$KEXT_DIGEST" <<'PY'
import hashlib, json, sys
from pathlib import Path

stage_dir = Path(sys.argv[1])
kext = sys.argv[2]

files = [
    ('ngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal', b'\xcf\xfa\xed\xfe' + b'metal-plugin-v2' * 64, 0o755),
    ('ngpu/addons/TegraGM20BMetal.bundle/Info.plist', b'<?xml version="1.0"?><plist version="1.0"><dict/></plist>\n', 0o644),
    ('ngpu/addons/TegraGM20BMetal.seed/cache.seed', b'seed-data-01', 0o644),
    # >= 1 MiB to exercise FileFill streaming into APFS extents.
    ('ngpu/addons/TegraGM20BMetal.pack/pipelines.bin', (b'PACK' + bytes(range(256))) * 6000, 0o644),
]

prefs = {
    'CAEnableDeepFramebuffer': False,
    'CAEnableFramebufferCompression': False,
    'CAEnableIOSurfaceCompression': False,
}
pref_files = ('com.apple.backboardd.plist', 'backboardd.plist', '.GlobalPreferences.plist')
homes = (('mobile', 501), ('root', 0))

lines = ['# gpu stage 2 tree', f'# kext {kext}']
for rel, data, mode in sorted(files):
    target = stage_dir / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    target.chmod(mode)
    sha = hashlib.sha256(data).hexdigest()
    lines.append(f'file\t{rel}\t{sha}\t{len(data)}\t{mode:o}\t{rel}')

for home, uid in homes:
    for name in pref_files:
        for key, value in prefs.items():
            lines.append(f'pref\t{home}/Library/Preferences/{name}\t{uid}\t{key}\t{json.dumps(value)}')

lines.append('pref\tmobile/Library/Preferences/com.apple.backboardd.plist\t501\tExtraCount\t42')
lines.append('pref\tmobile/Library/Preferences/com.apple.backboardd.plist\t501\tExtraTags\t["a", 2, true]')

(stage_dir / 'gpu.stage').write_text('\n'.join(lines) + '\n')
PY

out=$work/gpu.img
"$gpu_tool" \
    --source "$src" \
    --target "$out" \
    --target-bytes $((2 << 30)) \
    --system-uuid "$system_uuid" \
    --data-uuid "$DATA_UUID" \
    --gpu-stage "$work/gpu_stage" || die "gpu_tool build failed"

/sbin/fsck_apfs -n "$out" >"$work/fsck.txt" 2>&1
rc=$?
oks=$(grep -c 'appears to be OK' "$work/fsck.txt")
bad=$(grep -iE '^(warning|error)|corrupt|invalid|fail' "$work/fsck.txt" | grep -v 'not a valid disk name/path')
if [[ $rc == 0 && $oks == 3 && -z $bad ]]; then
    say "fsck_apfs -n: 3 OK, no warnings"
else
    cat "$work/fsck.txt"
    fail "fsck_apfs -n failed (rc=$rc oks=$oks)"
fi

attach "$out" -readonly || die "attach built image"
slice=$(volume_slice "$DATA_UUID") || die "find Data volume"
diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt" "$slice" >/dev/null || die "mount Data"
m=$work/mnt

check_stat "$m/ngpu" "0:0" "0755"
check_stat "$m/ngpu/addons" "0:0" "0755"
check_stat "$m/ngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal" "0:0" "0755"
check_stat "$m/ngpu/addons/TegraGM20BMetal.bundle/Info.plist" "0:0" "0644"
check_stat "$m/ngpu/addons/TegraGM20BMetal.seed/cache.seed" "0:0" "0644"
check_stat "$m/ngpu/addons/TegraGM20BMetal.pack/pipelines.bin" "0:0" "0644"
check_stat "$m/ngpu/stage.tsv.gz" "0:0" "0644"
check_stat "$m/Switch" "0:0" "0755"
check_stat "$m/Switch/icon-cache-gpu" "0:0" "0644"

check_stat "$m/mobile" "501:501" "0755"
check_stat "$m/mobile/Library" "501:501" "0755"
check_stat "$m/mobile/Library/Preferences" "501:501" "0755"
check_stat "$m/mobile/Library/Preferences/com.apple.backboardd.plist" "501:501" "0644"
check_stat "$m/mobile/Library/Preferences/backboardd.plist" "501:501" "0644"
check_stat "$m/mobile/Library/Preferences/.GlobalPreferences.plist" "501:501" "0644"
check_stat "$m/root/Library/Preferences/com.apple.backboardd.plist" "0:0" "0644"
check_stat "$m/root/Library/Preferences/backboardd.plist" "0:0" "0644"
check_stat "$m/root/Library/Preferences/.GlobalPreferences.plist" "0:0" "0644"

post_cmp_flags=$(stat -f '%f' "$m/root/Library/Preferences/com.apple.backboardd.plist")
if (((post_cmp_flags & 0x20) != 0)); then
    fail "merged root/Library/Preferences/com.apple.backboardd.plist still has UF_COMPRESSED"
fi

/usr/bin/python3 - "$work/gpu_stage" "$m" "$KEXT_DIGEST" <<'PY' || fail "python parity check failed"
import gzip, hashlib, json, plistlib, sys
from pathlib import Path

stage_dir, data, kext = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
lines = (stage_dir / 'gpu.stage').read_text().splitlines()
assert lines[0] == '# gpu stage 2 tree'

wanted = {}
prefs = {}
for line in lines[1:]:
    if line.startswith('# kext '):
        continue
    kind, *fields = line.split('\t')
    if kind == 'file':
        p, sha, size, mode, src = fields
        wanted[p] = (stage_dir / src, sha, int(size), int(mode, 8))
    elif kind == 'pref':
        pl, uid, key, val = fields
        _, vals = prefs.setdefault(pl, (int(uid), {}))
        vals[key] = json.loads(val)

for rel, (src_path, sha, size, _mode) in sorted(wanted.items()):
    got = (data / rel).read_bytes()
    assert len(got) == size, f'{rel}: len {len(got)} != {size}'
    assert hashlib.sha256(got).hexdigest() == sha, f'{rel}: sha mismatch'
    assert got == src_path.read_bytes(), f'{rel}: bytes mismatch'

whole_h = hashlib.sha256()
expected_tsv_lines = []
for p in sorted(wanted):
    _, sha, size, mode = wanted[p]
    rec = f'{p}\t{sha}\t{size}\t{mode:o}\n'
    whole_h.update(rec.encode())
    expected_tsv_lines.append(rec)
whole = whole_h.hexdigest()
expected_tsv = f'# {whole}\n' + ''.join(expected_tsv_lines)

got_tsv = gzip.decompress((data / 'ngpu/stage.tsv.gz').read_bytes()).decode()
assert got_tsv == expected_tsv, f'stage.tsv.gz mismatch:\n{got_tsv!r}\nvs\n{expected_tsv!r}'

expected_icon = hashlib.sha256(f'{whole}\n{kext}'.encode()).hexdigest()
got_icon = (data / 'Switch/icon-cache-gpu').read_text()
assert got_icon == expected_icon + '\n', f'icon-cache-gpu mismatch: {got_icon!r}'

for pl, (_uid, expected_vals) in sorted(prefs.items()):
    raw = (data / pl).read_bytes()
    assert raw.startswith(b'bplist00'), f'{pl}: not binary plist'
    loaded = plistlib.loads(raw)
    for k, v in expected_vals.items():
        assert k in loaded and type(loaded[k]) is type(v) and loaded[k] == v, f'{pl}[{k}]={loaded.get(k)!r} want {v!r}'

mobile_bb = plistlib.loads((data / 'mobile/Library/Preferences/com.apple.backboardd.plist').read_bytes())
assert mobile_bb['ExistingMobileKey'] == 'keep-me'

mobile_gl = plistlib.loads((data / 'mobile/Library/Preferences/.GlobalPreferences.plist').read_bytes())
assert mobile_gl['AppleLocale'] == 'en_US'

root_bb = plistlib.loads((data / 'root/Library/Preferences/com.apple.backboardd.plist').read_bytes())
assert root_bb['FromCompressedRoot'] == 'compressible-' * 2000
print('python parity check: OK')
PY

diskutil unmount "$work/mnt" >/dev/null
detach_all

say "failures: $failures"
[[ $failures == 0 ]]
