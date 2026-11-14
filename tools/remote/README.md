# Remote execution

Builds can run their actions on Mac workers behind the shared cache: Buildbarn's frontend, scheduler and storage run in
the homelab cluster (`k8s/bazel-cache`), and the workers run on the Macs. A worktree's output base then holds only
what the build hands back, so many worktrees can build at once without each keeping gigabytes of intermediate files.

## Using it

`bazel-cache-toggle` turns on `--config=remote` while the cache answers, and `remote` turns on `rbe`. The executor
comes from the Mac's private `remote.bazelrc`:

```text
build:rbe --remote_executor=grpc://<frontend host:port>
build:rbe --noremote_upload_local_results
```

Without that line, or off the LAN, actions build here as before. Actions also build here when no worker is left
(`--remote_local_fallback`, after the scheduler's two-minute wait). Bazel runs that fallback unsandboxed, which is
what the second line keeps out of the shared cache. It lives here and not in `.bazelrc` because every cache user
expands `rbe`. To build one command locally, pass `--remote_executor=`.

Only top-level targets come back. A script that reads `bazel-bin` has to build what it reads as a top-level target,
or use `bazel run`.

## Running a worker on a Mac

The worker needs Xcode at `/Applications/Xcode.app`, selected with `xcode-select`. Every client's actions name that
path, and the worker advertises that Xcode's build as the `xcode` exec property.

1. Build `bb_worker` and `bb_runner` from `buildbarn/bb-remote-execution` at the commit in the cluster's scheduler
   image tag, with `go build -trimpath ./cmd/bb_worker ./cmd/bb_runner`. On macOS this needs three patches:
   - bb-storage's `patches/org_golang_x_sys/o-search.diff` for `golang.org/x/sys`. Buildbarn's own Bazel build
     applies it.
   - `bb_worker-clonefile.diff` from this directory. Upstream hard-links each input from the cache into the action's
     directory, and macOS can report a hard-linked executable under any of its names. The hermetic Python and rustc
     find their libraries next to themselves, so they looked in the cache directory and failed. An APFS clone is a
     separate file that takes no space.
   - `bb_worker-slots.diff` from this directory, on top of the clonefile one. Upstream builds every action's inputs in
     a new directory and deletes them afterwards. On the EC2 host that churn kept about 4 of its 10 cores busy in the
     worker, fseventsd and XProtect, which scans every new copy of a tool. With the patch, each executor keeps its input
     root between actions and fetches only what changed. A file that an action wrote, chmodded or replaced is fetched
     again. `//gpu:raw_stack` (1,309 remote actions) then took 109-121 s there instead of 169-172 s (Gatekeeper off,
     data on the instance-store NVMe). The input roots are clones kept outside the cache's size limit, and neither the
     floor nor an idle worker empties them: with a small cache, such as the Studio's 2 GiB, they can hold more of the
     disk than the cache does.
2. Put two tokens, minted with the cache's key (homelab `make-cache-token.sh`), in
   `~/Library/Application Support/switch-ios/bazel-worker/`: `cache.token` (audience `bazel-cache`) and
   `worker.token` (audience `bazel-worker`).
3. Run `tools/remote/install.sh <dir with the binaries> <frontend host:port> <scheduler host:port> <cache GiB>
   <concurrent actions> [floor GiB]`. Use a node's own address, not the API VIP: the VIP resets long-lived connections
   within a minute. The Studio runs with a 2 GiB cache, 12 actions and a 17 GiB floor. The script installs three
   LaunchAgents (`com.brunostjohn.switch.bazel-runner`, `-worker`, `-floor`) and starts them.

Inputs are cached in `~/Library/Caches/switch-ios-worker/cache`, up to the given size. Each executor runs its actions in
`build/<number>/` next to it, and the last action's input root stays there until the executor's next action. Logs go
to `runner.log` and `worker.log`. While free disk is under the floor (20 GiB by default), the floor job takes the
runner out of service, and the worker takes no new work until there is room again. Restarting the worker empties its
cache and its build directory.

To stop a worker, boot out its three jobs: `launchctl bootout gui/$(id -u)/com.brunostjohn.switch.bazel-worker`,
then the same for `-runner` and `-floor`.
