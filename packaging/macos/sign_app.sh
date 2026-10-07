#!/usr/bin/env bash
# Signs Pychron.app for distribution: every piece of code inside out, with the
# hardened runtime (notarization requires it), the programs with
# packaging/macos/entitlements.plist.
#
#   sign_app.sh <Pychron.app> <identity> <entitlements.plist>
#
# <identity> is a "Developer ID Application: ..." certificate in a keychain,
# or "-" to sign ad hoc (no certificate: a build that is not distributed, and
# CI without the signing secrets, which still checks that everything signs and
# runs under the hardened runtime).
#
# Called by CPack before the disk image is made (packaging/macos/cpack_sign.cmake);
# docs/installation_runbook.md, "Signing and notarizing the macOS package".
set -euo pipefail

app=${1:?the .app to sign}
identity=${2:?a signing identity, or - for ad hoc}
entitlements=${3:?the entitlements file}

if [ "$identity" = "-" ]; then
  timestamp=(--timestamp=none)  # an ad hoc signature cannot be timestamped
else
  timestamp=(--timestamp)  # notarization requires a secure timestamp
fi
sign() { codesign --force --sign "$identity" "${timestamp[@]}" --options runtime "$@"; }

# Deepest paths first: what a bundle holds is signed before the bundle.
deepest_first() { awk -F/ '{ print NF "\t" $0 }' | sort -t$'\t' -k1,1nr -k2 | cut -f2-; }

contents="$app/Contents"
macos="$contents/MacOS"
main=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$contents/Info.plist")

# 1. Every library, plugin and framework binary: the Qt runtime, the bundled
#    Python's library and extension modules. Python sources are skipped only
#    to spare `file` thousands of looks.
count=0
while IFS= read -r f; do
  case "$f" in "$macos"/*) continue ;; esac
  if file -b "$f" | grep -q '^Mach-O'; then
    sign "$f"
    count=$((count + 1))
  fi
done < <(find "$contents" -type f ! -name '*.py' ! -name '*.pyc' ! -name '*.pyi' | deepest_first)

# 2. The frameworks themselves, which seal their resources.
while IFS= read -r fw; do
  sign "$fw"
done < <(find "$contents" -type d -name '*.framework' | deepest_first)

# 3. The programs beside the main one (elctl), then the bundle, whose
#    signature is its main program's.
for exe in "$macos"/*; do
  [ -f "$exe" ] && [ "$(basename "$exe")" != "$main" ] || continue
  sign --entitlements "$entitlements" "$exe"
done
sign --entitlements "$entitlements" "$app"

codesign --verify --deep --strict --verbose=2 "$app"
echo "signed $app ($count libraries) as: $identity"
