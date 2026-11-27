#!/bin/bash
# For each size from 1 GiB to 1 TiB, odd block counts and both sides of every step in the size rules included:
#  - the geometry switch_apfs_alloc gives must equal what newfs_apfs makes at that size, and what
#    `diskutil apfs resizeContainer` makes when it grows a small container to it;
#  - our writer's container at that size, with those checkpoint areas, must read back with that geometry, pass
#    apfs_tool's measure and fsck_apfs, attach both volumes, and mount Data (reading /hello) and System.
# Negative controls: a main or pool free-queue limit one past the rule attaches no volume, though fsck passes it.
# Every image is a sparse file, deleted once checked; only disks this script attached are detached.
set -o pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
sweep=$PWD/$1
tool=$PWD/$2
apfs_tool=$PWD/$3
work=$(mktemp -d /tmp/apfs_sweep.XXXXXX)
DATA_UUID=D1B1D053-D0A6-496C-8625-A947A3DA863E
TIME_NS=1795000000000000000
START_BLOCKS=65536
SIZES=(
    262144 393217 1048575 1048576 2097153 2213376 4128768 4128769 4980736 5255225 6291457 7200178
    12288000 12288001 12976128 12976129 36929537 67108867 134217727 268435455 268435456
)
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
    plist=$(hdiutil attach -plist -nomount -imagekey diskimage-class=CRawDiskImage "$@" "${image}" 2>/dev/null) || return 1
    disk=$(printf '%s\n' "${plist}" | grep -o '/dev/disk[0-9]*' | head -1 | cut -c6-)
    attached+=("${disk}")
    found=$(printf '%s\n' "${plist}" | grep -o '/dev/disk[0-9]*s[0-9]*' | cut -c6- | sort -u)
    while IFS= read -r s; do
        if [[ -n ${s} ]]; then
            vols+=("${s}")
        fi
    done <<<"${found}"
    return 0
}

detach_all() {
    local d
    for d in ${attached[@]+"${attached[@]}"}; do
        hdiutil detach -force "/dev/${d}" >/dev/null 2>&1
    done
    attached=()
}

cleanup() {
    detach_all
    if [[ ${work} == /tmp/apfs_sweep.* && -d ${work} ]]; then
        rm -rf -- "${work}"
    fi
}
trap cleanup EXIT

