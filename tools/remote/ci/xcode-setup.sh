#!/usr/bin/env bash
# Configures the pinned Xcode and Metal toolchain on a CI macOS runner, and writes worker env vars to a file:
#   xcode-setup.sh <repo_root> <env_file>
set -euo pipefail

repo_root=${1:?usage: xcode-setup.sh <repo_root> <env_file>}
env_file=${2:?}

xcode_bzl="$repo_root/bazel/host/xcode.bzl"
pinned_build=$(sed -n 's/^    "build": "\(.*\)",$/\1/p' "$xcode_bzl")
if [[ -z "$pinned_build" ]]; then
    echo "xcode-setup: could not read pinned Xcode build from $xcode_bzl" >&2
    exit 1
fi

developer_dir=""
xcode_app=""
for app in /Applications/Xcode*.app; do
    [[ -f "$app/Contents/version.plist" ]] || continue
    build=$(/usr/libexec/PlistBuddy -c 'Print :ProductBuildVersion' "$app/Contents/version.plist" 2>/dev/null || true)
    if [[ "$build" == "$pinned_build" ]]; then
        xcode_app=$(cd "$app" && pwd -P)
        developer_dir="$xcode_app/Contents/Developer"
        break
    fi
done

if [[ -z "$developer_dir" ]]; then
    echo "xcode-setup: no Xcode matching build $pinned_build found in /Applications" >&2
    exit 1
fi

sudo xcode-select -s "$developer_dir"

lsregister="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
if [[ -x "$lsregister" ]]; then
    "$lsregister" -f -R "$xcode_app" >/dev/null 2>&1 || true
fi

export DEVELOPER_DIR="$developer_dir"
if ! xcrun -sdk iphoneos metal -v >/dev/null 2>&1; then
    xcodebuild -downloadComponent MetalToolchain
fi

version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$xcode_app/Contents/version.plist")
case "$version" in
    *.*.*) ;;
    *.*) version="${version}.0" ;;
    *) version="${version}.0.0" ;;
esac

gpu=$(/usr/sbin/sysctl -n machdep.cpu.brand_string 2>/dev/null || true)

mkdir -p "$(dirname "$env_file")"
{
    printf 'DEVELOPER_DIR=%q\n' "$developer_dir"
    printf 'SWITCH_WORKER_DEVELOPER_DIR=%q\n' "$developer_dir"
    printf 'SWITCH_WORKER_XCODE_BUILD=%q\n' "$pinned_build"
    printf 'SWITCH_WORKER_XCODE_VERSION=%q\n' "${version}.${pinned_build}"
    printf 'SWITCH_WORKER_GPU=%q\n' "$gpu"
} >"$env_file"

echo "xcode-setup: selected $developer_dir ($version.$pinned_build, gpu=${gpu:-none})" >&2
