# switch_boot

Makes the boot entry `//boot:package` makes, on the user's computer: `switchroot/`, `bootloader/`, and the overlay
files built from Apple's (`installd`, and on iOS 17 the launchd catalogue and its signature). It takes three inputs:

- the build's record, the boot part of its catalog entry (`record.rs`, written from `ipsw/pins.bzl`);
- the release's boot assets for the build, one tar (`//boot:boot_assets`);
- the IPSW's IM4P members and a few files of its OS volume.

`boot_files` runs `boot/defs.bzl`'s steps in its order: prelink, launchd catalogue, `installd`, ADT finish, trust
bundle, kernel window, `bl33.bin`. The record and the bundle decide which steps run and on what, so the code never
names a build. The release's loader refuses any window whose `constants.json` differs from CI's, so a file that
differs by one byte doesn't boot. The checks CI already makes (`codesign --verify`, the prelink reports, the lz4
decode-back) stay in CI.

The bundle holds everything the steps read: the unprelinked kernel and its prelink inputs, the base ADT, the
overlay and GPU trust modules, the loader and its constants, U-Boot, and the files under `tree/`, which the boot
entry carries unchanged. `assets.rs` names each member. A bundle stamped for another build, or one with a member
missing or unknown, is refused before any step runs. switch_boot doesn't hash the tar: the download checks it
against the catalog's SHA-256.

```sh
bazel test --config=ios17 //install/boot:all      # every file of //boot:package's tree from @ipsw and the bundle
bazel test --config=ios27 //install/boot:all
bazel build --config=ios27 --config=release --keep_going --nobuild //install/boot:assets_ipsw_free
bazel build //install/boot:desktop                # switch_boot_tool for the six desktop platforms
```

`chain_test` compares every file it makes with `//boot:package`'s tree. Each other file of the tree must come from
another release asset: the rest of `overlay/`, and `bootstrap/payload.tar`. `chain_opt_test` runs the same at
`-c opt` and prints the time and peak RSS.
