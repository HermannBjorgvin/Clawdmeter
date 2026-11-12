#!/usr/bin/env bash
# Build the macOS disk image: Clawdmeter.app beside an Applications link, in
# a window laid out by macos/dmg-DS_Store over the picture from
# make_dmg_background.py.
#
#   tools/packaging/dmg.sh <Clawdmeter.app> <out.dmg> [workdir]
#
# The app must already be signed; the image carries it as it is. Uses only
# what macOS ships (ditto, tiffutil, hdiutil, codesign).
set -euo pipefail

app=$1
out=$2
work=${3:-$(mktemp -d)}
here=$(cd "$(dirname "$0")" && pwd)

# The layout names these: the volume, the app and the picture's path.
volname=Clawdmeter
root="$work/dmgroot"
rm -rf "$root"
mkdir -p "$root/.background" "$(dirname "$out")"
ditto "$app" "$root/Clawdmeter.app"
ln -s /Applications "$root/Applications"
cp "$here/macos/dmg-DS_Store" "$root/.DS_Store"
# One TIFF holding the picture at 1x and 2x, which Finder picks from.
tiffutil -cathidpicheck "$here/macos/dmg-background.png" \
  "$here/macos/dmg-background@2x.png" -out "$root/.background/background.tiff"

# hdiutil sometimes reports "Resource busy" on hosted runners.
for i in 1 2 3; do
  hdiutil create -volname "$volname" -srcfolder "$root" -fs HFS+ \
    -format UDZO -ov "$out" && break
  [ "$i" = 3 ] && exit 1
  sleep 5
done
hdiutil verify -quiet "$out"

# What the image holds: the app with its signature intact, the link and the
# window's layout.
mnt="$work/mnt"
mkdir -p "$mnt"
hdiutil attach "$out" -readonly -nobrowse -noautoopen -quiet -mountpoint "$mnt"
status=0
codesign --verify --verbose "$mnt/Clawdmeter.app" || status=$?
for item in Applications .DS_Store .background/background.tiff; do
  [ -e "$mnt/$item" ] || [ -L "$mnt/$item" ] || { echo "the image has no $item" >&2; status=1; }
done
hdiutil detach "$mnt" -quiet
exit "$status"
