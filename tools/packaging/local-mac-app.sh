#!/usr/bin/env bash
# Build Clawdmeter.app for this Mac from the working tree (the fork's local
# install; the release workflow builds the universal one the same way).
#
#   tools/packaging/local-mac-app.sh [out-dir]     default: desktop/target/dist
#
# An app bundle, not a bare binary: CoreBluetooth needs
# NSBluetoothAlwaysUsageDescription in Info.plist. Ad-hoc signed.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-"$root/desktop/target/dist"}
app="$out/Clawdmeter.app"

(cd "$root/desktop" && cargo build --release -p clawdmeter-app)
bin="$root/desktop/target/release/clawdmeter"
ver=$("$bin" --version | awk '{print $2}')

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin" "$app/Contents/MacOS/clawdmeter"
cp "$root/tools/packaging/icons/clawdmeter.icns" "$app/Contents/Resources/"
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIconFile</key><string>clawdmeter</string>
  <key>CFBundleIdentifier</key><string>com.clawdmeter.app</string>
  <key>CFBundleName</key><string>Clawdmeter</string>
  <key>CFBundleExecutable</key><string>clawdmeter</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$ver</string>
  <key>CFBundleVersion</key><string>$ver</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>LSUIElement</key><true/>
  <key>NSBluetoothAlwaysUsageDescription</key><string>Clawdmeter sends your Claude Code usage to your Clawdmeter over Bluetooth.</string>
</dict></plist>
PLIST
plutil -lint "$app/Contents/Info.plist"
codesign --force --sign - "$app"
codesign --verify --verbose "$app"
echo "$app"
