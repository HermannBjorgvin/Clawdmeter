#!/usr/bin/env bash
# Capture one screenshot of the desktop app's demo window under X11.
#
#   capture.sh OUT.png [demo flags...]
#   capture.sh /tmp/device.png --page device
#   CLAWDMETER_DECORATIONS=client capture.sh /tmp/gnome.png --page overview
#
# Runs the demo example from a throwaway HOME (with the binary copied into it,
# so hook commands in the Hooks preview show no personal paths), waits for the
# window, waits WAIT seconds more (default 5: on X11 the dark appearance
# arrives from the portal after the first frames), captures the client window
# with ImageMagick `import`, and closes the demo.
#
# Env: DEMO_BIN (default desktop/target/debug/examples/demo, built if
# missing), WAIT (seconds, default 5), DISPLAY (default :0).
# Needs: X11 or XWayland, xwininfo (x11-utils), import (ImageMagick).
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "usage: $0 OUT.png [demo flags...]" >&2
  exit 2
fi
out=$(realpath -m "$1"); shift

repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
bin=${DEMO_BIN:-$repo/desktop/target/debug/examples/demo}
wait_s=${WAIT:-5}

for tool in xwininfo import; do
  command -v "$tool" >/dev/null || { echo "missing $tool (x11-utils / imagemagick)" >&2; exit 1; }
done
if [ ! -x "$bin" ]; then
  echo "building the demo example…" >&2
  (cd "$repo/desktop" && cargo build -p clawdmeter-app --example demo --locked) >&2
fi

scratch=$(mktemp -d)
pid=
cleanup() { [ -n "$pid" ] && kill "$pid" 2>/dev/null; rm -rf "$scratch"; }
trap cleanup EXIT
cp "$bin" "$scratch/demo"

env -u WAYLAND_DISPLAY -u XDG_CONFIG_HOME -u XDG_DATA_HOME -u XDG_STATE_HOME \
  DISPLAY="${DISPLAY:-:0}" HOME="$scratch" XDG_CACHE_HOME="$scratch/.cache" \
  "$scratch/demo" "$@" >"$scratch/demo.log" 2>&1 &
pid=$!

id=
for _ in $(seq 1 60); do
  id=$(xwininfo -root -tree 2>/dev/null | grep '"Clawdmeter": ("clawdmeter"' | awk '{print $1}' | head -n1 || true)
  [ -n "$id" ] && break
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "demo exited before showing a window; its log:" >&2
    cat "$scratch/demo.log" >&2
    exit 1
  fi
  sleep 0.5
done
if [ -z "$id" ]; then
  echo "no Clawdmeter window after 30 s (is DISPLAY=${DISPLAY:-:0} an X server?)" >&2
  exit 1
fi

sleep "$wait_s"
import -window "$id" "$out"
echo "$out"
