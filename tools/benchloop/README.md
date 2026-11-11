# benchloop

Runs small GPU experiments on the Switch without a rebuild or a reboot. The Mac writes a batch of experiments into a
folder on the device over USB, HW Test's Bench screen runs each one on the GPU, and the results come back as files next
to the batch. Each result has the GPU time of every command buffer, from the raw path's PTIMER stamps, and the target's
texels. It exists to measure things nothing documents: SM53 instruction latencies, dual issue and operand reuse, the
ROP's iterated blend unit and its float-to-unorm rounding.

## On the Switch

The apps are on debug images only. Open HW Test. The first launch after a boot runs every test, so let that finish,
then tap Bench at the top left. The screen keeps the display awake and has to stay open while batches run.

The Self-check line should start with `PASS`. It times empty passes, draws the app's own known-answer program and runs a
kernel that only exits, all without the Mac. The Inboxes section lists the folders the app watches. If
`/private/var/mobile/Media/Switch/benchloop` is missing, run `doctor` once (it creates the folder over AFC), then close
the screen and tap Bench again.

## On the Mac

```sh
bazel run //tools/benchloop -- doctor                  # finds the device and a folder both sides reach, runs the calibration batch
bazel run //tools/benchloop -- latency                 # SM53 latency, throughput, dual issue, reuse
bazel run //tools/benchloop -- latency --family stall --op FFMA
bazel run //tools/benchloop -- blend "$PWD/table.json" --blend-enable both
bazel run //tools/benchloop -- rop                     # float to unorm rounding
bazel run //tools/benchloop -- run "$PWD/batch.json"   # any batch in protocol.py's format
bazel run //tools/benchloop -- latency --fake /tmp/fake  # the same loop against fakedevice.py, no Switch needed
```

Give absolute paths, since `bazel run` changes directory. Results go to
`~/Library/Application Support/switch-ios/benchloop/<time>/`: the device's result files, `results.tsv` (GPU ns per draw,
the fixed cost, the fit's largest residual and whether the texels were right) and `report.json`. A latency run also
writes `sm50_latency.rs`, the measured table in the shape of NAK's `instr_latency`.

`doctor` tries HW Test's Documents through house_arrest first, then the AFC root, and remembers the first folder the
calibration batch comes back through. Nobody has checked yet whether house_arrest serves an app that lives in
`/Applications`, which is why the AFC folder exists. While a batch runs, the Mac follows the app's `BENCH` log lines
through `idevicesyslog`. If the result files can't be pulled, those lines are the fallback, with medians and texel
CRCs only.

A blend table is JSON in the `iterated-blend/1` format described in `blend.py`: the microcode words (whole, or by the
field names in `clb197.h`), the pass count, any extra blend state, and the blend it should compute as a formula such as
`src.rgb * dst.rgb + src.rgb * (1 - dst.a) + dst.rgb * (1 - src.a)`. `testdata/multiply_example.json` shows the format.
Its microcode follows the fake device's guess at what the instruction fields mean, which nobody has run on hardware.

## Why it is built this way

The app draws through a raw device with its own connection and GPU VM. The app's Metal device already holds the
process's NVK VA window, and a channel the kernel kills during an experiment must not take the app down with it.
Programs replace the pixel program of a plain pack state, so the vertex program, the registers and the readback are
the ones the raw path already uses for Core Animation. No command buffer may have an estimated time over 50 ms, so a
program that hangs is caught by the kernel's stuck-channel rule (500 ms) and hang recovery, and the app needs no
timeout of its own. A kill is recorded in that experiment's result and the next experiment gets a fresh device.

On Maxwell, fixed-latency instructions don't interlock: the delay encoded with an instruction is the whole wait. A timed
chain of dependent FADDs therefore measures the delay written into it. The latency of those ops is the smallest delay at
which the chain still writes the right texel, which is why every timing experiment also returns its texels.

## When something is wrong

- `doctor` finds no device: iOS isn't running, or the Switch isn't paired. Unlock it and tap Trust.
- `no done.json after N s`: the Bench screen isn't open, the app was suspended, or the batch went to a folder the app
  isn't watching. The app writes `status.json` into every folder it watches, and `afcclient ls /Switch/benchloop` shows
  whether the Media folder has one.
- Self-check `FAIL ... no raw device`: the app couldn't open its own connection to the GPU. The line gives the reason.
- `refused` results name the draw predicate or state the raw path turned down. `killed` results carry the kernel's
  error code and fault address, and the kernel log has the decoded hang.
- `invalid` results never reached the GPU. Their `why` names the field that failed.
