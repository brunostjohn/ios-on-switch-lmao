#!/bin/bash
# One role's delta from switch_translate_tool against translate_tree.py's for the same IPSW, file for file. Only the
# reference's .work/ (translate_tree's resume records) has no counterpart.
#
#   delta_test.sh TOOL REFERENCE ROLE-ARGS...
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
tool=$PWD/$1
reference=$PWD/$2
shift 2
out=${TEST_TMPDIR:?}/delta
"$tool" role "$@" --out "$out"
diff -rq -x .work "$reference" "$out"
count=$(find "$out" -type f | wc -l)
echo "byte-equal to ${reference##*/}: ${count// /} files"
for f in "$out"/*.trustcache "$out"/*.manifest.json "$out"/*.a64xplan "$out"/a64x-cache-pretranslated.bin; do
    if [[ -f $f ]]; then
        shasum -a 256 "$f" | sed "s|$out/||"
    fi
done
