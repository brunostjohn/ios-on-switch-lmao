#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
tool=$PWD/$1
work=$(mktemp -d "${TEST_TMPDIR:-/tmp}/cms_test.XXXXXX")
trap 'rm -rf "$work"' EXIT

cat >"$work/profile.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Name</key>
    <string>Switch Test Profile</string>
    <key>Entitlements</key>
    <dict>
        <key>application-identifier</key>
        <string>ABCDE12345.com.brunostjohn.switch.installer</string>
    </dict>
</dict>
</plist>
EOF

cat >"$work/no_ent.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Name</key>
    <string>Missing Entitlements</string>
</dict>
</plist>
EOF

/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$work/key.pem" -out "$work/cert.pem" \
    -subj "/CN=Switch CMS Test" -days 1 >/dev/null 2>&1

/usr/bin/openssl cms -sign -nodetach -binary -outform DER \
    -signer "$work/cert.pem" -inkey "$work/key.pem" \
    -in "$work/profile.plist" -out "$work/valid.mobileprovision"

/usr/bin/openssl cms -sign -nodetach -binary -outform DER \
    -signer "$work/cert.pem" -inkey "$work/key.pem" \
    -in "$work/no_ent.plist" -out "$work/no_ent.mobileprovision"

# Cross-check with openssl cms -verify and macOS security cms -D (using a temp keychain).
/usr/bin/openssl cms -verify -noverify -inform DER \
    -in "$work/valid.mobileprovision" -out "$work/openssl_decoded.plist" >/dev/null 2>&1
cmp -s "$work/profile.plist" "$work/openssl_decoded.plist"

/usr/bin/security create-keychain -p "" "$work/test.keychain"
/usr/bin/security cms -D -k "$work/test.keychain" -i "$work/valid.mobileprovision" >"$work/decoded.plist"
cmp -s "$work/profile.plist" "$work/decoded.plist"

"$tool" verify-profile "$work/valid.mobileprovision"

if "$tool" verify-profile "$work/profile.plist" >/dev/null 2>&1; then
    echo "FAIL: plain XML plist was accepted" >&2
    exit 1
fi

if "$tool" verify-profile "$work/no_ent.mobileprovision" >/dev/null 2>&1; then
    echo "FAIL: profile without Entitlements was accepted" >&2
    exit 1
fi
