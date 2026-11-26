#!/bin/bash
# Darwin APFS fixture check for W6 (switch_image_overlay + switch_image_bootstrap):
# commits overlay, staged system apps, shader seed and bootstrap into an APFS container via switch_apfs_write,
# runs fsck_apfs -n, mounts Data with owners, and checks uid/gid/mode/symlinks/hard-links and file contents.
set -o pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

w6_tool=$PWD/$1
m1_tool=$PWD/$2
work=$(mktemp -d /tmp/apfs_w6.XXXXXX)
DATA_UUID=D1B1D053-D0A6-496C-8625-A947A3DA863E
BUILD=21H461
FIRMWARE=17.7.11
MANAGED_GEN=$(printf 'a%.0s' {1..64})
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
    if [[ $work == /tmp/apfs_w6.* && -d $work ]]; then
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
mkdir -p "$work/mnt" "$work/pkg/overlay" "$work/pkg/bootstrap" "$work/shader_seed/shaders"

src=$work/source.img
truncate -s 512m "$src"
attach "$src" || die "attach blank source"
newfs_apfs -e -v System "/dev/$disk" >/dev/null || die "newfs_apfs"
detach_all

attach "$src" || die "attach source"
diskutil mount -mountOptions nobrowse,owners -mountPoint "$work/mnt" "${vols[0]}" >/dev/null || die "mount source"
v=$work/mnt/private/var
sudo -n mkdir -p "$v/mobile/Library" "$v/shaders" "$v/staged_system_apps/Maps.app" || die "mkdir /private/var"
sudo -n chown 501:501 "$v/mobile" "$v/mobile/Library" "$v/staged_system_apps" "$v/staged_system_apps/Maps.app"
sudo -n /bin/sh -c "printf 'old-shader\n' >'$v/shaders/existing.bin'"
sudo -n chmod 0600 "$v/shaders/existing.bin"
sudo -n chown 501:501 "$v/shaders/existing.bin"
diskutil unmount "$work/mnt" >/dev/null || die "unmount source"
detach_all

system_uuid=$("$m1_tool" inspect "$src" | sed -n 's/^volume 0 .* uuid \([0-9A-F-]*\) .*/\1/p')
[[ -n $system_uuid ]] || die "inspect source"
"$m1_tool" set-role "$src" --uuid "$system_uuid" --role 1 >/dev/null || die "set-role"

ov=$work/pkg/overlay
mkdir -p \
    "$ov/usr/libexec" \
    "$ov/usr/bin" \
    "$ov/etc" \
    "$ov/Applications/SwitchInstaller.app/_CodeSignature" \
    "$ov/usr/share/Switch/Applications/DemoApp.app/_CodeSignature" \
    "$ov/System/Library/AppPlaceholders/DemoApp.app" \
    "$ov/System/Library/AppSignatures"

printf '#!/bin/sh\necho helper\n' >"$ov/usr/libexec/switch-bootstrap"
chmod 0555 "$ov/usr/libexec/switch-bootstrap"
printf 'conf=1\n' >"$ov/etc/switch.conf"
chmod 0444 "$ov/etc/switch.conf"
printf 'appledouble' >"$ov/etc/._switch.conf"
printf 'appledouble-root' >"$ov/._top"
ln -s '../libexec/switch-bootstrap' "$ov/usr/bin/helper-link"

cat >"$ov/Applications/SwitchInstaller.app/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>com.brunostjohn.switch.installer</string>
  <key>CFBundleExecutable</key><string>SwitchInstaller</string>
  <key>CFBundleVersion</key><string>10</string>
</dict></plist>
EOF
printf 'installer-bin' >"$ov/Applications/SwitchInstaller.app/SwitchInstaller"
chmod 0555 "$ov/Applications/SwitchInstaller.app/SwitchInstaller"
printf 'seal' >"$ov/Applications/SwitchInstaller.app/_CodeSignature/CodeResources"
chmod 0444 "$ov/Applications/SwitchInstaller.app/Info.plist" "$ov/Applications/SwitchInstaller.app/_CodeSignature/CodeResources"

cat >"$ov/usr/share/Switch/Applications/DemoApp.app/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>com.example.demo</string>
  <key>CFBundleExecutable</key><string>DemoApp</string>
  <key>CFBundleVersion</key><string>3.1</string>
</dict></plist>
EOF
cp "$ov/usr/share/Switch/Applications/DemoApp.app/Info.plist" "$ov/System/Library/AppPlaceholders/DemoApp.app/Info.plist"
printf 'demo-bin' >"$ov/usr/share/Switch/Applications/DemoApp.app/DemoApp"
chmod 0555 "$ov/usr/share/Switch/Applications/DemoApp.app/DemoApp"
printf 'demo-seal' >"$ov/usr/share/Switch/Applications/DemoApp.app/_CodeSignature/CodeResources"
ln -s '_CodeSignature' "$ov/usr/share/Switch/Applications/DemoApp.app/linked"
printf 'detached-sig' >"$ov/System/Library/AppSignatures/com.example.demo"

