# Remote execution

Builds run their actions on Mac workers behind the shared cache (Buildbarn: frontend, scheduler and storage in the
homelab cluster, workers on the Macs). The worktree's output base then holds only the top-level outputs, so many
worktrees can build at once without each one keeping gigabytes of intermediate files.

## Using it

Nothing to type. `bazel-cache-toggle` turns on `--config=remote` while the cache answers, and `remote` turns on `rbe`.
The executor comes from the Mac's private `remote.bazelrc`:

```
build:rbe --remote_executor=grpc://<frontend host:port>
```

Without that line, or off the LAN, actions build locally as before. If a worker disappears, its actions build locally
too (`--remote_local_fallback`). To build one command locally, pass `--remote_executor=`.

Only top-level targets are downloaded. A script that reads `bazel-bin` must build what it reads as a top-level target,
or use `bazel run`.

## Running a worker

The worker needs Xcode at `/Applications/Xcode.app`, selected with `xcode-select`. Every client's actions name that
path, and the worker advertises that Xcode's build as the `xcode` exec property. A client with a different Xcode
gets no work done there.

1. Build `bb_worker` and `bb_runner` from `buildbarn/bb-remote-execution` at the commit that matches the cluster's
   scheduler image (its tag ends in the commit), with `go build -trimpath ./cmd/bb_worker ./cmd/bb_runner`. On macOS
   `golang.org/x/sys` needs bb-storage's `patches/org_golang_x_sys/o-search.diff` (Buildbarn's own Bazel build
   applies it), and `bb_worker-clonefile.diff` here. Upstream hard-links each input from the cache into the action's
   directory, and macOS may report a hard-linked executable by any of its names. The hermetic Python and rustc look
   for their libraries next to themselves, so they found the cache directory and failed. APFS clones are separate
   files that cost no space.
2. Put two tokens, minted with the cache's key (homelab `make-cache-token.sh`), in
   `~/Library/Application Support/switch-ios/bazel-worker/`: `cache.token` (audience `bazel-cache`) and
   `worker.token` (audience `bazel-worker`).
3. Run `tools/remote/install.sh <dir with the binaries> <frontend host:port> <scheduler host:port> <cache GiB>
   <concurrent actions>`. This installs two LaunchAgents (`com.brunostjohn.switch.bazel-runner` and
   `-worker`) and starts them.

Inputs are cached in `~/Library/Caches/switch-ios-worker/cache`, up to the given size. Actions run in `build/`
next to it and are deleted when they finish. Logs go to `runner.log` and `worker.log` in the same directory. When less
than 20 GiB is free (`SWITCH_WORKER_FLOOR_GIB`), the worker stops taking work until there is room again.

To stop a worker: `launchctl bootout gui/$(id -u)/com.brunostjohn.switch.bazel-worker` (and `-runner`).
