#!/bin/bash
# What the installer refuses, on 24B119's AppOS cryptex: each run below must fail for its own reason and leave no
# output behind.
#   - a wrong pin of the trust module: the pins refuse it;
#   - a corrupted member, one byte of a Mach-O's stored data flipped in a copy: the seal or decmpfs refuses it;
#   - a translator whose result differs from the pinned one (:switch_translate_tool_perturbed): the pins refuse it.
# The same run with nothing changed must pass.
#
#   refusal_test.sh TOOL PERTURBED_TOOL IMAGE TRUSTCACHE PINS CLI
set -uo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
tool=$PWD/$1
perturbed=$PWD/$2
image=$PWD/$3
trust=$PWD/$4
pins=$PWD/$5
cli=$PWD/$6
work=${TEST_TMPDIR:?}
failures=0

run() {
    "$1" role --role app_cryptex --apple-trustcache "$trust" --translator-of "$cli" --image "$2" --pins "$3" \
        --out "$4"
}

# refused WHAT NAME REASON TOOL IMAGE PINS: REASON is an extended regular expression the refusal must match.
refused() {
    local what=$1 out=$work/$2 reason=$3 last
    shift 3
    if run "$@" "$out" >"$out.log" 2>&1; then
        echo "FAIL: $what was accepted"
        failures=$((failures + 1))
    elif [[ -e $out || -e $out.partial ]]; then
        echo "FAIL: $what left output behind"
        failures=$((failures + 1))
    else
        last=$(tail -1 "$out.log")
        if [[ $last =~ $reason ]]; then
            echo "refused, $what: $last"
        else
            echo "FAIL: $what was refused for another reason: $last"
            failures=$((failures + 1))
        fi
    fi
}

if ! run "$tool" "$image" "$pins" "$work/accepted" >"$work/accepted.log" 2>&1; then
    cat "$work/accepted.log"
    echo "FAIL: the unchanged run was refused"
    exit 1
fi
accepted=$(tail -1 "$work/accepted.log")
echo "accepted, unchanged: $accepted"

zeros=$(printf '0%.0s' {1..64})
sed "/app_cryptex.trustcache/s/\"[0-9a-f]\{64\}\"/\"$zeros\"/" "$pins" >"$work/wrong-pins.json"
if cmp -s "$pins" "$work/wrong-pins.json"; then
    echo "FAIL: the pins could not be changed"
    exit 1
fi
refused "a wrong pin" wrong-pin "app_cryptex.trustcache is [0-9a-f]{64}, the release pins 0{64}" \
    "$tool" "$image" "$work/wrong-pins.json"

# The member is a read-only Bazel output, and cp keeps its mode.
cp "$image" "$work/corrupt.img"
chmod u+w "$work/corrupt.img"
located=$("$tool" locate --image "$work/corrupt.img") || exit 1
read -r offset path <<<"$located"
byte_at() {
    local b
    b=$(dd if="$work/corrupt.img" bs=1 skip="$offset" count=1 2>/dev/null | od -An -tu1)
    echo "${b// /}"
}
byte=$(byte_at)
octal=$(printf %03o $((255 - byte)))
printf '%b' "\\0$octal" | dd of="$work/corrupt.img" bs=1 seek="$offset" conv=notrunc 2>/dev/null
now=$(byte_at)
if [[ $now != $((255 - byte)) ]]; then
    echo "FAIL: byte $offset of the copy is $now, not $((255 - byte))"
    exit 1
fi
echo "corrupted byte $offset ($path): $byte -> $now"
refused "a corrupted member" corrupt "$path: .*(fail the volume's seal|decmpfs)" \
    "$tool" "$work/corrupt.img" "$pins"
rm -f -- "$work/corrupt.img"

refused "a translator whose result differs" perturbed \
    "app_cryptex.trustcache is [0-9a-f]{64}, the release pins [0-9a-f]{64}" "$perturbed" "$image" "$pins"

exit $((failures > 0))
