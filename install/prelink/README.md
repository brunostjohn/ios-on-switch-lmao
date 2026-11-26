# switch_prelink

Prelinks Apple's kexts into the project's kernel on the user's computer. The result must equal CI's byte for byte: the
kernel window's LZ4 blocks and `STAGE_ID` are fixed when CI builds the loader, so a kernel that differs by one byte
won't boot.

The iOS 17 prelink (`src/v17/`) is `tools/prelink/emit_prelink.py`'s `plan` and `apply` in one pass. It reads:

- the unprelinked `kernel.macho` and its `prelink.json` (`//install:kernel_inputs`);
- the decompressed kernelcache (`switch_img4`);
- `dma-kernel.json` and `dma-prelink.json`, from the V20 prelink it inherits;
- the kind's site tables (`kits/SwitchABI/kinds/<kind>/sites/`).

It refuses the inputs the Python refuses, mostly with the same message. The prelink report (`emit_prelink.py report`)
stays in CI.

```sh
bazel test //install/prelink:switch_prelink_test
bazel test --config=ios17 //install/prelink:oracle_test     # against //kernel:kernel and the release path
bazel build //install/prelink:desktop                       # switch_prelink_tool for the six platforms
switch_prelink_tool v17 kernel.macho kernelcache prelink.json dma-prelink.json dma-kernel.json <kind> out.macho sites/*.json
```

`//install:release_kernel_10063` reads a published release, so the oracle runs the same `release_prelink` rule over
this checkout's own `//install:kernel_inputs` instead (`:release_kernel_local`).
