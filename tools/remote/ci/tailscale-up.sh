#!/usr/bin/env bash
# Joins the ephemeral tailnet as tag:gha-ci using Tailscale OAuth credentials:
#   tailscale-up.sh <hostname> [state_dir]
set -euo pipefail

hostname=${1:?usage: tailscale-up.sh <hostname> [state_dir]}
state_dir=${2:-${RUNNER_TEMP:-/tmp}/ts}
tag=${TS_TAG:-tag:gha-ci}

: "${TS_OAUTH_CLIENT_ID:?TS_OAUTH_CLIENT_ID must be set}"
: "${TS_OAUTH_SECRET:?TS_OAUTH_SECRET must be set}"

mkdir -p "$state_dir"
sock="$state_dir/tailscaled.sock"
log="$state_dir/tailscaled.log"
pid_file="$state_dir/tailscaled.pid"
key_file="$state_dir/auth.key"

cleanup_key() {
    rm -f "$key_file"
}
trap cleanup_key EXIT

find_bin() {
    local name=$1
    local candidate
    for candidate in \
        "/opt/homebrew/opt/tailscale/bin/$name" \
        "/opt/homebrew/bin/$name" \
        "/usr/local/bin/$name"; do
        if [[ -x "$candidate" ]]; then
            echo "$candidate"
            return 0
        fi
    done
    command -v "$name" 2>/dev/null || true
}

tailscale_bin=$(find_bin tailscale)
tailscaled_bin=$(find_bin tailscaled)
if [[ -z "$tailscale_bin" || -z "$tailscaled_bin" ]]; then
    echo "Installing tailscale via Homebrew..." >&2
    HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1 brew install --quiet tailscale
    tailscale_bin=$(find_bin tailscale)
    tailscaled_bin=$(find_bin tailscaled)
fi
if [[ -z "$tailscale_bin" || -z "$tailscaled_bin" ]]; then
    echo "tailscale-up: tailscale or tailscaled not found after install" >&2
    exit 1
fi

printf '%s\n' "$tailscale_bin" >"$state_dir/tailscale.bin"

umask 077
TS_TAG="$tag" TS_HOSTNAME="$hostname" TS_KEY_FILE="$key_file" python3 - <<'PY'
import json
import os
import urllib.parse
import urllib.request

client_id = os.environ["TS_OAUTH_CLIENT_ID"]
client_secret = os.environ["TS_OAUTH_SECRET"]
tag = os.environ["TS_TAG"]
hostname = os.environ["TS_HOSTNAME"]
key_file = os.environ["TS_KEY_FILE"]

token_body = urllib.parse.urlencode(
    {
        "client_id": client_id,
        "client_secret": client_secret,
    }
).encode("utf-8")
token_req = urllib.request.Request(
    "https://api.tailscale.com/api/v2/oauth/token",
    data=token_body,
    headers={"Content-Type": "application/x-www-form-urlencoded"},
    method="POST",
)
with urllib.request.urlopen(token_req, timeout=30) as resp:
    access_token = json.loads(resp.read().decode("utf-8"))["access_token"]

key_payload = json.dumps(
    {
        "capabilities": {
            "devices": {
                "create": {
                    "reusable": False,
                    "ephemeral": True,
                    "preauthorized": True,
                    "tags": [tag],
                }
            }
        },
        "expirySeconds": 600,
        "description": f"gha-rbe-{hostname}",
    }
).encode("utf-8")
key_req = urllib.request.Request(
    "https://api.tailscale.com/api/v2/tailnet/-/keys",
    data=key_payload,
    headers={
        "Authorization": f"Bearer {access_token}",
        "Content-Type": "application/json",
    },
    method="POST",
)
with urllib.request.urlopen(key_req, timeout=30) as resp:
    auth_key = json.loads(resp.read().decode("utf-8"))["key"]

with open(key_file, "w", encoding="utf-8") as f:
    f.write(auth_key)
PY

sudo rm -f "$sock"
sudo bash -c "nohup $(printf '%q' "$tailscaled_bin") --tun=utun --state=mem: --socket=$(printf '%q' "$sock") >$(printf '%q' "$log") 2>&1 </dev/null & echo \$! >$(printf '%q' "$pid_file")"

ready=0
for _ in $(seq 1 60); do
    if sudo "$tailscale_bin" --socket="$sock" status --json >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 0.5
done

if [[ "$ready" -ne 1 ]]; then
    echo "tailscale-up: tailscaled did not become ready" >&2
    tail -n 50 "$log" >&2 || true
    exit 1
fi

if ! sudo "$tailscale_bin" --socket="$sock" up \
    --auth-key="file:$key_file" \
    --hostname="$hostname" \
    --accept-dns=false \
    --timeout=60s; then
    echo "tailscale-up: tailscale up failed" >&2
    tail -n 50 "$log" >&2 || true
    exit 1
fi

rm -f "$key_file"

ts_ip=$(sudo "$tailscale_bin" --socket="$sock" ip -4 | head -n 1)
if [[ -z "$ts_ip" ]]; then
    echo "tailscale-up: failed to obtain Tailscale IPv4 address" >&2
    exit 1
fi

printf '%s\n' "$ts_ip" >"$state_dir/ip.txt"
echo "tailscale-up: joined as $hostname ($ts_ip)" >&2
