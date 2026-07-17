#!/bin/bash
# Regression tests for compact host resource metrics sent with usage payloads.
set -u

DAEMON="$(dirname "$0")/../claude-usage-daemon.sh"
eval "$(awk '/^system_resources_fragment\(\) \{/{f=1} f{print} f&&/^\}/{exit}' "$DAEMON")"
eval "$(awk '/^build_payload_for_token\(\) \{/{f=1} f{print} f&&/^\}/{exit}' "$DAEMON")"

fails=0
check() {
    if [ "$2" = "$3" ]; then echo "PASS: $1"
    else echo "FAIL: $1"; echo "      expected [$2]"; echo "      got      [$3]"; fails=$((fails+1)); fi
}

# Overrides make the formatter deterministic; production leaves them unset and
# reads /proc, hwmon, and nvidia-smi/sysfs.
SYSTEM_CPU_PCT=42.6
SYSTEM_CPU_TEMP=67.4
SYSTEM_GPU_PCT=8
SYSTEM_GPU_TEMP=37
SYSTEM_RAM_PCT=63.2
frag=$(system_resources_fragment)
check "resource fragment rounds values" \
      ',"cpu":43,"ct":67,"gpu":8,"gt":37,"ram":63' "$frag"

SYSTEM_CPU_PCT=150
SYSTEM_GPU_PCT=-4
SYSTEM_RAM_PCT=101
SYSTEM_CPU_TEMP=none
SYSTEM_GPU_TEMP=none
frag=$(system_resources_fragment)
check "percentages clamp and unavailable temperatures use sentinel" \
      ',"cpu":100,"ct":-1,"gpu":0,"gt":-1,"ram":100' "$frag"

read_clock_setting() { echo off; }
read_chime_setting() { echo off; }
read_plan_label_for() { :; }
curl() {
    printf 'anthropic-ratelimit-unified-5h-utilization: 0.2\n'
    printf 'anthropic-ratelimit-unified-5h-reset: 4102444800\n'
    printf 'anthropic-ratelimit-unified-7d-utilization: 0.3\n'
    printf 'anthropic-ratelimit-unified-7d-reset: 4102444800\n'
    printf 'anthropic-ratelimit-unified-5h-status: allowed\n'
}
PAYLOAD_DIR=/tmp
CODEX_FRAGMENT=''
CODEX_CONTEXT_FRAGMENT=''   # build_payload_for_token references it; unset trips set -u
ANTIGRAVITY_FRAGMENT=''
SYSTEM_FRAGMENT=',"cpu":43,"ct":67,"gpu":8,"gt":37,"ram":63'
payload=$(build_payload_for_token token)
case "$payload" in
    *'"cpu":43,"ct":67,"gpu":8,"gt":37,"ram":63'*)
        echo "PASS: usage payload carries system resources" ;;
    *) echo "FAIL: usage payload omitted system resources [$payload]"; fails=$((fails+1)) ;;
esac

echo
if [ "$fails" -eq 0 ]; then echo "All system resource checks passed"; exit 0; fi
echo "$fails check(s) failed"
exit 1
