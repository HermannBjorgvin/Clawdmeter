#!/bin/bash
# Wrapper to run PlatformIO inside Docker for this firmware project.
#
# Usage:
#   ./pio-docker.sh build [-e env]
#   ./pio-docker.sh upload [-e env] [port]        # default port /dev/ttyACM0
#   ./pio-docker.sh monitor [port]
#   ./pio-docker.sh clean [-e env]
#   ./pio-docker.sh shell                         # interactive shell in the container
#   ./pio-docker.sh -- <raw pio args>             # passthrough (no auto device bind)
#
# Examples:
#   ./pio-docker.sh build -e lilygo_t4s3
#   ./pio-docker.sh upload -e lilygo_t4s3 /dev/ttyACM0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMAGE_TAG="clawdmeter-pio:latest"
CACHE_VOLUME="clawdmeter-pio-cache"
DEFAULT_PORT="/dev/ttyACM0"

if ! docker image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
    echo "==> Building Docker image $IMAGE_TAG"
    docker build -t "$IMAGE_TAG" "$SCRIPT_DIR"
fi

docker volume inspect "$CACHE_VOLUME" >/dev/null 2>&1 || docker volume create "$CACHE_VOLUME" >/dev/null

cmd="${1:-build}"
shift || true

# Optional -e/--env <name>; collected into pio_env_args so it's only passed
# when the user actually specifies one (otherwise pio uses default_envs).
pio_env_args=()
new_args=()
while (($# > 0)); do
    case "$1" in
        -e|--env)
            if [[ $# -lt 2 ]]; then
                echo "Error: $1 requires an environment name" >&2
                exit 2
            fi
            pio_env_args=(-e "$2")
            shift 2
            ;;
        *)
            new_args+=("$1")
            shift
            ;;
    esac
done
set -- "${new_args[@]+"${new_args[@]}"}"

run_args=(
    --rm
    -v "$SCRIPT_DIR:/workspace"
    -v "$CACHE_VOLUME:/pio"
    -w /workspace
)

add_device() {
    local port="$1"
    if [[ ! -e "$port" ]]; then
        echo "Error: serial device $port not found." >&2
        echo "Plug the board in, or pass a different path." >&2
        exit 1
    fi
    run_args+=(--device "$port:$port")
}

case "$cmd" in
    build|run)
        docker run "${run_args[@]}" "$IMAGE_TAG" run "${pio_env_args[@]+"${pio_env_args[@]}"}"
        ;;
    upload|flash)
        port="${1:-$DEFAULT_PORT}"
        add_device "$port"
        docker run "${run_args[@]}" "$IMAGE_TAG" \
            run "${pio_env_args[@]+"${pio_env_args[@]}"}" -t upload --upload-port "$port"
        ;;
    monitor)
        port="${1:-$DEFAULT_PORT}"
        add_device "$port"
        docker run -it "${run_args[@]}" "$IMAGE_TAG" \
            device monitor --port "$port"
        ;;
    clean)
        docker run "${run_args[@]}" "$IMAGE_TAG" run "${pio_env_args[@]+"${pio_env_args[@]}"}" -t clean
        ;;
    shell)
        docker run -it --entrypoint /bin/bash "${run_args[@]}" "$IMAGE_TAG"
        ;;
    --)
        docker run "${run_args[@]}" "$IMAGE_TAG" "$@"
        ;;
    *)
        echo "Unknown command: $cmd" >&2
        echo "Usage: $0 {build|upload|monitor|clean|shell|--} [-e env] [port]" >&2
        exit 2
        ;;
esac
