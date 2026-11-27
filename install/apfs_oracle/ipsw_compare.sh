#!/bin/bash
# Manual EC2 comparison runner: validates a Rust-built 21H461 or 24B119 container and deep-diffs all 3 volumes
# against a Mac //:image container. Never writes Apple/IPSW dumps inside a git repository.
#
# Usage:
#   bazel run //install/apfs_oracle:ipsw_compare -- \
#       --kind 24B119 \
#       --rust-image /path/to/rust.img [--rust-offset 0] \
#       --mac-image /path/to/mac.img [--mac-offset 0] \
#       [--work-dir /tmp/ipsw-compare] [--keep-dumps]
set -o pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
R=/System/Library/Filesystems/apfs.fs/Contents/Resources

DATA_UUID=D1B1D053-D0A6-496C-8625-A947A3DA863E
PREBOOT_UUID=4AA4A761-5138-439D-9A30-D4138F6887FA
DATA_NAME=Switch-iOS-Data
PREBOOT_NAME=Switch-iOS-Preboot

script_dir=$(cd "$(dirname "$0")" && pwd)
tool=$script_dir/switch_apfs_oracle_tool
reader=$script_dir/../apfs_read/switch_apfs_read_tool
apfs_tool=$script_dir/../../boot/installer/apfs_tool
tree_diff=$script_dir/tree_diff.py
images_sh=$script_dir/../apfs_m1/images.sh

usage() {
    cat <<'EOF'
Usage: ipsw_compare.sh --kind <21H461|24B119> --rust-image <PATH> --mac-image <PATH> [options]

Options:
  --rust-offset <BYTES>   APFS container byte offset in --rust-image (default: 0)
  --mac-offset <BYTES>    APFS container byte offset in --mac-image (default: 0)
  --system-uuid <UUID>    Override pinned System UUID
  --data-uuid <UUID>      Override pinned Data UUID
  --preboot-uuid <UUID>   Override pinned Preboot UUID
  --work-dir <DIR>        Scratch directory outside any git repo (default: mktemp under /tmp)
  --keep-dumps            Keep JSON dumps in --work-dir after exit
EOF
}

kind=
rust_img=
mac_img=
rust_offset=0
mac_offset=0
system_uuid=
work=
keep_dumps=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --kind)
            kind=$2
            shift 2
            ;;
        --rust-image)
            rust_img=$2
            shift 2
            ;;
        --mac-image)
            mac_img=$2
            shift 2
            ;;
        --rust-offset)
            rust_offset=$2
            shift 2
            ;;
        --mac-offset)
            mac_offset=$2
            shift 2
            ;;
        --system-uuid)
            system_uuid=$2
            shift 2
            ;;
        --data-uuid)
            DATA_UUID=$2
            shift 2
            ;;
        --preboot-uuid)
            PREBOOT_UUID=$2
            shift 2
            ;;
        --work-dir)
            work=$2
            shift 2
            ;;
        --keep-dumps)
            keep_dumps=1
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            printf 'unknown option: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

[[ -n $kind && -n $rust_img && -n $mac_img ]] || {
    usage >&2
    exit 2
}

if [[ -z $system_uuid ]]; then
    case "$kind" in
        21H461) system_uuid=04769D26-2CA0-4825-B443-E2E5F88300F2 ;;
        24B119) system_uuid=D05A9D99-D227-4757-9F58-C32593D3BFA7 ;;
        *)
            printf 'unsupported --kind %s (expected 21H461 or 24B119)\n' "$kind" >&2
            exit 2
            ;;
    esac
fi

failures=0
vols=()

# shellcheck source=/dev/null
source "$images_sh"

if [[ -z $work ]]; then
    work=$(mktemp -d /tmp/switch_ipsw_compare.XXXXXX)
    owned_work=1
else
    mkdir -p "$work"
    owned_work=0
fi

# Never allow Apple/IPSW dumps inside a git repository.
if git -C "$work" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    die "work directory $work is inside a git repository; pass a path under /tmp"
fi

cleanup() {
    local m
    for m in rust-sys rust-data rust-boot mac-sys mac-data mac-boot; do
        diskutil unmount force "$work/mnt/$m" >/dev/null 2>&1
    done
    detach_all
    if [[ $keep_dumps == 0 && $owned_work == 1 && $work == /tmp/switch_ipsw_compare.* && -d $work ]]; then
        sudo -n rm -rf -- "$work"
    fi
}
trap cleanup EXIT

