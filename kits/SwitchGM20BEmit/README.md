# SwitchGM20BEmit

GM20B push words, one set of functions for the kernel and user space: the push builder, the copy engine's rectangle
copy, 3D state (render targets, viewport, scissor, clears, blending), barriers and the draw macros. It sits on
`kits/SwitchGM20B:classes` and never on `:caps`, so an emitter cannot branch on what a format supports.

```sh
bazel test //kits/SwitchGM20BEmit/...
```

User space depends on `:emit` and `:macros_h`. A kernel driver names `kits/SwitchGM20B` and `kits/SwitchGM20BEmit` in
`hdr_deps`, adds `SWITCH_GM20B_EMIT_INCLUDES`, and compiles `EMIT_SOURCES`. `test:SwitchGM20BEmitKernelCheck` builds
the kit with the kernel toolchain and is never linked.

## Taken from Mesa

- `mesa/nv_push.h` is Mesa's `nv_push.h`, adapted. It uses `nv_push_compat.h` instead of `util/macros.h` and nothing
  from libc the kernel lacks. `P_IMMD` takes the one-word form whenever the value fits, as `raw::Push::m` does, so the
  words don't change with the optimisation level. It also names the copy, inline-to-memory and host subchannels.
- `gen/nv_push_cl*.h` is Mesa's `class_parser.py` output, unchanged except that stdio and the method-dump declarations
  are dropped (`mesa/kernel_safe_push_header.py`). `mesa_gen/` holds the untouched output, for Mesa's own sources built
  on the host.
- `mesa/nouveau_copy_rect.c` is NVK's `nouveau_copy_rect` on GM20B's copy class only, writing into the caller's push.
  Its origin methods continue the block-size method run, which saves one header word per block-linear side.
- `mme/switch_gm20b_macros.c` builds the raw path's macro ABI v1 with Mesa's MME builder around NVK's pre-Pascal draw
  loops. A zero instance count skips the loop, because Fermi's MME tests a loop's condition at its end. The indirect
  macros free their base registers before the loop, because the builder reuses a live register once all seven are
  taken.
- Mesa's MME simulator and decoder (`emit_mme_test`) are used from Mesa's tree.

The emitters are C because the generated helpers use C designated initialisers. C++ code calls them through the
prototypes. Without LTO a C++ caller pays a call per sequence; release builds inline them.

## Adding an emitter

Write it in C with `P_MTHD`/`P_IMMD` and the generated `P_NV<class>_*` helpers, give its header a word bound, and add
a test that compares its words with whatever the drivers emit today for the same inputs. Use word-for-word comparison
where the header forms agree, and method-for-method (`SwitchGM20BPushDecode.h`) where they don't. When a raw sequence
is written inline in `raw_gpu.cpp`, the test replays it through `raw::Push` with `raw_gpu.cpp`'s own constants and
checks that the replayed lines are still in that file.
