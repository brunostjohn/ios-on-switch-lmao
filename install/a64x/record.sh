#!/bin/sh
# The arm64e CLI ($SWITCH_A64X_CLI), run as it is, each call kept with its inputs and outputs in
# $SWITCH_A64X_RECORD/<call>-<command>/.
set -eu
record=${SWITCH_A64X_RECORD:?}
cli=${SWITCH_A64X_CLI:?}
calls=0
for call in "$record"/*; do
    if [ -e "$call" ]; then
        calls=$((calls + 1))
    fi
done
dir=$record/$(printf '%03d' "$calls")-$1
/bin/mkdir "$dir"
case $1 in
    kext)
        /bin/cp "$2" "$dir/blob"
        /bin/cp "$4" "$dir/jobs"
        printf '%s\n%s\n' "$3" "$5" >"$dir/vas"
        ;;
    fips)
        shift
        i=0
        for header in "$@"; do
            /bin/cp "$header" "$dir/header.$i"
            i=$((i + 1))
        done
        set -- fips "$@"
        ;;
    fips-mac)
        /bin/cp "$2" "$dir/original"
        /bin/cp "$3" "$dir/translated"
        printf '%s\n' "$4" >"$dir/mac"
        ;;
    *) ;;
esac
status=0
"$cli" "$@" >"$dir/stdout" || status=$?
if [ "$1" = kext ] && [ "$status" = 0 ]; then
    /bin/cp "$6" "$dir/out_blob"
    /bin/cp "$7" "$dir/out_stubs"
fi
/bin/cat "$dir/stdout"
exit "$status"
