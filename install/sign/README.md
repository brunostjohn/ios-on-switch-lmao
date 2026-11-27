# switch_sign

Ad hoc code signing that gives the same bytes as `/usr/bin/codesign -s -`. The installer runs it on Windows, Linux and
macOS for two files:

- `installd`, whose CDHash goes into the trust bundle;
- the iOS 17 launchd catalogue's detached signature.

Both reach `STAGE_ID`, so a signature that differs from CI's by one byte won't boot.

These details come from measuring `codesign` on macOS 27, and it refuses inputs the measurements don't cover:

- the space reserved for the signature;
- page sizes;
- leftover bytes after the SuperBlob;
- CoreEntitlements key order.

```sh
bazel test //install/sign:darwin_test                     # against codesign itself (needs a Darwin worker)
bazel build //install/sign:desktop                        # switch_sign_tool for the six desktop platforms
```

`//install/installd:installd_test` checks the `installd` signature against `//drivers/SwitchInstallPolicy:installd`
for each kind, together with the `LC_LOAD_DYLIB` patch that comes before it.
