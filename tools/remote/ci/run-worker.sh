#!/usr/bin/env bash
# Runs an ephemeral Buildbarn worker joined to the coordinator over Tailscale until the coordinator marks /done:
#   run-worker.sh <bb_bin_dir> <cluster_id> <worker_index> [coordinator_job_name]
set -euo pipefail

bb_bin=${1:?usage: run-worker.sh <bb_bin_dir> <cluster_id> <worker_index> [coordinator_job_name]}
cluster_id=${2:?}
worker_index=${3:?}
coord_job_name=${4:-release / build}

script_dir=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$script_dir/../../.." && pwd)
state_dir="${RUNNER_TEMP:-/tmp}/rbe-worker-${worker_index}"
ts_dir="$state_dir/ts"
bb_dir="$state_dir/bb"
worker_dir="$bb_dir/worker"
coord_host="bb-c-${cluster_id}"
worker_host="bb-w-${cluster_id}-${worker_index}"

mkdir -p "$state_dir/logs" "$bb_dir" "$worker_dir/build" "$worker_dir/cache"

runner_pid=""
worker_pid=""

cleanup() {
    if [[ -n "$worker_pid" ]]; then
        echo "=== worker metrics ===" >&2
        curl -sf --max-time 3 "http://127.0.0.1:9986/metrics" \
            | grep -E '^buildbarn_builder_(local_build_executor_operations_total|slot_input_root_files_total)' \
            | head -n 30 >&2 || true
        kill -TERM "$worker_pid" 2>/dev/null || true
        for _ in $(seq 1 30); do
            kill -0 "$worker_pid" 2>/dev/null || break
            sleep 1
        done
        kill -KILL "$worker_pid" 2>/dev/null || true
    fi
    if [[ -n "$runner_pid" ]]; then
        kill -TERM "$runner_pid" 2>/dev/null || true
        sleep 1
        kill -KILL "$runner_pid" 2>/dev/null || true
    fi
    "$script_dir/tailscale-down.sh" "$ts_dir" || true
}
trap cleanup EXIT

coordinator_job_status() {
    [[ -n "${GITHUB_REPOSITORY:-}" && -n "${GITHUB_RUN_ID:-}" ]] || {
        echo "unknown"
        return 0
    }
    local auth_args=()
    if [[ -n "${GITHUB_TOKEN:-}" ]]; then
        auth_args=(-H "Authorization: Bearer ${GITHUB_TOKEN}")
    fi
    local json
    json=$(curl -sf --max-time 10 \
        -H "Accept: application/vnd.github+json" \
        "${auth_args[@]}" \
        "https://api.github.com/repos/${GITHUB_REPOSITORY}/actions/runs/${GITHUB_RUN_ID}/jobs?per_page=100" 2>/dev/null) || {
        echo "unknown"
        return 0
    }
    COORD_JOB_NAME="$coord_job_name" python3 - "$json" <<'PY'
import json
import os
import sys

want = os.environ["COORD_JOB_NAME"]
data = json.loads(sys.argv[1])
for job in data.get("jobs", []):
    name = job.get("name", "")
    if name == want or name.endswith(" / " + want):
        print(job.get("status") or "unknown")
        sys.exit(0)
print("unknown")
PY
}

find_coordinator_ip() {
    local ts_bin=$1
    local sock="$ts_dir/tailscaled.sock"
    local status_json
    status_json=$(sudo "$ts_bin" --socket="$sock" status --json 2>/dev/null) || return 0
    COORD_HOST="$coord_host" python3 - "$status_json" <<'PY'
import json
import os
import sys

want = os.environ["COORD_HOST"].lower()
data = json.loads(sys.argv[1])
for peer in (data.get("Peer") or {}).values():
    host = (peer.get("HostName") or "").lower()
    if host == want or host.startswith(want + "-"):
        for ip in peer.get("TailscaleIPs") or []:
            if "." in ip:
                print(ip)
                sys.exit(0)
PY
}

"$script_dir/xcode-setup.sh" "$repo_root" "$state_dir/xcode.env"
# shellcheck source=/dev/null
. "$state_dir/xcode.env"

"$script_dir/tailscale-up.sh" "$worker_host" "$ts_dir"
ts_bin=$(cat "$ts_dir/tailscale.bin")
sock="$ts_dir/tailscaled.sock"

coord_ip=""
echo "worker-${worker_index}: waiting for coordinator $coord_host..." >&2
for i in $(seq 1 300); do
    ip=$(find_coordinator_ip "$ts_bin")
    if [[ -n "$ip" ]] && curl -sf --max-time 3 "http://${ip}:8999/health" >/dev/null 2>&1; then
        coord_ip="$ip"
        break
    fi
    if ((i % 5 == 0)); then
        job_status=$(coordinator_job_status)
        if [[ "$job_status" == "completed" ]]; then
            echo "worker-${worker_index}: coordinator job already completed; exiting cleanly" >&2
            exit 0
        fi
    fi
    sleep 3