cat >"$work/profile.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>Entitlements</key><dict>
    <key>application-identifier</key><string>ABCDE12345.com.example.demo</string>
  </dict>
</dict></plist>
EOF
/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$work/key.pem" -out "$work/cert.pem" \
    -subj "/CN=Switch W6 Test" -days 1 >/dev/null 2>&1
/usr/bin/openssl cms -sign -nodetach -binary -outform DER \
    -signer "$work/cert.pem" -inkey "$work/key.pem" \
    -in "$work/profile.plist" \
    -out "$ov/usr/share/Switch/Applications/DemoApp.app/embedded.mobileprovision"

printf 'new-shader\n' >"$work/shader_seed/shaders/existing.bin"
printf 'extra-shader\n' >"$work/shader_seed/shaders/new.bin"
chmod 0755 "$work/shader_seed/shaders/existing.bin" "$work/shader_seed/shaders/new.bin"

/usr/bin/python3 - "$work" "$BUILD" "$MANAGED_GEN" <<'PY'
import hashlib, io, json, sys, tarfile
from pathlib import Path

work, build, managed_gen = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
pkg = work / 'pkg'

app = pkg / 'overlay/Applications/SwitchInstaller.app'
files = {}
for p in sorted(app.rglob('*')):
    if p.is_file():
        files[p.relative_to(app).as_posix()] = hashlib.sha256(p.read_bytes()).hexdigest()
(pkg / 'installer-bundle.json').write_text(json.dumps({
    'schema': 1,
    'identifier': 'com.brunostjohn.switch.installer',
    'version': '10',
    'files': files,
}))

status = (
    'Package: org.coolstar.sileo\nStatus: install ok installed\nVersion: 2.5.1\n\n'
    'Package: ellekit\nStatus: install ok installed\nVersion: 1.1\n\n'
    'Package: uikittools\nStatus: install ok installed\nVersion: 2.1\n\n'
    'Package: launchctl\nStatus: install ok installed\nVersion: 1:1.1.1\n\n'
    'Package: firmware\nStatus: install ok installed\nVersion: 15.0\n'
)

def add(tf, name, data=b'', mode=0o644, uid=0, gid=0, kind=tarfile.REGTYPE, link=''):
    ti = tarfile.TarInfo(name)
    ti.type, ti.mode, ti.uid, ti.gid, ti.linkname = kind, mode, uid, gid, link
    ti.size = len(data) if kind == tarfile.REGTYPE else 0
    tf.addfile(ti, io.BytesIO(data) if kind == tarfile.REGTYPE else None)

tar_path = pkg / 'bootstrap/payload.tar'
with tarfile.open(tar_path, 'w') as tf:
    add(tf, '.procursus_strapped', b'')
    add(tf, '.switch-bootstrap-ready', b'')
    add(tf, 'Library/dpkg/status', status.encode())
    add(tf, 'usr/bin/tool', b'bootstrap-tool', mode=0o755)
    add(tf, 'usr/bin/suid', b'suid-tool', mode=0o4755)
    add(tf, 'usr/bin/tool-alias', mode=0o777, kind=tarfile.SYMTYPE, link='tool')
    add(tf, 'usr/bin/hard', mode=0o755, kind=tarfile.LNKTYPE, link='usr/bin/tool')
    add(tf, 'var/cache/apt/.keep', b'', mode=0o644, uid=501, gid=501)

helper = pkg / 'overlay/usr/libexec/switch-bootstrap'
manifest = {
    'schema_version': 1,
    'archive_sha256': hashlib.sha256(tar_path.read_bytes()).hexdigest(),
    'helper_sha256': hashlib.sha256(helper.read_bytes()).hexdigest(),
    'holds': ['org.coolstar.sileo', 'ellekit', 'uikittools', 'launchctl'],
    'managed_generation': managed_gen,
}
(pkg / 'bootstrap/manifest.json').write_text(json.dumps(manifest))
PY

out=$work/w6.img
"$w6_tool" \
    --source "$src" \
    --target "$out" \
    --target-bytes $((2 << 30)) \
    --system-uuid "$system_uuid" \
    --data-uuid "$DATA_UUID" \
    --overlay "$ov" \
    --builtin-pin "$work/pkg/installer-bundle.json" \
    --bootstrap-package "$work/pkg" \
    --shader-seed "$work/shader_seed" \
    --build "$BUILD" \
    --firmware "$FIRMWARE" || die "w6_tool build failed"

fsck_apfs -n "$out" >"$work/fsck.txt" 2>&1
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