drop() {
    if [[ $1 == "${work}"/* && -f $1 ]]; then
        rm -f -- "$1"
    fi
}

volume_slice() {
    local s
    for s in ${vols[@]+"${vols[@]}"}; do
        if diskutil info "${s}" | grep -q "Volume UUID: *$1"; then
            say "${s}"
            return 0
        fi
    done
    return 1
}

fsck_clean() {
    local image=$1 log=$2 want=$3 rc oks bad
    fsck_apfs -n "${image}" >"${log}" 2>&1
    rc=$?
    oks=$(grep -c 'appears to be OK' "${log}")
    bad=$(grep -iE '^(warning|error)|corrupt|invalid|fail' "${log}" | grep -v 'not a valid disk name/path')
    [[ ${rc} == 0 && ${oks} == "${want}" && -z ${bad} ]]
}

newfs_geometry() {
    local blocks=$1 image=${work}/newfs.img
    truncate -s $((blocks * 4096)) "${image}"
    attach "${image}" || return 1
    newfs_apfs -e -v Sys "/dev/${disk}" >/dev/null || return 1
    detach_all
    "${sweep}" read "${image}"
    drop "${image}"
}

# A newfs_apfs container of START_BLOCKS in a GPT partition, grown to `blocks`.
resized_geometry() {
    local blocks=$1 image=${work}/resize.img offset=$((1 << 20))
    truncate -s $((offset + blocks * 4096 + (1 << 20))) "${image}"
    "${sweep}" gpt "${image}" --first $((offset / 512)) --sectors $((START_BLOCKS * 8)) || return 1
    attach "${image}" || return 1
    newfs_apfs -e -v Sys "/dev/${disk}s1" >/dev/null || return 1
    diskutil apfs resizeContainer "${disk}s1" "$((blocks * 4096))B" >"${work}/resize.log" 2>&1 || return 1
    detach_all
    "${sweep}" read "${image}" --offset "${offset}"
    drop "${image}"
}

mounts() {
    local name=$1 data system st hello
    data=$(volume_slice "${DATA_UUID}")
    system=$(volume_slice "${system_uuid}")
    if [[ ${#vols[@]} != 2 || -z ${data} || -z ${system} ]]; then
        fail "${name}: both volumes attach (${vols[*]})"
        return
    fi
    if diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "${work}/mnt" "${data}" >/dev/null; then
        st=$(stat -f '%u:%g %Sp %z' "${work}/mnt/hello" 2>&1)
        hello=$(cat "${work}/mnt/hello")
        [[ ${st} == "0:0 -rw-r--r-- 6" && ${hello} == hello ]] || fail "${name}: /hello reads ${st} ${hello}"
        diskutil unmount "${work}/mnt" >/dev/null
    else
        fail "${name}: mount Data"
    fi
    if diskutil mount readOnly -mountOptions nobrowse,owners -mountPoint "${work}/mnt" "${system}" >/dev/null; then
        [[ -f ${work}/mnt/marker ]] || fail "${name}: System's /marker"
        diskutil unmount "${work}/mnt" >/dev/null
    else
        fail "${name}: mount System"
    fi
}

control() {
    local image=$1 queue=$2 copy=${work}/control.img
    cp -c "${image}" "${copy}"
    "${sweep}" bump-limit "${copy}" "${queue}" >/dev/null || die "bump-limit ${queue}"
    if ! fsck_clean "${copy}" "${work}/fsck-control.txt" 3; then
        say "control ${queue} limit +1: fsck_apfs rejects it"
    elif attach "${copy}" -readonly; then
        say "control ${queue} limit +1: fsck passes; ${#vols[@]} volumes attach"
        [[ ${#vols[@]} == 0 ]] || fail "a ${queue} queue limit one past the rule still attached volumes"
        detach_all
    else
        fail "control ${queue}: attach"
    fi
    drop "${copy}"
}

check_size() {
    local blocks=$1 want got desc data apfs_measure measured_blocks measured_pool out=${work}/ours.img
    want=$("${sweep}" rule "${blocks}") || die "rule ${blocks}"
    say "== ${want}"
    got=$(newfs_geometry "${blocks}") || got="newfs_apfs failed"
    say "newfs_apfs: ${got}"
    [[ ${got} == "${want}" ]] || fail "${blocks}: newfs_apfs differs"
    detach_all
    got=$(resized_geometry "${blocks}") || got="resizeContainer failed: $(tail -2 "${work}/resize.log" | tr '\n' ' ')"
    say "resizeContainer from ${START_BLOCKS}: ${got}"
    [[ ${got} == "${want}" ]] || fail "${blocks}: resizeContainer differs"
    detach_all
    desc=$(printf '%s\n' "${want}" | sed -n 's/.* desc=\([0-9]*\) .*/\1/p')
    data=$(printf '%s\n' "${want}" | sed -n 's/.* data=\([0-9]*\) .*/\1/p')
    if ! "${tool}" add-volume --source "${src}" --target "${out}" --target-bytes $((blocks * 4096)) \
        --system-uuid "${system_uuid}" --uuid "${DATA_UUID}" --name Switch-iOS-Data --role 0x40 --features 0x2 \
        --rocompat 0x0c --incompat 0x200 --unicode 17.0 --areas "${desc}:${data}" --time-ns "${TIME_NS}" >/dev/null; then
        fail "${blocks}: add-volume"
        return
    fi
    got=$("${sweep}" read "${out}")
    [[ ${got} == "${want}" ]] || fail "${blocks}: our container differs"
    apfs_measure=$("${apfs_tool}" "${out}" 0) || fail "${blocks}: apfs_tool can't measure ours"
    read -r measured_blocks _ _ measured_pool <<<"${apfs_measure}"
    [[ ${measured_blocks} == "${blocks}" && ${want} == *" pool=${measured_pool} "* ]] \
        || fail "${blocks}: apfs_tool measures ${apfs_measure}"
    if ! fsck_clean "${out}" "${work}/fsck.txt" 3; then
        grep -iE 'error|warning|appears' "${work}/fsck.txt" | head -5
        fail "${blocks}: fsck_apfs -n"
        drop "${out}"
        return
    fi
    if attach "${out}" -readonly; then
        mounts "${blocks}"
    else
        fail "${blocks}: attach"
    fi
    detach_all
    say "ours: ${got}; apfs_tool ${apfs_measure}; fsck clean; attached ${#vols[@]}, mounted"
    case ${blocks} in
        4980736) control "${out}" main ;;
        12288001 | 268435456) control "${out}" pool ;;
        *) ;;
    esac
    drop "${out}"
}

host=$(hostname)
macos=$(sw_vers -productVersion)
say "host=${host} macos=${macos} work=${work}"
mkdir -p "${work}/mnt"
src=${work}/source.img
truncate -s $((START_BLOCKS * 4096)) "${src}"
attach "${src}" || die "attach the blank source"
newfs_apfs -e -v System "/dev/${disk}" >/dev/null || die "newfs_apfs the source"
detach_all
attach "${src}" || die "attach the source"
diskutil mount -mountOptions nobrowse -mountPoint "${work}/mnt" "${vols[0]}" >/dev/null || die "mount the source"
say marker >"${work}/mnt/marker"
diskutil unmount "${work}/mnt" >/dev/null || die "unmount the source"
detach_all
system_uuid=$("${tool}" inspect "${src}" | sed -n 's/^volume 0 .* uuid \([0-9A-F-]*\) .*/\1/p')
[[ -n ${system_uuid} ]] || die "inspect the source"
"${tool}" set-role "${src}" --uuid "${system_uuid}" --role 1 >/dev/null || die "set-role"

for blocks in "${SIZES[@]}"; do
    check_size "${blocks}"
done

say "failures: ${failures}"
[[ ${failures} == 0 ]]
