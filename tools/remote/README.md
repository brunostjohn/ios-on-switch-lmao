# Remote execution

Builds can run their actions on Mac workers behind the shared cache: Buildbarn's frontend, scheduler and storage run in
the homelab cluster (`k8s/bazel-cache`), and the workers run on the Macs. A worktree's output base then holds only
what the build hands back, so many worktrees can build at once without each keeping gigabytes of intermediate files.

## Using it

`bazel-cache-toggle` turns on `--config=remote` while the cache answers, and `remote` turns on `rbe`. The executor
comes from the Mac's private `remote.bazelrc`:

```text
build:rbe --remote_executor=grpc://<frontend host:port>
```

Without that line, or off the LAN, actions build here as before. Actions also build here when no worker is left
(`--remote_local_fallback`, after the scheduler's two-minute wait). To build one command locally, pass
`--remote_executor=`.

Only top-level targets come back. A script that reads `bazel-bin` has to build what it reads as a top-level target,
or use `bazel run`.

`--config=rbe-linux` is an experiment that sends targets marked `//bazel/platforms:portable` to the cluster's Linux
workers. Today that means Mesa's Python generators.

## Running a worker on a Mac

The worker needs Xcode at `/Applications/Xcode.app`, selected with `xcode-select`. Every client's actions name that
path, and the worker advertises that Xcode's build as the `xcode` exec property.

1. Build `bb_worker` and `bb_runner` from `buildbarn/bb-remote-execution` at the commit in the cluster's scheduler
   image tag, with `go build -trimpath ./cmd/bb_worker ./cmd/bb_runner`. On macOS this needs two patches:
   - bb-storage's `patches/org_golang_x_sys/o-search.diff` for `golang.org/x/sys`. Buildbarn's own Bazel build
     applies it.
   - `bb_worker-clonefile.diff` from this directory. Upstream hard-links each input from the cache into the action's
     directory, and macOS can report a hard-linked executable under any of its names. The hermetic Python and rustc
     find their libraries next to themselves, so they looked in the cache directory and failed. An APFS clone is a
     separate file that takes no space.
2. Put two tokens, minted with the cache's key (homelab `make-cache-token.sh`), in
   `~/Library/Application Support/switch-ios/bazel-worker/`: `cache.token` (audience `bazel-cache`) and
   `worker.token` (audience `bazel-worker`).
3. Run `tools/remote/install.sh <dir with the binaries> <frontend host:port> <scheduler host:port> <cache GiB>
   <concurrent actions>`. Use a node's own address, not the API VIP: the VIP resets long-lived connections within a
   minute. The script installs three LaunchAgents (`com.brunostjohn.switch.bazel-runner`, `-worker`, `-floor`) and
   starts them.

Inputs are cached in `~/Library/Caches/switch-ios-worker/cache`, up to the given size. Actions run in `build/` next to
it and are deleted when they finish. Logs go to `runner.log` and `worker.log`. While less than 20 GiB is free
(`SWITCH_WORKER_FLOOR_GIB`), the floor job takes the runner out of service, and the worker takes no new work until
there is room again.

To stop a worker, boot out its three jobs: `launchctl bootout gui/$(id -u)/com.brunostjohn.switch.bazel-worker`,
then the same for `-runner` and `-floor`.
