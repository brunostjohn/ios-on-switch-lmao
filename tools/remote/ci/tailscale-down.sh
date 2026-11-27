#!/usr/bin/env bash
# Logs out of Tailscale and stops the ephemeral tailscaled process:
#   tailscale-down.sh [state_dir]
set -euo pipefail

state_dir=${1:-${RUNNER_TEMP:-/tmp}/ts}
sock="$state_dir/tailscaled.sock"
pid_file="$state_dir/tailscaled.pid"

tailscale_bin=""
if [[ -f "$state_dir/tailscale.bin" ]]; then
    tailscale_bin=$(cat "$state_dir/tailscale.bin")
elif command -v tailscale >/dev/null 2>&1; then
    tailscale_bin=$(command -v tailscale)
fi

if [[ -n "$tailscale_bin" && -S "$sock" ]]; then
    echo "=== tailscale status ===" >&2
    sudo "$tailscale_bin" --socket="$sock" status >&2 || true
    sudo "$tailscale_bin" --socket="$sock" logout >/dev/null 2>&1 || true
fi

if [[ -f "$pid_file" ]]; then
    pid=$(cat "$pid_file")
    if [[ -n "$pid" ]]; then
        sudo kill -TERM "$pid" 2>/dev/null || true
        sleep 1
        sudo kill -KILL "$pid" 2>/dev/null || true
    fi
fi
