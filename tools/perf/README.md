# perf

Turns one session on the device into a report on what was slow and why: frame pacing per scenario, every missed frame
with its timeline and the stage that lost the period, CPU, GPU, state creations and compiles, hangs and the stacks the
crash folder already holds. It reads a debug boot's lines and Apple's own signposts. Pass and fail verdicts stay in
`tools/devicerun`.

```sh
bazel run //tools/perf -- pull "$HOME/Library/Application Support/switch-ios/sessions"   # Switch plugged in
bazel run //tools/perf -- report "$PWD/boot.logarchive" --crashes "$PWD/crash" --out "$PWD/report.txt"
```

`pull` makes a dated folder holding `boot.logarchive`, `crash/` and `report.txt`. `bazel run` changes into the runfiles
tree, so give absolute paths. From the repository root, `python3 -m tools.perf.perf` takes the same arguments and needs
nothing built. A 270 MB archive takes about 10 s.

## Reading it

A window is a HW Test test (its result line less its `ms=`), an app launch (FrontBoard's ask to the scene's content
turning ready), a touch run, a hang or a mark. Each section is read per window.

The stage that lost a period is the first point of the present's chain past the frame end it was due at: the render
thread's wake, its first GPU submission, its swap, the display queue, the GPU's completion, then the flip. The longest
stage would mislead, because a late frame spends most of a period waiting for the next scanout. Presents these lines
cannot place get their own rows and are never counted as lost. An unanchored present had no frame end before it, because
the display was idle. A gap before it means the present kept its own chain but the frame before it never showed. When
the render thread waited, it spent half a period or more in one block before its swap, waiting for the app's commit or
for a server rather than recording.

A cycle line means the late presents of a window keep to a timer's period, so something periodic is behind them.

## A session

1. Boot a debug build. Let it settle for ten minutes so the post-boot work is over.
2. Unplug and do the scenario. Note when you started and give that time to `report --start`. Launch each app twice,
   since the first launch after a boot is the cold one.
3. Plug in while the Switch is still up and run `pull`. The archive holds about the last hour.
