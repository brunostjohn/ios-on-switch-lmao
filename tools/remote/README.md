# Remote execution

Builds run their actions on a Mac build host: an EC2 Mac that runs Buildbarn's storage (the CAS and the action cache
on its own disk), scheduler, worker and runner. A worktree's output base then holds only what a command brings back,
so many worktrees build at once without each keeping gigabytes of intermediate files.

## Using it

Each Mac keeps an SSH forward to the host open (LaunchAgent `com.brunostjohn.switch.bazel-tunnel`, script
`~/Library/Application Support/switch-ios/bazel-cache/bazel-tunnel`). Through it, Bazel's cache and executor are
`grpc://127.0.0.1:18980`; the host's storage and scheduler diagnostics (`/-/healthy`, `/metrics`) are on 18990 and
18992. `bazel-cache-toggle` turns on `--config=remote` while the host's storage answers through the forward, and
`remote` turns on `rbe`. ssh accepts on its local port even when the far end is gone, so the toggle fetches the
storage's health page through the forward. The executor comes from the Mac's private `remote.bazelrc`:

```text
build:remote --remote_cache=grpc://127.0.0.1:18980
build:rbe --remote_executor=grpc://127.0.0.1:18980
build:rbe --noremote_upload_local_results
```

Actions build here when no worker is left (`--remote_local_fallback`, after the scheduler's two-minute wait). Bazel
runs that fallback unsandboxed, which is what the last line keeps out of the shared cache. To build one command
locally, pass `--remote_executor=`.

Only top-level outputs come back. A script that reads `bazel-bin` has to build what it reads as a top-level target,
or use `bazel run`.

Repository rules (IPSW members, toolchains) still run on the Mac, and an action's inputs reach the host from the Mac
that first needs them, over the home uplink. The host's own client (`~/src/switch-ios`, `~/.bazelrc` pointing at
127.0.0.1:8980) fetches the same pinned files at AWS speed: building a commit there first puts its inputs in the CAS,
and the Macs then upload only what they changed.

## The build host

It needs Xcode at `/Applications/Xcode.app`, selected with `xcode-select`, byte for byte the Macs' (actions name that
path; the worker advertises its build as the `xcode` exec property), the Metal toolchain at the pinned version
(`xcodebuild -downloadComponent MetalToolchain -buildVersion <build>`), and Xcode registered with LaunchServices
(`lsregister -f -R /Applications/Xcode.app`): a headless Mac never registers it, and rules_swift's xcode-locator then
finds no Xcode.

1. Build `bb_storage` from `buildbarn/bb-storage`, and `bb_scheduler`, `bb_worker` and `bb_runner` from
   `buildbarn/bb-remote-execution` at the commit whose go.mod names that bb-storage, with `go build -trimpath`. On
   macOS this needs bb-storage's own patches that its Bazel build applies, `o-search.diff` for `golang.org/x/sys` and
   `service-registrar.diff` for `googleapis/bytestream` (go.mod `replace` lines to patched copies), and
   `bb_worker-clonefile.diff` from this directory. Upstream hard-links each input from the worker's cache into the
   action's directory, and macOS can report a hard-linked executable under any of its names. The hermetic Python and
   rustc find their libraries next to themselves, so they looked in the cache directory and failed. An APFS clone is
   a separate file that takes no space.
2. Run `tools/remote/host/install-host.sh <dir with the four binaries> <name> <concurrent actions>`. It installs four
   LaunchDaemons that run as the invoking user (`com.brunostjohn.switch.bb-storage`, `-scheduler`, `-runner`,
   `-worker`) and starts them. Everything listens on 127.0.0.1 only: clients come in through SSH. The CAS (400 GiB)
   and the action cache (16 GiB) are block files under `~/Library/Caches/switch-ios-buildbarn/storage`, kept across
   restarts; their sizes are fixed at first start, and changing one discards that store.

Logs are `~/Library/Caches/switch-ios-buildbarn/<service>.log`. `sudo launchctl kickstart -k
system/com.brunostjohn.switch.bb-<service>` restarts one; restarting the worker empties its input cache.

What sets the host's speed, measured on 14 Nov (10-core M4, 16 executors):

- Gatekeeper. Every action gets new APFS clones of its tools, and macOS assesses each new executable or library the
  first time it runs (syspolicyd and XProtect, one at a time, 0.4-2 s each): about 2 actions a second with half the
  CPU idle. The host runs with assessments off (`spctl --global-disable`, confirmed once in System Settings over
  Screen Sharing through an SSH forward).
- IOPS. Creating and deleting every input of every action held the 5,000-IOPS root volume at its ceiling; at 16,000
  IOPS the host does about 4 actions a second, CPU-bound, half of it in the kernel (bb_worker and fseventsd).
  `setup-volume.sh` moves that churn to the instance store once the daemons have Full Disk Access.
- Automatic macOS updates are off, so the host never installs one and reboots in the middle of a build.

Bluetooth has no working controller on an EC2 Mac and bluetoothd restarts every ten seconds, which keeps audiomxd and
configd at about a core between them; SIP does not let launchd disable it.

`@gpu_device_data` comes from the host's own `~/Library/Application Support/switch-ios/gpu-device-data/` (gpu/deps.bzl).
A client without the pinned tar there gives the seed's probe actions other inputs and keys, so every Mac and the host
keep the same tar.

### Another host

An extra host runs only the worker and the runner against the first host's storage and scheduler over the private
network: reinstall the first host with `SWITCH_BB_PRIVATE_IP=<its private IP>` in the environment, so its storage
(8980) and the scheduler's worker port (8983) also listen there, and run `install-host.sh <bins> <name> <actions>
<first host's private IP>` on the new one. The security group then needs TCP 8980-8983 from itself. Those ports take
no credentials.

## The cluster cache (until 14 Nov)

Buildbarn's frontend, scheduler and storage also run in the homelab cluster (`k8s/bazel-cache`), with the Mac Studio
as a worker (`install.sh`, `worker.jsonnet`, `runner.jsonnet`, `switch-worker` here; tokens in
`~/Library/Application Support/switch-ios/bazel-worker/`). To go back to it, point the private `remote.bazelrc` and
`bazel-cache-toggle` at `10.0.0.10:30980` again; both files keep the old lines as comments. To stop the Studio's
worker, boot out its three jobs: `launchctl bootout gui/$(id -u)/com.brunostjohn.switch.bazel-worker`, then the same
for `-runner` and `-floor`.