done

if [[ -z "$coord_ip" ]]; then
    echo "worker-${worker_index}: timed out waiting for coordinator $coord_host" >&2
    exit 1
fi

echo "worker-${worker_index}: discovered coordinator at $coord_ip" >&2
sudo "$ts_bin" --socket="$sock" ping --c=3 --timeout=5s "$coord_ip" >&2 || true
curl -sf --max-time 5 -X POST "http://${coord_ip}:8999/heartbeat?worker=w${worker_index}" >/dev/null || true

ncpu=$(sysctl -n hw.ncpu)
mem_gib=$(($(sysctl -n hw.memsize) / 1073741824))
free_gib=$(df -g "$bb_dir" | awk 'NR==2 {print $4}')
concurrency=$((ncpu > 1 ? ncpu : 2))

worker_cache_gib=$((free_gib * 40 / 100))
((worker_cache_gib > 16)) && worker_cache_gib=16
((worker_cache_gib < 6)) && worker_cache_gib=6

echo "worker-${worker_index}: cpus=$ncpu mem=${mem_gib}GiB free=${free_gib}GiB workerCache=${worker_cache_gib}GiB slots=$concurrency" >&2

cp "$script_dir/runner.jsonnet" "$bb_dir/runner.jsonnet"
cp "$script_dir/worker.jsonnet" "$bb_dir/worker.jsonnet"

cat >"$bb_dir/host.libsonnet" <<EOF
{
  name: 'worker-${worker_index}-${cluster_id}',
  storageAddress: '${coord_ip}:8980',
  schedulerAddress: '${coord_ip}:8983',
  workerCacheGiB: ${worker_cache_gib},
  filepoolGiB: 4,
  concurrency: ${concurrency},
}
EOF

export SWITCH_BB_WORKER_DIR="$worker_dir"
export SWITCH_WORKER_DEVELOPER_DIR
export SWITCH_WORKER_XCODE_BUILD
export SWITCH_WORKER_XCODE_VERSION
export SWITCH_WORKER_GPU

ulimit -n 65536 2>/dev/null || true

rm -f "$worker_dir/runner.sock"
"$bb_bin/bb_runner" "$bb_dir/runner.jsonnet" >"$state_dir/logs/runner.log" 2>&1 &
runner_pid=$!
for _ in $(seq 1 40); do
    [[ -S "$worker_dir/runner.sock" ]] && break
    sleep 0.25
done
if [[ ! -S "$worker_dir/runner.sock" ]]; then
    echo "worker-${worker_index}: bb_runner socket did not appear" >&2
    tail -n 40 "$state_dir/logs/runner.log" >&2 || true
    exit 1
fi

"$bb_bin/bb_worker" "$bb_dir/worker.jsonnet" >"$state_dir/logs/worker.log" 2>&1 &
worker_pid=$!
for _ in $(seq 1 40); do
    if curl -sf --max-time 2 "http://127.0.0.1:9986/-/healthy" >/dev/null 2>&1; then
        break
    fi
    sleep 0.5
done

echo "worker-${worker_index}: running against $coord_ip:8980 / :8983" >&2

missed=0
while true; do
    if ! kill -0 "$worker_pid" 2>/dev/null; then
        echo "worker-${worker_index}: bb_worker exited unexpectedly" >&2
        tail -n 60 "$state_dir/logs/worker.log" >&2 || true
        exit 1
    fi
    if ! kill -0 "$runner_pid" 2>/dev/null; then
        echo "worker-${worker_index}: bb_runner exited unexpectedly" >&2
        tail -n 60 "$state_dir/logs/runner.log" >&2 || true
        exit 1
    fi

    http_code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "http://${coord_ip}:8999/done" || echo "000")
    if [[ "$http_code" == "200" ]]; then
        echo "worker-${worker_index}: coordinator signaled done" >&2
        exit 0
    elif [[ "$http_code" == "202" ]]; then
        missed=0
    else
        missed=$((missed + 1))
        if ((missed >= 12)); then
            job_status=$(coordinator_job_status)
            if [[ "$job_status" == "completed" ]]; then
                echo "worker-${worker_index}: coordinator unreachable and job completed; exiting" >&2
                exit 0
            fi
            echo "worker-${worker_index}: lost contact with coordinator for 60s; exiting" >&2
            exit 0
        fi
    fi
    sleep 5
done
