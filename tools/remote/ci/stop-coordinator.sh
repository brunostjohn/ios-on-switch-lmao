#!/usr/bin/env bash
# Signals cluster completion to remote workers, prints cluster metrics, and tears down coordinator services.
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
state_dir="${RUNNER_TEMP:-/tmp}/rbe-coord"

if curl -sf --max-time 3 -X POST "http://127.0.0.1:8999/done" >/dev/null 2>&1; then
    echo "coordinator: signaled /done to workers" >&2
fi

echo "=== coordinator sentinel status ===" >&2
curl -sf --max-time 3 "http://127.0.0.1:8999/status" >&2 || true

echo "=== bb_scheduler worker summary ===" >&2
curl -sf --max-time 3 "http://127.0.0.1:9982/metrics" \
    | grep -E '^buildbarn_builder_(in_memory_build_queue_operations_queued_total|in_memory_build_queue_operations_completed_total|in_memory_build_queue_workers_current)' \
    | head -n 40 >&2 || true

echo "=== sentinel log ===" >&2
cat "$state_dir/logs/sentinel.log" >&2 || true

# Give remote workers one poll interval to observe /done before Tailscale disconnects.
sleep 6

stop_pid() {
    local name=$1
    local pid_file="$state_dir/pids/${name}.pid"
    [[ -f "$pid_file" ]] || return 0
    local pid
    pid=$(cat "$pid_file")
    [[ -n "$pid" ]] || return 0
    kill -TERM "$pid" 2>/dev/null || true
}

for svc in worker runner scheduler storage sentinel; do
    stop_pid "$svc"
done
sleep 2
for svc in worker runner scheduler storage sentinel; do
    pid_file="$state_dir/pids/${svc}.pid"
    if [[ -f "$pid_file" ]]; then
        pid=$(cat "$pid_file")
        kill -KILL "$pid" 2>/dev/null || true
    fi
done

"$script_dir/tailscale-down.sh" "$state_dir/ts"
