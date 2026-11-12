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
work=${3:-}
here=$(cd "$(dirname "$0")" && pwd)

scratch=
if [ -z "$work" ]; then
  work=$(mktemp -d)
  scratch=$work
fi
mnt="$work/mnt"
attached=0

# hdiutil sometimes reports "Resource busy" on hosted runners.
retry() {
  local i
  for i in 1 2 3; do
    "$@" && return 0
    [ "$i" = 3 ] || sleep 5
  done
  return 1
}

detach() {
  retry hdiutil detach "$mnt" -quiet || hdiutil detach "$mnt" -force -quiet
}

# shellcheck disable=SC2329  # run by the EXIT trap
cleanup() {
  if [ "$attached" = 1 ]; then
    detach || echo "couldn't unmount $mnt" >&2
  fi
  if [ -n "$scratch" ]; then
    rm -rf "$scratch"
  fi
}
trap cleanup EXIT

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

retry hdiutil create -volname "$volname" -srcfolder "$root" -fs HFS+ \
  -format UDZO -ov "$out"
hdiutil verify -quiet "$out"

# What the image holds: the app with its signature intact, the link and the
# window's layout.
mkdir -p "$mnt"
retry hdiutil attach "$out" -readonly -nobrowse -noautoopen -quiet -mountpoint "$mnt"
attached=1
status=0
codesign --verify --verbose "$mnt/Clawdmeter.app" || status=$?
for item in Applications .DS_Store .background/background.tiff; do
  [ -e "$mnt/$item" ] || [ -L "$mnt/$item" ] || { echo "the image has no $item" >&2; status=1; }
done
detach
attached=0
exit "$status"
