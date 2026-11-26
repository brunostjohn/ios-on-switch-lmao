# switch_translate

iOS 27's pretranslation for the installer. Each role's delta is made from Apple's decoded IPSW member, byte for byte
as `//install:delta_<role>` makes it with `translate_tree.py` and the `arm64e` CLI. The member is read in process
(`switch_apfs_read`, `switch_decmpfs`) without mounting it.

```sh
bazel run -c opt //install/translate:switch_translate_tool -- role --role os --image <decoded member> \
    --ipsw-pins <//install:ipsw_pins> --apple-trustcache <IM4P> --edits <edits_<build>.json> \
    --pins <//ipsw:pretranslated_pins> --translator <version> --out <dir>
```

The SystemOS cryptex also takes `--cdhash-pairs` from the OS and AppOS deltas. A delta is kept only if its trust
module, and for the SystemOS cryptex its cache list, hash to the release's pins. If the compiler on the user's machine
produces bytes that differ from CI's, the run leaves no delta.

The C is `translate/arm64e`'s, compiled for each desktop platform. Two files in `c/` keep its output equal to CI's
CLI:

- `c/plan.c` builds `tools/a64plan2.c` unchanged. It serves the planner's file calls, printing and `exit` in process
  and resets its statics between runs. An edit to `a64plan2.c` itself changes the translator version and every delta.
- `c/sort.c` is macOS's own qsort. The planner's comparators tie, so any other sort gives a different plan.

The tests that read the IPSW's members are manual and need `--config=ios27` on a build host. BUILD.bazel lists them.
