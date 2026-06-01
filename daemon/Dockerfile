FROM python:3.11-slim

# ca-certificates: outbound HTTPS to api.anthropic.com.
# bleak talks to BlueZ over D-Bus via the pure-Python dbus-fast wheel,
# so no system dbus-dev / bluez packages are needed inside the container.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir "bleak>=0.22" "httpx>=0.27"

WORKDIR /app
COPY claude_usage_daemon.py /app/

ENTRYPOINT ["python3", "-u", "/app/claude_usage_daemon.py"]
