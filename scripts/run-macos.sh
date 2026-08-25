#!/bin/bash
# Build, sign, and run Harmony Browser on macOS.
#
# The passkey platform authenticator answers only to an app that carries the
# web-browser entitlement, and an entitlement travels with a real signing
# identity — an ad-hoc binary is a browser with no identity, and WebKit
# answers accordingly.
#
# The executable is linked during a run, so the first launch stages it
# unsigned; this script quits that first launch, signs the staged binary,
# and starts again — a warm cache relinks nothing, so the signature stays.
# Without an Apple Development identity on the keychain the browser still
# runs, minus passkeys.
#
# Usage: scripts/run-macos.sh
set -euo pipefail
cd "$(dirname "$0")/.."

kira build browser

BIN="browser/.kira-build/main"
IDENTITY=$(security find-identity -v -p codesigning | awk '/Apple Development/ {print $2; exit}')

if [ -z "${IDENTITY:-}" ]; then
    echo "no Apple Development identity on the keychain: running unsigned (passkeys unavailable)"
    exec kira run --backend llvm browser
fi

# Already carrying a real signature? Then nothing to do but run.
SIGNED=$(codesign -dv "$BIN" 2>/dev/null | grep -c 'TeamIdentifier=[A-Z0-9]' || true)
if [ "${SIGNED:-0}" -ne 0 ]; then
    exec kira run --backend llvm browser
fi

# Stage once, quit, sign, start again.
kira run --backend llvm browser > /dev/null 2>&1 &
WRAPPER=$!
while [ ! -f "$BIN" ]; do sleep 2; done
sleep 3
pkill -f ".kira-build/main" || true
kill "$WRAPPER" 2>/dev/null || true
wait "$WRAPPER" 2>/dev/null || true

codesign --force --sign "$IDENTITY" --entitlements browser/macos/Entitlements.plist "$BIN"
echo "signed with $IDENTITY (passkeys available)"

exec kira run --backend llvm browser