check_stat "$m/Switch/overlay" "0:0" "0755"
check_stat "$m/Switch/overlay/usr/libexec/switch-bootstrap" "0:0" "0755"
check_stat "$m/Switch/overlay/etc/switch.conf" "0:0" "0644"
check_stat "$m/Switch/overlay/Applications/SwitchInstaller.app/SwitchInstaller" "0:0" "0755"
[[ ! -e "$m/Switch/overlay/etc/._switch.conf" ]] || fail "AppleDouble etc/._switch.conf leaked"
[[ ! -e "$m/Switch/overlay/._top" ]] || fail "AppleDouble ._top leaked"
got=$(readlink "$m/Switch/overlay/usr/bin/helper-link")
[[ $got == "../libexec/switch-bootstrap" ]] || fail "overlay symlink target mismatch"

check_stat "$m/staged_system_apps" "501:501" "0755"
check_stat "$m/staged_system_apps/DemoApp.app" "501:501" "0755"
check_stat "$m/staged_system_apps/DemoApp.app/DemoApp" "501:501" "0755"
check_stat "$m/staged_system_apps/DemoApp.app/_CodeSignature/CodeResources" "501:501" "0644"
got=$(readlink "$m/staged_system_apps/DemoApp.app/linked")
[[ $got == "_CodeSignature" ]] || fail "staged app symlink target mismatch"
got=$(stat -f '%u:%g' "$m/staged_system_apps/DemoApp.app/linked")
[[ $got == "501:501" ]] || fail "staged app symlink owner mismatch"
[[ -d "$m/staged_system_apps/Maps.app" ]] || fail "existing Maps.app was removed"

check_stat "$m/shaders" "0:0" "0755"
check_stat "$m/shaders/existing.bin" "0:0" "0644"
check_stat "$m/shaders/new.bin" "0:0" "0644"
got=$(cat "$m/shaders/existing.bin")
[[ $got == "new-shader" ]] || fail "existing shader file not replaced"
got=$(cat "$m/shaders/new.bin")
[[ $got == "extra-shader" ]] || fail "new shader file content mismatch"

jb_target=$(readlink "$m/jb")
[[ $jb_target == "/private/var/Switch/bootstrap/$BUILD/root" ]] || fail "jb symlink=$jb_target"
jb_owner=$(stat -f '%u:%g' "$m/jb")
[[ $jb_owner == "0:0" ]] || fail "jb symlink owner mismatch"

bbase="$m/Switch/bootstrap/$BUILD"
broot="$bbase/root"
check_stat "$m/Switch/bootstrap" "0:0" "0755"
check_stat "$bbase" "0:0" "0755"
check_stat "$broot" "0:0" "0755"
check_stat "$bbase/manifest.json" "0:0" "0644"
check_stat "$bbase/metadata.json" "0:0" "0644"
check_stat "$bbase/migration.pending" "0:0" "0644"
check_stat "$broot/usr/bin/tool" "0:0" "0755"
check_stat "$broot/usr/bin/suid" "0:0" "4755"
check_stat "$broot/var/cache/apt/.keep" "501:501" "0644"
got=$(readlink "$broot/usr/bin/tool-alias")
[[ $got == "tool" ]] || fail "bootstrap symlink target mismatch"

ino_tool=$(stat -f '%i' "$broot/usr/bin/tool")
ino_hard=$(stat -f '%i' "$broot/usr/bin/hard")
nlink_tool=$(stat -f '%l' "$broot/usr/bin/tool")
[[ $ino_tool == "$ino_hard" && $nlink_tool == 2 ]] \
    || fail "hard link mismatch: tool ino=$ino_tool hard ino=$ino_hard nlink=$nlink_tool"

got=$(cat "$bbase/migration.pending")
[[ $got == "$MANAGED_GEN" ]] || fail "migration.pending content mismatch"
[[ ! -e "$m/Switch/system-app-migration" ]] || fail "Switch/system-app-migration must not be created"
[[ ! -e "$m/datamigrator/migrate" ]] || fail "datamigrator/migrate must not be created"

grep -q '"build": "21H461"' "$bbase/manifest.json" || fail "manifest.json missing build"
grep -q '"firmware": "17.7.11"' "$bbase/manifest.json" || fail "manifest.json missing firmware"
grep -q '"path": "usr/bin/suid"' "$bbase/metadata.json" || fail "metadata.json missing usr/bin/suid"
grep -q '^Version: 17.7.11$' "$broot/Library/dpkg/status" || fail "firmware version missing in dpkg/status"
held_count=$(grep -c '^Status: hold ok installed$' "$broot/Library/dpkg/status")
[[ $held_count == 5 ]] || fail "expected 5 held packages (4 core + firmware) in dpkg/status"

diskutil unmount "$work/mnt" >/dev/null
detach_all

say "failures: $failures"
[[ $failures == 0 ]]
