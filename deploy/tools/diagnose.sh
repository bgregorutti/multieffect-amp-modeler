#!/usr/bin/env bash
# Collect everything useful for investigating a problem on the pedal into
# one text file you can copy off the Pi and share:
#
#   sudo ./deploy/tools/diagnose.sh            # writes ~/multieffect-diag-<date>.txt
#   scp pi@<pedal>:multieffect-diag-*.txt .
#
# Run it as soon as possible after the problem (sound dropout, crash and
# reboot, app disconnects): the "previous boot" sections are only there if
# the persistent journal is on (install.sh sets it up).
set -u
REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DATA_DIR="${DATA_DIR:-/var/lib/multieffect-amp-modeler}"
HOME_DIR="$(getent passwd "${SUDO_USER:-$USER}" | cut -d: -f6)"
OUT="${HOME_DIR:-/tmp}/multieffect-diag-$(date +%Y%m%d-%H%M%S).txt"
UNITS="multieffect-audio-engine multieffect-control-daemon multieffect-display-client multieffect-health-logger"

[[ $EUID -eq 0 ]] || echo "note: not root -- other services' logs may be missing (run with sudo)" >&2

section() { printf '\n===== %s =====\n' "$*"; }
run() { printf '$ %s\n' "$*"; "$@" 2>&1; }
# Engine logs minus ALSA's harmless device-probe noise.
engine_log() { journalctl "$@" -u multieffect-audio-engine --no-pager 2>&1 | grep -v "ALSA lib"; }

{
  section "When / what"
  date
  run git -C "${REPO_DIR}" log --oneline -3
  run git -C "${REPO_DIR}" status --short
  cat /proc/device-tree/model 2>/dev/null; echo
  grep PRETTY_NAME /etc/os-release
  run uname -a
  run uptime

  section "Power and temperature"
  if command -v vcgencmd >/dev/null; then
    run vcgencmd get_throttled
    echo "  bits: 0x1 under-voltage now, 0x2 freq capped, 0x4 throttled, 0x8 temp limit;"
    echo "        0x10000/0x20000/0x40000/0x80000 = same, at some point since boot"
    run vcgencmd measure_temp
    run vcgencmd measure_volts core
  fi
  for f in /sys/devices/system/cpu/cpu0/cpufreq/scaling_{governor,cur_freq,max_freq}; do
    [[ -r $f ]] && echo "$(basename "$f"): $(cat "$f")"
  done

  section "Memory and disk"
  run free -m
  run df -h / "${DATA_DIR}"

  section "Boots (a crash shows up as a boot whose log just stops)"
  if [[ -d /var/log/journal ]]; then echo "persistent journal: yes"; else echo "persistent journal: NO -- previous boots are lost"; fi
  run journalctl --list-boots --no-pager

  section "Previous boot: last 60 lines of everything"
  journalctl -b -1 -n 60 --no-pager 2>&1
  section "Previous boot: kernel power/thermal/USB/OOM/watchdog events"
  journalctl -k -b -1 --no-pager 2>&1 | grep -iE "voltage|thermal|oom|out of memory|panic|watchdog|usb .*disconnect|xhci|snd-usb|brcmfmac" | tail -40
  section "Previous boot: health logger (last 20)"
  journalctl -b -1 -u multieffect-health-logger -n 20 --no-pager 2>&1

  section "This boot: services"
  for u in ${UNITS}; do
    systemctl show "${u}" -p ActiveState -p SubState -p NRestarts -p ExecMainStartTimestamp 2>/dev/null | paste -sd' '
    echo "  $u"
  done
  run systemctl --failed --no-pager

  section "This boot: audio engine (ALSA probe noise removed)"
  engine_log -b | tail -60
  section "Audio engine threads (callback should be cls=FF with an rtprio)"
  pid=$(pgrep -f 'build/audio_engine' | head -1)
  [[ -n "${pid}" ]] && run ps -L -o tid,cls,rtprio,psr,pcpu,comm -p "${pid}"
  section "This boot: control daemon (last 80)"
  journalctl -b -u multieffect-control-daemon -n 80 --no-pager 2>&1
  section "This boot: kernel warnings"
  journalctl -k -b -p warning -n 40 --no-pager 2>&1
  section "This boot: health logger (last 20)"
  journalctl -b -u multieffect-health-logger -n 20 --no-pager 2>&1

  section "Audio devices"
  run aplay -l
  run arecord -l
  command -v lsusb >/dev/null && run lsusb

  section "Boot configuration"
  for f in /boot/firmware/config.txt /boot/config.txt; do
    [[ -f $f ]] && { echo "--- $f (non-comment lines)"; grep -vE '^\s*(#|$)' "$f"; break; }
  done
  for f in /boot/firmware/cmdline.txt /boot/cmdline.txt; do
    [[ -f $f ]] && { echo "--- $f"; cat "$f"; break; }
  done

  section "Assets (NAM/IR CPU cost)"
  if [[ -f "${DATA_DIR}/state.json" ]]; then
    python3 -I "${REPO_DIR}/scripts/check_assets.py" --state "${DATA_DIR}/state.json" 2>&1
  else
    echo "no ${DATA_DIR}/state.json"
  fi
} > "${OUT}" 2>&1

[[ -n "${SUDO_USER:-}" ]] && chown "${SUDO_USER}:" "${OUT}"
echo "wrote ${OUT} ($(wc -l < "${OUT}") lines)"
