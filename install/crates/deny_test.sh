#!/bin/bash
# cargo-deny's licence and source checks over Cargo.lock, offline: crates.io is replaced by @installer_vendor. The paths
# come from the test's env (BUILD.bazel), relative to the runfiles root.
set -euo pipefail

root=$PWD
vendor=$root/$(dirname "${VENDOR_LIST:?}")/vendor
export HOME=${TEST_TMPDIR:?}
export CARGO_HOME=$HOME/cargo-home
export CARGO=$root/${CARGO_BIN:?}
export RUSTC=$root/${RUSTC_BIN:?}
mkdir -p "$CARGO_HOME"
cat >"$CARGO_HOME/config.toml" <<EOF
[source.crates-io]
replace-with = "installer-vendor"

[source.installer-vendor]
directory = "$vendor"
EOF

exec "$root/${CARGO_DENY:?}" --color never --frozen --manifest-path "$root/${MANIFEST:?}" \
    --config "$root/${DENY_CONFIG:?}" check licenses sources