sudo -n true || die "passwordless sudo is required"
mkdir -p "$work/mnt"/{rust-sys,rust-data,rust-boot,mac-sys,mac-data,mac-boot} "$work/extracted"

say "== validate Rust-built $kind container ($rust_img offset $rust_offset)"
"$tool" verify-pins \
    --image "$rust_img" \
    --offset "$rust_offset" \
    --system-uuid "$system_uuid" \
    --data-uuid "$DATA_UUID" \
    --data-name "$DATA_NAME" \
    --preboot-uuid "$PREBOOT_UUID" \
    --preboot-name "$PREBOOT_NAME" || fail "verify-pins"

"$apfs_tool" "$rust_img" "$rust_offset" || fail "apfs_tool"
"$reader" audit "$rust_img" --offset "$rust_offset" || fail "allocation audit"

if [[ $rust_offset == 0 ]]; then
    fsck_clean "$rust_img" "$work/fsck-rust.txt" 4 || {
        cat "$work/fsck-rust.txt"
        fail "fsck_apfs -n on Rust image"
    }
fi

"$tool" verify-preboot-extents \
    --image "$rust_img" \
    --offset "$rust_offset" \
    --preboot-uuid "$PREBOOT_UUID" \
    --extract-dir "$work/extracted" \
    --expect-count 2 || fail "verify-preboot-extents"

if [[ $kind == 24B119 ]]; then
    "$tool" verify-index \
        --image "$rust_img" \
        --offset "$rust_offset" \
        --system-uuid "$system_uuid" \
        --data-uuid "$DATA_UUID" || fail "verify-index"
    for dmg in "$work"/extracted/*.dmg; do
        fsck_clean "$dmg" "$work/fsck-$(basename "$dmg").txt" 2 || fail "fsck $(basename "$dmg")"
        attach "$dmg" -readonly || {
            fail "attach $(basename "$dmg")"
            continue
        }
        "$R/apfs_checkseal" "/dev/${vols[0]}" || fail "apfs_checkseal $(basename "$dmg")"
        detach_all
    done
fi
rm -rf -- "$work/extracted"

mount_container() {
    local label=$1 img=$2 sys_s data_s boot_s
    attach "$img" -readonly || die "attach $label ($img)"
    sys_s=$(volume_slice "$system_uuid") || die "$label: missing System UUID $system_uuid"
    data_s=$(volume_slice "$DATA_UUID") || die "$label: missing Data UUID $DATA_UUID"
    boot_s=$(volume_slice "$PREBOOT_UUID") || die "$label: missing Preboot UUID $PREBOOT_UUID"
    diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt/$label-sys" "$sys_s" >/dev/null \
        || die "mount $label System"
    diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt/$label-data" "$data_s" >/dev/null \
        || die "mount $label Data"
    diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "$work/mnt/$label-boot" "$boot_s" >/dev/null \
        || die "mount $label Preboot"
}

if [[ $rust_offset != 0 || $mac_offset != 0 ]]; then
    say "NOTE: hdiutil attaches whole images; ensure offsets match partition layout"
fi

say "== mount and dump both containers"
mount_container rust "$rust_img"
mount_container mac "$mac_img"

for vol in sys data boot; do
    sudo -n /usr/bin/python3 -I "$tree_diff" dump "$work/mnt/rust-$vol" --out "$work/rust-$vol.json" \
        || fail "dump rust-$vol"
    sudo -n /usr/bin/python3 -I "$tree_diff" dump "$work/mnt/mac-$vol" --out "$work/mac-$vol.json" \
        || fail "dump mac-$vol"
done

say "== deep diff (Mac //:image vs Rust)"
/usr/bin/python3 -I "$tree_diff" diff "$work/mac-sys.json" "$work/rust-sys.json" --compare-mtime \
    || fail "System diff"
/usr/bin/python3 -I "$tree_diff" diff "$work/mac-data.json" "$work/rust-data.json" \
    --ignore-path /.fseventsd || fail "Switch-iOS-Data diff"
/usr/bin/python3 -I "$tree_diff" diff "$work/mac-boot.json" "$work/rust-boot.json" \
    --ignore-path /.fseventsd --ignore-path /Cryptexes || fail "Switch-iOS-Preboot diff"

say "failures: $failures"
[[ $failures == 0 ]]
