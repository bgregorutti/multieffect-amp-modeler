#!/usr/bin/env bash
# Stop and remove what install.sh installed. By default this is
# conservative: it stops/disables the services and removes the systemd
# units it created, but LEAVES the persistent data directory (presets,
# banks, uploaded assets) and the Wi-Fi AP connection profile in place, so
# a re-run of install.sh picks up right where you left off. Pass --purge to
# also remove those.
#
#   sudo ./deploy/uninstall.sh [--purge]
set -euo pipefail

PURGE="false"
DATA_DIR="/var/lib/multieffect-amp-modeler"

log()  { echo "[deploy] $*"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --purge) PURGE="true"; shift ;;
    -h|--help)
      echo "Usage: sudo $0 [--purge]"
      echo "  --purge   Also delete ${DATA_DIR} (presets/assets) and the"
      echo "            'multieffect-ap' NetworkManager connection profile."
      exit 0 ;;
    *) echo "[deploy] ERROR: unknown option: $1" >&2; exit 1 ;;
  esac
done

[[ $EUID -eq 0 ]] || { echo "[deploy] ERROR: must be run as root: sudo $0" >&2; exit 1; }

for unit in multieffect-control-daemon multieffect-audio-engine; do
  if systemctl list-unit-files "${unit}.service" >/dev/null 2>&1; then
    log "stopping/disabling ${unit}"
    systemctl disable --now "${unit}.service" 2>/dev/null || true
    rm -f "/etc/systemd/system/${unit}.service"
  fi
done
systemctl daemon-reload

if [[ "${PURGE}" == "true" ]]; then
  log "removing data directory ${DATA_DIR}"
  rm -rf "${DATA_DIR}"
  if command -v nmcli >/dev/null 2>&1; then
    log "removing Wi-Fi AP connection profile 'multieffect-ap'"
    nmcli connection delete multieffect-ap >/dev/null 2>&1 || true
  fi
else
  log "leaving ${DATA_DIR} and the Wi-Fi AP profile in place (pass --purge to remove them too)"
fi

log "done. audio-engine/build and control-daemon/.venv build artifacts were left in place; remove them by hand if desired."
