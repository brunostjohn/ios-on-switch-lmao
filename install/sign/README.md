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
bazel test --config=ios17 //install/sign:installd_test    # against //drivers/SwitchInstallPolicy:installd
bazel test --config=ios27 //install/sign:installd_test
bazel build //install/sign:desktop                        # switch_sign_tool for the six desktop platforms
```

`installd_test` feeds the build's own `tools/install_policy/installd.py` patch through `installd_patch.py`, so it
checks the re-signing step on its own.
