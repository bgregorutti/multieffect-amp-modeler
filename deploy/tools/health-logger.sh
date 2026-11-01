#!/usr/bin/env bash
# One line of Pi health every INTERVAL seconds, to stdout (the journal, via
# multieffect-health-logger.service). With the persistent journal this is
# the record of what the Pi looked like just before a crash or a reboot:
#
#   journalctl -b -1 -u multieffect-health-logger | tail -20
#
# Fields: SoC temperature, firmware throttling flags (decoded), CPU 0
# frequency, load average, available memory, audio-engine CPU% and RSS.
set -u
INTERVAL="${1:-15}"

decode_throttled() {
  local v=$(( $1 )) out=()
  (( v & 0x1 ))     && out+=("UNDERVOLTAGE")
  (( v & 0x2 ))     && out+=("freq-capped")
  (( v & 0x4 ))     && out+=("THROTTLED")
  (( v & 0x8 ))     && out+=("temp-limit")
  (( v & 0x10000 )) && out+=("undervoltage-since-boot")
  (( v & 0x40000 )) && out+=("throttled-since-boot")
  (( v & 0x80000 )) && out+=("temp-limit-since-boot")
  [[ ${#out[@]} -eq 0 ]] && out=("ok")
  local IFS=,; echo "${out[*]}"
}

while true; do
  temp="?"
  [[ -r /sys/class/thermal/thermal_zone0/temp ]] && \
    temp=$(awk '{printf "%.1f", $1/1000}' /sys/class/thermal/thermal_zone0/temp)
  throttled="?"
  if command -v vcgencmd >/dev/null 2>&1; then
    raw=$(vcgencmd get_throttled 2>/dev/null | cut -d= -f2)
    [[ -n "${raw}" ]] && throttled="${raw}($(decode_throttled "${raw}"))"
  fi
  freq="?"
  [[ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq ]] && \
    freq=$(( $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) / 1000 ))
  load=$(cut -d' ' -f1-3 /proc/loadavg)
  mem=$(awk '/MemAvailable/ {printf "%d", $2/1024}' /proc/meminfo)
  engine="not-running"
  pid=$(pgrep -f 'build/audio_engine' | head -1 || true)
  [[ -n "${pid}" ]] && engine=$(ps -o pcpu=,rss= -p "${pid}" | awk '{printf "cpu=%s%% rss=%dMB", $1, $2/1024}')
  echo "temp=${temp}C throttled=${throttled} cpu0=${freq}MHz load=${load} mem_avail=${mem}MB engine: ${engine}"
  sleep "${INTERVAL}"
done
