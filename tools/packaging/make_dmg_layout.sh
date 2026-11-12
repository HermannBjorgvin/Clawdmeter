#!/usr/bin/env bash
# Write macos/dmg-DS_Store: the Finder settings of the disk image's window
# (size, icon view, icon positions, the background picture), which dmg.sh
# copies into every image as its .DS_Store.
#
#   tools/packaging/make_dmg_layout.sh
#
# Run it on a Mac when make_dmg_background.py's sizes or positions change,
# and commit the result. It mounts a scratch volume named Clawdmeter and has
# Finder lay its window out, so Finder shows that window for a moment and may
# ask to be automated. Only what macOS ships, plus python3 for the numbers.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
volume=/Volumes/Clawdmeter
[ ! -e "$volume" ] || { echo "$volume is mounted; eject it first" >&2; exit 1; }

read -r width height app_x applications_x icon_y \
  < <(python3 "$here/make_dmg_background.py" --layout)
# Finder's window bounds include the title bar; the picture fills the rest.
title_bar=28
left=200
top=120

# A fixed scratch folder: Finder records the image's path in the layout, and
# that file ships in every release.
work=/tmp/clawdmeter-dmg-layout
rm -rf "$work"
mkdir -p "$work"

cleanup() {
  if [ -e "$volume" ]; then
    hdiutil detach "$volume" -quiet 2>/dev/null \
      || { sleep 3; hdiutil detach "$volume" -quiet 2>/dev/null; } \
      || hdiutil detach "$volume" -force -quiet \
      || echo "couldn't eject $volume; eject it before building an image" >&2
  fi
  rm -rf "$work"
}
trap cleanup EXIT

# The same names dmg.sh puts in the image; the app can be an empty folder.
root="$work/root"
mkdir -p "$root/Clawdmeter.app" "$root/.background"
ln -s /Applications "$root/Applications"
tiffutil -cathidpicheck "$here/macos/dmg-background.png" \
  "$here/macos/dmg-background@2x.png" -out "$root/.background/background.tiff"
hdiutil create -quiet -volname Clawdmeter -srcfolder "$root" -fs HFS+ \
  -format UDRW "$work/layout.dmg"
hdiutil attach "$work/layout.dmg" -noautoopen -quiet

osascript <<SCRIPT
tell application "Finder"
  tell disk "Clawdmeter"
    open
    set w to container window
    set current view of w to icon view
    set toolbar visible of w to false
    set statusbar visible of w to false
    set bounds of w to {$left, $top, $((left + width)), $((top + height + title_bar))}
    set o to icon view options of w
    set arrangement of o to not arranged
    set icon size of o to 128
    set text size of o to 13
    set background picture of o to file ".background:background.tiff"
    set position of item "Clawdmeter.app" to {$app_x, $icon_y}
    set position of item "Applications" to {$applications_x, $icon_y}
    close
    open
    update without registering applications
    delay 2
    close
  end tell
end tell
SCRIPT

# Finder writes the file a moment after the window closes.
sync
for _ in 1 2 3 4 5 6 7 8 9 10; do
  [ -s "$volume/.DS_Store" ] && break
  sleep 1
done
[ -s "$volume/.DS_Store" ] || { echo "Finder wrote no .DS_Store" >&2; exit 1; }
cp "$volume/.DS_Store" "$here/macos/dmg-DS_Store"
# Finder also records which volumes the picture was on: the scratch one and
# this Mac's own disk, by UUID. Neither exists where the image is opened
# (the picture is found by its path), so blank them, keeping the length.
perl -0777 -pi -e 's/[0-9A-F]{8}(-[0-9A-F]{4}){3}-[0-9A-F]{12}/00000000-0000-0000-0000-000000000000/g' \
  "$here/macos/dmg-DS_Store"
echo "wrote tools/packaging/macos/dmg-DS_Store"
