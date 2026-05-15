FROM python:3.11-slim

# System deps PlatformIO uses for fetching, building, and flashing.
# udev so esptool can enumerate USB serial devices.
RUN apt-get update && apt-get install -y --no-install-recommends \
        git \
        ca-certificates \
        curl \
        srecord \
        udev \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir platformio==6.1.19

# Build/cache directories live in a named volume mounted at /pio.
ENV PLATFORMIO_CORE_DIR=/pio/.platformio
ENV PLATFORMIO_WORKSPACE_DIR=/workspace/.pio
WORKDIR /workspace

ENTRYPOINT ["pio"]
CMD ["run"]
