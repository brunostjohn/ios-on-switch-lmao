#!/bin/sh
# Installs (or updates) Buildbarn on a Mac build host as LaunchDaemons running as the invoking user, and starts them.
#
#   install-host.sh <dir with bb_storage bb_scheduler bb_worker bb_runner> <name> <concurrent actions> [first host's private IP]
#
# Without a first host's IP this is the first host: bb_storage (CAS + action cache), bb_scheduler, bb_worker and
# bb_runner, listening on 127.0.0.1 only (clients come in through an SSH forward). With one, it is an extra host that
# runs only bb_worker and bb_runner against the first host's storage and scheduler over the private network; the first
# host then needs its own private IP in SWITCH_BB_PRIVATE_IP when it is (re)installed, so it listens there too.
set -eu

bin=${1:?usage: install-host.sh <bb binaries dir> <name> <concurrent actions> [first host private IP]}
name=${2:?}
concurrency=${3:?}
first=${4:-}

here=$(cd "$(dirname "$0")" && pwd)
user=$(id -un)
home="$HOME/Library/Application Support/switch-ios/buildbarn"
data="$HOME/Library/Caches/switch-ios-buildbarn"

if [ -n "$first" ]; then
    roles="runner worker"
    storage="$first:8980"
    scheduler="$first:8983"
    listen="'127.0.0.1'"
else
    roles="storage scheduler runner worker"
    storage=127.0.0.1:8980
    scheduler=127.0.0.1:8983
    listen="'127.0.0.1'"
    [ -n "${SWITCH_BB_PRIVATE_IP:-}" ] && listen="$listen, '$SWITCH_BB_PRIVATE_IP'"
fi

mkdir -p "$home" "$data/worker/build" "$data/worker/cache"
for store in cs cms ac; do
    mkdir -p "$data/storage/$store/persistent_state"
done
touch "$data/.metadata_never_index"

# A new file, never one written over: macOS kills a binary whose signed pages changed under the same vnode.
for file in "$bin/bb_storage" "$bin/bb_scheduler" "$bin/bb_worker" "$bin/bb_runner" "$here/switch-buildbarn" \
    "$here/storage.jsonnet" "$here/scheduler.jsonnet" "$here/worker.jsonnet" "$here/runner.jsonnet"; do
    base=$(basename "$file")
    cp "$file" "$home/.$base.new"
    mv -f "$home/.$base.new" "$home/$base"
done

cat >"$home/.host.libsonnet.new" <<EOF
{
  name: '$name',
  dataDir: '$data',
  listenAddresses: [$listen],
  storageAddress: '$storage',
  schedulerAddress: '$scheduler',
  casGiB: 400,
  workerCacheGiB: 100,
  concurrency: $concurrency,
}
EOF
mv -f "$home/.host.libsonnet.new" "$home/host.libsonnet"

for role in $roles; do
    label=com.brunostjohn.switch.bb-$role
    plist=/Library/LaunchDaemons/$label.plist
    sudo tee "$plist" >/dev/null <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$label</string>
	<key>UserName</key>
	<string>$user</string>
	<key>GroupName</key>
	<string>staff</string>
	<key>ProgramArguments</key>
	<array>
		<string>/bin/sh</string>
		<string>$home/switch-buildbarn</string>
		<string>$role</string>
	</array>
	<key>EnvironmentVariables</key>
	<dict>
		<key>HOME</key>
		<string>$HOME</string>
		<key>PATH</key>
		<string>/usr/bin:/bin:/usr/sbin:/sbin</string>
	</dict>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<true/>
	<!-- Without it launchd throttles a daemon's CPU and I/O as background work. -->
	<key>ProcessType</key>
	<string>Interactive</string>
	<key>SoftResourceLimits</key>
	<dict>
		<key>NumberOfFiles</key>
		<integer>65536</integer>
	</dict>
	<key>StandardOutPath</key>
	<string>$data/$role.log</string>
	<key>StandardErrorPath</key>
	<string>$data/$role.log</string>
</dict>
</plist>
EOF
    sudo launchctl bootout "system/$label" 2>/dev/null || true
    # bootout returns before the job is gone, and bootstrapping it again before then fails.
    while sudo launchctl print "system/$label" >/dev/null 2>&1; do
        sleep 0.2
    done
    sudo launchctl bootstrap system "$plist"
done
echo "installed $roles: $home; data: $data"
