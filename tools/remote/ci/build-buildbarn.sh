#!/usr/bin/env bash
# Builds the four patched macOS arm64 Buildbarn binaries for ephemeral CI clusters:
#   build-buildbarn.sh <output_dir>
set -euo pipefail

out_dir=${1:?usage: build-buildbarn.sh <output_dir>}

BB_STORAGE_COMMIT="ca3d8b49a5af52eb02ed473dd689d19b949c9e41"
BB_REMOTE_EXECUTION_COMMIT="c9ba98cf52456c400278f45a9679635daa40dcd2"
GO_VERSION="1.27.3"

script_dir=$(cd "$(dirname "$0")" && pwd)
remote_dir=$(cd "$script_dir/.." && pwd)
clonefile_patch="$remote_dir/bb_worker-clonefile.diff"
slots_patch="$remote_dir/bb_worker-slots.diff"

mkdir -p "$out_dir"
out_dir=$(cd "$out_dir" && pwd)

work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/buildbarn-build.XXXXXX")
cleanup() {
    chmod -R u+w "$work" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

ensure_go() {
    local current=""
    if command -v go >/dev/null 2>&1; then
        current=$(go env GOVERSION 2>/dev/null || true)
    fi
    if [[ "$current" == "go${GO_VERSION}" ]]; then
        return 0
    fi
    local archive="$work/go.tar.gz"
    echo "Downloading Go ${GO_VERSION} for darwin/arm64..." >&2
    curl -fsSL --retry 3 "https://go.dev/dl/go${GO_VERSION}.darwin-arm64.tar.gz" -o "$archive"
    tar -xzf "$archive" -C "$work"
    export GOROOT="$work/go"
    export PATH="$GOROOT/bin:$PATH"
}

fetch_commit() {
    local url=$1
    local commit=$2
    local dest=$3
    mkdir -p "$dest"
    git -C "$dest" init -q
    git -C "$dest" remote add origin "$url"
    git -C "$dest" fetch --depth 1 -q origin "$commit"
    git -C "$dest" checkout -q --detach FETCH_HEAD
}

go_mod_dir() {
    local repo=$1
    local module=$2
    (
        cd "$repo"
        go mod download -json "$module"
    ) | python3 -c 'import json, sys; print(json.load(sys.stdin)["Dir"])'
}

ensure_go
export GOPATH="$work/gopath"
export GOMODCACHE="$work/gomodcache"
export GOCACHE="$work/gocache"
export GOTOOLCHAIN="local"
export CGO_ENABLED=1
export GOOS=darwin
export GOARCH=arm64

go_ver=$(go version)
echo "Using $go_ver" >&2

bb_storage="$work/bb-storage"
bb_re="$work/bb-remote-execution"
fetch_commit "https://github.com/buildbarn/bb-storage.git" "$BB_STORAGE_COMMIT" "$bb_storage"
fetch_commit "https://github.com/buildbarn/bb-remote-execution.git" "$BB_REMOTE_EXECUTION_COMMIT" "$bb_re"

xsys_src=$(go_mod_dir "$bb_storage" "golang.org/x/sys")
bytestream_src=$(go_mod_dir "$bb_storage" "google.golang.org/genproto/googleapis/bytestream")

cp -R "$xsys_src" "$work/xsys"
chmod -R u+w "$work/xsys"
patch -p0 -d "$work/xsys" <"$bb_storage/patches/org_golang_x_sys/o-search.diff"

cp -R "$bytestream_src" "$work/bytestream"
chmod -R u+w "$work/bytestream"
patch -p0 -d "$work/bytestream" <"$bb_storage/patches/org_golang_google_genproto_googleapis_bytestream/service-registrar.diff"

for repo in "$bb_storage" "$bb_re"; do
    (
        cd "$repo"
        go mod edit \
            -replace="golang.org/x/sys=$work/xsys" \
            -replace="google.golang.org/genproto/googleapis/bytestream=$work/bytestream"
    )
done

git -C "$bb_re" apply "$clonefile_patch"
git -C "$bb_re" apply "$slots_patch"

echo "Building bb_storage..." >&2
(cd "$bb_storage" && go build -trimpath -o "$out_dir/bb_storage" ./cmd/bb_storage)

for cmd in bb_scheduler bb_worker bb_runner; do
    echo "Building $cmd..." >&2
    (cd "$bb_re" && go build -trimpath -o "$out_dir/$cmd" "./cmd/$cmd")
done

chmod +x "$out_dir/bb_storage" "$out_dir/bb_scheduler" "$out_dir/bb_worker" "$out_dir/bb_runner"
shasum -a 256 "$out_dir/bb_storage" "$out_dir/bb_scheduler" "$out_dir/bb_worker" "$out_dir/bb_runner"
