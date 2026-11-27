# switch_prelink

Prelinks Apple's kexts into the project's kernel on the user's computer. The result must equal CI's byte for byte: the
kernel window's LZ4 blocks and `STAGE_ID` are fixed when CI builds the loader, so a kernel that differs by one byte
won't boot.

The iOS 17 prelink (`src/v17/`) is `tools/prelink/emit_prelink.py`'s `plan` and `apply` in one pass. It reads:

- the unprelinked `kernel.macho` and its `prelink.json` (`//install:kernel_inputs`);
- the decompressed kernelcache (`switch_img4`);
- `dma-kernel.json` and `dma-prelink.json`, from the V20 prelink it inherits. The build's own inline-data pools
  (21H461's Libm) come from `dma-kernel.json`'s `known_inline_data`, read back from the kernelcache;
- the kind's site tables (`kits/SwitchABI/kinds/<kind>/sites/`).

The fileset prelink for iOS 27 (`src/fileset/`) is `translate/arm64e/xnu/prelink26/emit26.py --from-bindings`, step for
step. It reads:

- the unprelinked 12377 kernel (`//kernel:kernel_unprelinked_12377`);
- the decompressed 24B119 kernelcache;
- CI's bindings export (`//kernel:prelink_bindings_12377`), which holds for that kernel and kernelcache only;
- the build's pins (`:fileset_pins`), the SHA-256 of each site table and of the prelinked kernel;
- the kind's site tables.

The arm64e translation and the FIPS MACs run in process through `switch_a64x`. That translator is compiled for the
user's platform by a different compiler than the CLI that made CI's kernel, so no input hash can pin it: the prelink
returns no bytes unless the result hashes to the pinned kernel.

Both refuse the inputs their Python refuses, mostly with the same message. The prelink reports stay in CI.

```sh
bazel test //install/prelink:switch_prelink_test
bazel test --config=ios17 //install/prelink:oracle_test           # against //kernel:kernel and the release path
bazel test --config=ios27 //install/prelink:fileset_oracle_test   # against //kernel:kernel_12377, and the refusals
bazel build //install/prelink:desktop                             # switch_prelink_tool for the six platforms
switch_prelink_tool v17 kernel.macho kernelcache prelink.json dma-prelink.json dma-kernel.json <kind> out.macho sites/*.json
switch_prelink_tool fileset kernel.macho kernelcache prelink-bindings.json prelink-pins.json out.macho sites/*.json
switch_prelink_tool pins <build> prelinked.macho prelink-pins.json sites/*.json
```

`//install:release_kernel_10063` reads a published release, so the 17 oracle runs the same `release_prelink` rule over
this checkout's own `//install:kernel_inputs` instead (`:release_kernel_local`).
