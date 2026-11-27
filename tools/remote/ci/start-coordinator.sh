#!/usr/bin/env bash
# Starts the ephemeral Buildbarn coordinator (storage, scheduler, local worker+runner, sentinel) over Tailscale
# and appends RBE flags to ~/.bazelrc:
#   start-coordinator.sh <bb_bin_dir> <cluster_id> [upload]
set -euo pipefail

bb_bin=${1:?usage: start-coordinator.sh <bb_bin_dir> <cluster_id> [upload]}
cluster_id=${2:?}
upload=${3:-false}

script_dir=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$script_dir/../../.." && pwd)
state_dir="${RUNNER_TEMP:-/tmp}/rbe-coord"
ts_dir="$state_dir/ts"
bb_dir="$state_dir/bb"
worker_dir="$bb_dir/worker"

mkdir -p "$state_dir/pids" "$state_dir/logs" "$bb_dir" "$worker_dir/build" "$worker_dir/cache"
for store in cs cms ac; do
    mkdir -p "$bb_dir/storage/$store/persistent_state"
done

"$script_dir/xcode-setup.sh" "$repo_root" "$state_dir/xcode.env"
# shellcheck source=/dev/null
. "$state_dir/xcode.env"

"$script_dir/tailscale-up.sh" "bb-c-${cluster_id}" "$ts_dir"
ts_ip=$(cat "$ts_dir/ip.txt")

ncpu=$(sysctl -n hw.ncpu)
mem_gib=$(($(sysctl -n hw.memsize) / 1073741824))
free_gib=$(df -g "$bb_dir" | awk 'NR==2 {print $4}')
concurrency=$((ncpu > 1 ? ncpu : 2))

cas_gib=$((free_gib * 45 / 100))
((cas_gib > 32)) && cas_gib=32
((cas_gib < 10)) && cas_gib=10

worker_cache_gib=$((free_gib * 25 / 100))
((worker_cache_gib > 12)) && worker_cache_gib=12
((worker_cache_gib < 4)) && worker_cache_gib=4

echo "coordinator: cpus=$ncpu mem=${mem_gib}GiB free=${free_gib}GiB cas=${cas_gib}GiB workerCache=${worker_cache_gib}GiB slots=$concurrency ts=$ts_ip" >&2

for f in storage.jsonnet scheduler.jsonnet runner.jsonnet worker.jsonnet; do
    cp "$script_dir/$f" "$bb_dir/$f"
done

cat >"$bb_dir/host.libsonnet" <<EOF
{
  name: 'coord-${cluster_id}',
  dataDir: '${bb_dir}',
  listenAddresses: ['127.0.0.1', '${ts_ip}'],
  storageAddress: '127.0.0.1:8980',
  schedulerAddress: '127.0.0.1:8983',
  casGiB: ${cas_gib},
  cmsGiB: 1,
  acGiB: 2,
  workerCacheGiB: ${worker_cache_gib},
  filepoolGiB: 4,
  concurrency: ${concurrency},
}
EOF

wait_http() {
    local url=$1
    local name=$2
    local log_file=$3
    for _ in $(seq 1 60); do
        if curl -sf --max-time 2 "$url" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    echo "coordinator: $name failed to become healthy at $url" >&2
    tail -n 60 "$log_file" >&2 || true
    exit 1
}

export SWITCH_BB_WORKER_DIR="$worker_dir"
export SWITCH_WORKER_DEVELOPER_DIR
export SWITCH_WORKER_XCODE_BUILD
export SWITCH_WORKER_XCODE_VERSION
export SWITCH_WORKER_GPU

ulimit -n 65536 2>/dev/null || true

nohup "$bb_bin/bb_storage" "$bb_dir/storage.jsonnet" >"$state_dir/logs/storage.log" 2>&1 </dev/null &
echo $! >"$state_dir/pids/storage.pid"
wait_http "http://127.0.0.1:9980/-/healthy" "bb_storage" "$state_dir/logs/storage.log"

nohup "$bb_bin/bb_scheduler" "$bb_dir/scheduler.jsonnet" >"$state_dir/logs/scheduler.log" 2>&1 </dev/null &
echo $! >"$state_dir/pids/scheduler.pid"
wait_http "http://127.0.0.1:9982/-/healthy" "bb_scheduler" "$state_dir/logs/scheduler.log"

rm -f "$worker_dir/runner.sock"
nohup "$bb_bin/bb_runner" "$bb_dir/runner.jsonnet" >"$state_dir/logs/runner.log" 2>&1 </dev/null &
echo $! >"$state_dir/pids/runner.pid"
for _ in $(seq 1 40); do
    [[ -S "$worker_dir/runner.sock" ]] && break
    sleep 0.25
done
if [[ ! -S "$worker_dir/runner.sock" ]]; then
    echo "coordinator: bb_runner socket did not appear" >&2
    tail -n 40 "$state_dir/logs/runner.log" >&2 || true
    exit 1
fi

nohup "$bb_bin/bb_worker" "$bb_dir/worker.jsonnet" >"$state_dir/logs/worker.log" 2>&1 </dev/null &
echo $! >"$state_dir/pids/worker.pid"
wait_http "http://127.0.0.1:9986/-/healthy" "bb_worker" "$state_dir/logs/worker.log"

nohup python3 "$script_dir/sentinel.py" --bind 0.0.0.0 --port 8999 >"$state_dir/logs/sentinel.log" 2>&1 </dev/null &
echo $! >"$state_dir/pids/sentinel.pid"
wait_http "http://127.0.0.1:8999/health" "sentinel" "$state_dir/logs/sentinel.log"

{
    echo "build --config=remote"
    echo "build:remote --remote_cache=grpc://127.0.0.1:8980"
    echo "build:rbe --remote_executor=grpc://127.0.0.1:8980"
    echo "build:rbe --jobs=64"
    echo "build:rbe --local_resources=cpu=HOST_CPUS"
    echo "build:rbe --remote_retries=5"
    if [[ "$upload" == "true" ]]; then
        echo "build:rbe --remote_download_outputs=all"
    fi
} >>~/.bazelrc

echo "coordinator: ready on $ts_ip (storage :8980, scheduler :8983, sentinel :8999)" >&2
