#!/usr/bin/env bash
# Build the Linux AppImage from a release binary.
#
#   tools/packaging/appimage.sh <clawdmeter binary> <output .AppImage> <work dir>
#
# linuxdeploy bundles the libraries the binary links, minus its excludelist
# (glibc, libxcb, ...) and minus libdbus-1 and the libsystemd chain under it:
# BlueZ needs the host's D-Bus anyway, and the host's GPU drivers load their
# own libzstd/liblzma/libsystemd, which must not be shadowed by older copies.
# Vulkan and the Wayland client are dlopen()ed by GPUI and come from the host
# with the drivers; nothing GPU-side goes into the image. libasound stays the
# host's too: its config and plugins (PipeWire / PulseAudio routing for the
# desktop chime) live on the host. Build on the oldest
# supported distro (release.yml uses ubuntu-22.04): the image's glibc floor is
# the builder's.
set -euo pipefail

bin=$(realpath "$1")
out=$(realpath -m "$2")
work=$(realpath -m "$3")
here=$(cd "$(dirname "$0")" && pwd)

# Pinned tools, checked by hash.
ld_url=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
ld_sha=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
plugin_url=https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-appimage-x86_64.AppImage
plugin_sha=992d502a248e14ab185448ddf6f6e7d25558cb84d4623c354c3af350c25fccb3

mkdir -p "$work/tools"
fetch() {
  local url=$1 sha=$2 dest
  dest=$work/tools/$(basename "$1")
  [ -f "$dest" ] || curl -fsSL -o "$dest" "$url"
  echo "$sha  $dest" | sha256sum -c --quiet
  chmod +x "$dest"
}
fetch "$ld_url" "$ld_sha"
fetch "$plugin_url" "$plugin_sha"

appdir=$work/AppDir
rm -rf "$appdir"
icons=()
for s in 16 24 32 48 64 128 256 512; do
  install -Dm644 "$here/icons/clawdmeter-$s.png" "$appdir/usr/share/icons/hicolor/${s}x$s/apps/clawdmeter.png"
  icons+=(--icon-file "$appdir/usr/share/icons/hicolor/${s}x$s/apps/clawdmeter.png")
done

# The runners have no FUSE; the tools unpack themselves instead. The plugin
# is found on PATH and names the image from $OUTPUT.
export APPIMAGE_EXTRACT_AND_RUN=1 NO_STRIP=1 OUTPUT="$out"
export PATH="$work/tools:$PATH"
(cd "$work" && linuxdeploy-x86_64.AppImage \
  --appdir "$appdir" \
  --executable "$bin" \
  --desktop-file "$here/linux/clawdmeter.desktop" \
  "${icons[@]}" \
  --exclude-library 'libdbus-1.so*' --exclude-library 'libsystemd.so*' \
  --exclude-library 'libgcrypt.so*' --exclude-library 'libgpg-error.so*' \
  --exclude-library 'liblz4.so*' --exclude-library 'liblzma.so*' \
  --exclude-library 'libzstd.so*' --exclude-library 'libcap.so*' \
  --exclude-library 'libasound.so*' \
  --output appimage)
ls -l "$out"
