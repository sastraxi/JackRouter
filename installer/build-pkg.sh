#!/usr/bin/env bash
# Build a notarized JackBridge installer .pkg.
#
# Usage:
#   ./installer/build-pkg.sh [version]
#
# Local dev (ad-hoc): no env vars; produces an unsigned, un-notarized .pkg
# suitable for testing the layout on the build machine only.
#
# Release: set all of
#   SIGN_APP_IDENTITY     "Developer ID Application: <Team> (XXXXXXXXXX)"
#   SIGN_INSTALLER_IDENTITY "Developer ID Installer: <Team> (XXXXXXXXXX)"
#   NOTARY_PROFILE        keychain profile name (xcrun notarytool store-credentials)
# Optional:
#   SKIP_NOTARIZE=1       sign but do not submit (smoke-test the signing chain)

set -euo pipefail

VERSION="${1:-0.1.0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INSTALLER="$ROOT/installer"
BUILD="$INSTALLER/build"
STAGING="$BUILD/staging"
SCRIPTS="$INSTALLER/scripts"

PKG_ID="com.jackbridge.pkg"
PKG_OUT="$BUILD/JackBridge-$VERSION.pkg"

rm -rf "$BUILD"
mkdir -p "$STAGING/Library/Audio/Plug-Ins/HAL"
mkdir -p "$STAGING/Library/Application Support/JackBridge"
mkdir -p "$STAGING/Library/LaunchAgents"

XCBUILD_ARGS=(
    -project "$ROOT/driver/JackBridgePlugIn.xcodeproj"
    -configuration Release
    CONFIGURATION_BUILD_DIR="$BUILD/xcode"
)
if [[ -n "${SIGN_APP_IDENTITY:-}" ]]; then
    XCBUILD_ARGS+=(CODE_SIGN_IDENTITY="$SIGN_APP_IDENTITY" CODE_SIGN_STYLE=Manual OTHER_CODE_SIGN_FLAGS="--timestamp")
fi

echo "==> Building driver + daemon"
xcodebuild "${XCBUILD_ARGS[@]}" -target JackBridgePlugIn clean build >/dev/null
xcodebuild "${XCBUILD_ARGS[@]}" -target JackBridged build >/dev/null

cp -R "$BUILD/xcode/JackBridgePlugIn.driver" "$STAGING/Library/Audio/Plug-Ins/HAL/"
cp    "$BUILD/xcode/JackBridged"             "$STAGING/Library/Application Support/JackBridge/"
install -m 0755 "$INSTALLER/jackd-launch"    "$STAGING/Library/Application Support/JackBridge/jackd-launch"
install -m 0644 "$INSTALLER/launchagents/com.jackbridge.daemon.plist" "$STAGING/Library/LaunchAgents/"
install -m 0644 "$INSTALLER/launchagents/com.jackbridge.jackd.plist"  "$STAGING/Library/LaunchAgents/"

echo "==> pkgbuild (component)"
COMPONENT_PKG="$BUILD/JackBridge-component.pkg"
pkgbuild \
    --root "$STAGING" \
    --identifier "$PKG_ID" \
    --version "$VERSION" \
    --install-location / \
    --scripts "$SCRIPTS" \
    "$COMPONENT_PKG"

echo "==> productbuild (distribution)"
DIST_XML="$BUILD/distribution.xml"
sed -e "s/@VERSION@/$VERSION/g" "$INSTALLER/distribution.xml.in" > "$DIST_XML"

PRODUCTBUILD_ARGS=(--distribution "$DIST_XML" --package-path "$BUILD")
if [[ -n "${SIGN_INSTALLER_IDENTITY:-}" ]]; then
    PRODUCTBUILD_ARGS+=(--sign "$SIGN_INSTALLER_IDENTITY")
fi
productbuild "${PRODUCTBUILD_ARGS[@]}" "$PKG_OUT"

if [[ -n "${NOTARY_PROFILE:-}" && -z "${SKIP_NOTARIZE:-}" ]]; then
    echo "==> notarize + staple"
    xcrun notarytool submit "$PKG_OUT" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$PKG_OUT"
else
    echo "==> skipping notarization (NOTARY_PROFILE unset or SKIP_NOTARIZE=1)"
fi

echo "==> Done: $PKG_OUT"
