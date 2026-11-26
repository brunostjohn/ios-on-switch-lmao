# switch_catalog

The release's `catalog.json`. It has one record per build, holding everything the installer needs to know about that
build: the IPSW's URL, each member's pins, the volumes, the derived values, which steps the build runs, and the
release assets with their names, sizes and SHA-256. Adding a build means adding its pins to `ipsw/pins.bzl`. The
installer's code doesn't change.

## What gets refused

`Catalog::parse` refuses the whole catalog if it's another format or holds a field this installer doesn't know. It
reads the format first, so an installer that's too old says so instead of naming a field.

`Record::plan` refuses one record if it names a step, a step's kind, a member role, a container format or an asset
role this installer doesn't know. The other builds stay usable, and the refused one says it needs a newer installer.

A new step or kind is data and keeps format 1. A new field changes the format.

Steps in format 1:

| Step | Kind | When `catalog.bzl` writes it |
|---|---|---|
| `prelink` | `native`, `fileset` | always, from the XNU (`PRELINK`, mirroring `//kernel:prelinked`) |
| `launchd_catalog` | `jobs` | the build pins Apple's launchd catalogue |
| `pretranslate` | `a64x` | the build pins `pretranslated` |
| `boot_manifest_hash` | `sha384` | iOS 27 and later (`boot/defs.bzl`'s rule) |

## Generation

`catalog.bzl` turns each pinned kind of `IPSWS` into a record, merged with `staged.bzl`. `STAGED` holds the member
pins `pins.bzl` doesn't have yet: AEA raw sizes, container formats and hashes, and IM4P payload sizes and hashes. A
key that both files define fails the build, so moving a value into `pins.bzl` means deleting it here. A `pins.bzl` key
the catalog doesn't carry also fails the build.

`:catalog_json` builds `//boot:boot_assets` once for each kind through a split transition on `//ipsw:kind`.
`switch_catalog_tool` hashes each tar into its record, then refuses any catalog that this installer can't parse, that
has a record it can't plan, or that lacks an asset. Its outputs are what the release publishes:

- `catalog.json`, under that name, so `releases/latest/download/catalog.json` finds it;
- `ios-on-switch-lmao-<version>-boot-<build>.tar` for each build, a link to the tar it hashed.

`:catalog_json` is tagged manual. Under `--config=ios17`, the 24B119 tar is built in a transitioned configuration
(an `-ST-` output directory), so the first build runs 24B119's whole boot pipeline again. Later builds hit the cache.

```sh
bazel test //install/catalog:all                  # parser, round trip against IPSWS + STAGED, synthetic iOS 18
bazel build //install/catalog:catalog_json        # catalog.json and the boot tars, every kind
bazel test //install/catalog:published_test       # those checked against each other
bazel build --config=release --nobuild //install/catalog:catalog_ipsw_free
```

`:records.json` is the catalog without assets. It builds no kind, and the IPSW crates' tests read their pins from it.

## Where each staged pin is checked

| Pin | Test |
|---|---|
| `payload_size`, `payload_sha256` | `//install/img4:ipsw_test_<kind>`, against `//ipsw`'s decoded payloads |
| `aea_raw_size` | `//install/aea:header_test` (manual, network), the archive headers from Apple's URL |
| `container*` (21H461 OS) | `//install/ipsw:hdiutil_test` (manual), against `hdiutil`'s unwrapped container |
