#!/usr/bin/env bash
# Provision a Raspberry Pi (Raspberry Pi OS Bookworm or newer, 64-bit
# recommended) to run the control-daemon and audio-engine as systemd
# services, and (unless --skip-ap) turn its Wi-Fi radio into an access
# point the mobile app connects to directly, per ARCHITECTURE.md's
# "Connectivity" decision.
#
# Idempotent: safe to re-run after `git pull` to rebuild + redeploy, or
# after changing deploy/config.env. Must run as root (systemd units, apt,
# NetworkManager, and a dedicated system user all need it):
#
#   sudo ./deploy/install.sh [options]
#
# See deploy/README.md for the full step-by-step guide, what this script
# does NOT do (flashing the SD card, real-time kernel, wiring real audio
# hardware), and troubleshooting.
set -euo pipefail

# --------------------------------------------------------------------------
# Defaults (overridable by deploy/config.env, then by CLI flags -- CLI wins)
# --------------------------------------------------------------------------
SERVICE_USER="ampmodeler"
DATA_DIR="/var/lib/multieffect-amp-modeler"
WIFI_SSID="MultiEffectPedal"
WIFI_PASSWORD=""
WIFI_COUNTRY=""
WIFI_IFACE="wlan0"
SKIP_AP="false"
ENABLE_RT_KERNEL="false"
SKIP_BUILD="false"

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

log()  { echo "[deploy] $*"; }
warn() { echo "[deploy] WARNING: $*" >&2; }
die()  { echo "[deploy] ERROR: $*" >&2; exit 1; }

usage() {
  cat <<EOF
Usage: sudo $0 [options]

Options:
  --ssid NAME             Wi-Fi AP SSID (default: ${WIFI_SSID})
  --wifi-password PASS    Wi-Fi AP password, >= 8 chars (default: random,
                           printed once at the end -- save it)
  --wifi-country CC       2-letter Wi-Fi regulatory country code (e.g. FR,
                           US). Required by the Pi's radio to transmit at
                           all; without it the AP step may silently not
                           broadcast. No default -- set it explicitly, or
                           pre-set it yourself via 'raspi-config'.
  --wifi-iface IFACE      Wi-Fi interface to turn into an AP (default: ${WIFI_IFACE})
  --skip-ap               Don't touch Wi-Fi/NetworkManager at all (use this
                           if you're managing networking yourself, or
                           testing this script on a non-Pi machine).
  --skip-build             Skip rebuilding audio-engine / reinstalling the
                           control-daemon venv -- only (re)install systemd
                           units and restart services. Useful for a config-
                           only re-run.
  --enable-rt-kernel        Also install the PREEMPT_RT kernel package
                             (linux-image-rt-arm64) for the latency
                             benchmarking mentioned in docs/open-questions.md.
                             NOT enabled by default: it changes what kernel
                             boots and needs a manual reboot + verification
                             you actually want (see deploy/README.md).
  --service-user NAME       System user the services run as (default: ${SERVICE_USER})
  -h, --help                 Show this help.

Settings can also be placed in deploy/config.env (copy from
deploy/config.env.example) -- CLI flags always take precedence over it.
EOF
}

# --------------------------------------------------------------------------
# 0. Load deploy/config.env if present, then parse CLI flags over it.
# --------------------------------------------------------------------------
if [[ -f "${REPO_DIR}/deploy/config.env" ]]; then
  log "loading ${REPO_DIR}/deploy/config.env"
  # shellcheck source=/dev/null
  source "${REPO_DIR}/deploy/config.env"
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ssid) WIFI_SSID="$2"; shift 2 ;;
    --wifi-password) WIFI_PASSWORD="$2"; shift 2 ;;
    --wifi-country) WIFI_COUNTRY="$2"; shift 2 ;;
    --wifi-iface) WIFI_IFACE="$2"; shift 2 ;;
    --skip-ap) SKIP_AP="true"; shift ;;
    --skip-build) SKIP_BUILD="true"; shift ;;
    --enable-rt-kernel) ENABLE_RT_KERNEL="true"; shift ;;
    --service-user) SERVICE_USER="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
  esac
done

[[ $EUID -eq 0 ]] || die "must be run as root: sudo $0 [options]"

ARCH="$(uname -m)"
case "$ARCH" in
  aarch64|armv7l|armv6l) ;;
  *) warn "uname -m reports '$ARCH', not an ARM architecture -- this script" \
          "targets a Raspberry Pi. Continuing anyway (e.g. useful for" \
          "dry-running the non-network steps), but expect surprises." ;;
esac

log "repo directory: ${REPO_DIR}"

# --------------------------------------------------------------------------
# 1. System packages
# --------------------------------------------------------------------------
log "apt-get update && install build/runtime dependencies"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
  python3 python3-venv python3-pip \
  build-essential cmake pkg-config nlohmann-json3-dev \
  network-manager \
  ca-certificates curl

# --------------------------------------------------------------------------
# 2. Dedicated, unprivileged system user to run both services
# --------------------------------------------------------------------------
if ! id "${SERVICE_USER}" >/dev/null 2>&1; then
  log "creating system user '${SERVICE_USER}'"
  useradd --system --create-home --home-dir "/var/lib/${SERVICE_USER}-home" \
    --shell /usr/sbin/nologin "${SERVICE_USER}"
else
  log "system user '${SERVICE_USER}' already exists"
fi

# --------------------------------------------------------------------------
# 2b. Make sure ${SERVICE_USER} can actually reach ${REPO_DIR}. Following
#     this guide's own step 3 ("git clone" straight into your home
#     directory) puts the checkout somewhere a personal account's default
#     permissions routinely deny to any *other* user -- and the systemd
#     units below run as the dedicated ${SERVICE_USER}, not you. Left
#     unfixed, that surfaces as the units failing to start with
#     status=200/CHDIR (can't enter WorkingDirectory) or status=203/EXEC
#     (can't even resolve ExecStart) -- a real first deploy hit exactly
#     this. Grants the minimum needed on each ancestor directory: "search"
#     (x) only, never "read" -- ${SERVICE_USER} can reach this checkout by
#     path but still can't list or read anything else of yours.
# --------------------------------------------------------------------------
path="${REPO_DIR}"
while [[ "${path}" != "/" ]]; do
  if ! sudo -u "${SERVICE_USER}" test -x "${path}" 2>/dev/null; then
    log "granting ${SERVICE_USER} traversal (o+x, not read) on ${path}"
    chmod o+x "${path}"
  fi
  path="$(dirname "${path}")"
done

# --------------------------------------------------------------------------
# 3. Persistent data directory (presets/banks/footswitch-mapping JSON +
#    uploaded .nam/IR assets). Deliberately OUTSIDE the repo checkout so a
#    `git pull` / redeploy never touches it.
# --------------------------------------------------------------------------
log "ensuring data directory ${DATA_DIR}"
mkdir -p "${DATA_DIR}/assets"
chown -R "${SERVICE_USER}:${SERVICE_USER}" "${DATA_DIR}"
chmod 750 "${DATA_DIR}"

if [[ "${SKIP_BUILD}" == "true" ]]; then
  log "--skip-build set: leaving existing audio-engine build / control-daemon venv as-is"
else
  # ------------------------------------------------------------------------
  # 4. Build audio-engine (Release)
  # ------------------------------------------------------------------------
  log "building audio-engine (Release)"
  cmake -S "${REPO_DIR}/audio-engine" -B "${REPO_DIR}/audio-engine/build" \
    -DCMAKE_BUILD_TYPE=Release -DAUDIO_ENGINE_BUILD_TESTS=OFF
  cmake --build "${REPO_DIR}/audio-engine/build" -j"$(nproc)"

  # ------------------------------------------------------------------------
  # 5. control-daemon: venv + install (runtime deps only, no dev/test extras)
  # ------------------------------------------------------------------------
  log "setting up control-daemon virtualenv"
  python3 -m venv "${REPO_DIR}/control-daemon/.venv"
  "${REPO_DIR}/control-daemon/.venv/bin/pip" install --quiet --upgrade pip
  "${REPO_DIR}/control-daemon/.venv/bin/pip" install --quiet -e "${REPO_DIR}/control-daemon"
fi

# --------------------------------------------------------------------------
# 6. systemd units
# --------------------------------------------------------------------------
log "installing systemd units"
for unit in control-daemon audio-engine; do
  sed \
    -e "s#__REPO_DIR__#${REPO_DIR}#g" \
    -e "s#__SERVICE_USER__#${SERVICE_USER}#g" \
    -e "s#__DATA_DIR__#${DATA_DIR}#g" \
    "${REPO_DIR}/deploy/systemd/${unit}.service" \
    > "/etc/systemd/system/multieffect-${unit}.service"
done

systemctl daemon-reload
systemctl enable --now multieffect-control-daemon.service
systemctl enable --now multieffect-audio-engine.service
# Re-run-safe: if the units were already running (a redeploy), pick up the
# freshly built binaries/venv rather than leaving the old process resident.
systemctl restart multieffect-control-daemon.service
systemctl restart multieffect-audio-engine.service

# --------------------------------------------------------------------------
# 7. Wi-Fi access point (NetworkManager "shared" mode: DHCP server + NAT
#    for connected clients are handled by NetworkManager itself, no manual
#    hostapd/dnsmasq configuration needed on the Bookworm-and-newer default
#    network stack -- see deploy/README.md for the older dhcpcd+hostapd
#    alternative if your image predates NetworkManager).
# --------------------------------------------------------------------------
if [[ "${SKIP_AP}" == "true" ]]; then
  log "--skip-ap set: leaving Wi-Fi/networking untouched"
else
  if ! command -v nmcli >/dev/null 2>&1; then
    warn "nmcli not found (NetworkManager not active) -- skipping Wi-Fi AP" \
         "setup. Install/enable NetworkManager or pass --skip-ap to silence" \
         "this warning. See deploy/README.md for the older dhcpcd+hostapd path."
  else
    if [[ -z "${WIFI_PASSWORD}" ]]; then
      WIFI_PASSWORD="$(tr -dc 'A-Za-z0-9' </dev/urandom | head -c 16)"
      GENERATED_PASSWORD="true"
    else
      GENERATED_PASSWORD="false"
    fi
    [[ ${#WIFI_PASSWORD} -ge 8 ]] || die "--wifi-password must be at least 8 characters (WPA2 minimum)"

    if [[ -n "${WIFI_COUNTRY}" ]] && command -v raspi-config >/dev/null 2>&1; then
      log "setting Wi-Fi regulatory country to ${WIFI_COUNTRY}"
      raspi-config nonint do_wifi_country "${WIFI_COUNTRY}"
    elif [[ -z "${WIFI_COUNTRY}" ]]; then
      warn "no --wifi-country given and raspi-config not available/skipped:" \
           "the Wi-Fi radio may refuse to transmit until a country is set" \
           "(raspi-config > Localisation Options > WLAN Country, or" \
           "'sudo raspi-config nonint do_wifi_country CC')."
    fi

    rfkill unblock wifi || true

    log "configuring Wi-Fi AP '${WIFI_SSID}' on ${WIFI_IFACE} via NetworkManager"
    nmcli connection delete multieffect-ap >/dev/null 2>&1 || true
    nmcli connection add \
      type wifi \
      ifname "${WIFI_IFACE}" \
      con-name multieffect-ap \
      autoconnect yes \
      ssid "${WIFI_SSID}"
    nmcli connection modify multieffect-ap \
      mode ap \
      802-11-wireless.band bg \
      ipv4.method shared \
      wifi-sec.key-mgmt wpa-psk \
      wifi-sec.psk "${WIFI_PASSWORD}"
    nmcli connection up multieffect-ap

    log "Wi-Fi AP is up: SSID '${WIFI_SSID}'"
    if [[ "${GENERATED_PASSWORD}" == "true" ]]; then
      log "generated Wi-Fi password (save this, it is not stored anywhere): ${WIFI_PASSWORD}"
    fi
  fi
fi

# --------------------------------------------------------------------------
# 8. Optional, opt-in: PREEMPT_RT kernel package for latency benchmarking
#    (docs/open-questions.md #1). Installs the package only -- does NOT
#    reboot or change the default boot kernel selection beyond what the
#    package itself does; verify with `uname -r` after a manual reboot.
# --------------------------------------------------------------------------
if [[ "${ENABLE_RT_KERNEL}" == "true" ]]; then
  log "installing PREEMPT_RT kernel package (linux-image-rt-arm64)"
  if apt-get install -y linux-image-rt-arm64; then
    warn "PREEMPT_RT kernel installed but NOT activated yet -- reboot" \
         "manually and confirm with 'uname -r' (expect a '-rt' suffix)," \
         "then re-benchmark before relying on it. See deploy/README.md."
  else
    warn "linux-image-rt-arm64 is not available for this OS/arch." \
         "See deploy/README.md for the Elk Audio OS alternative."
  fi
fi

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------
PI_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
cat <<SUMMARY

[deploy] Done.

  control-daemon : systemctl status multieffect-control-daemon
                    ws://${PI_IP:-<pi-ip>}:8765/ws
  audio-engine   : systemctl status multieffect-audio-engine
                    /run/multieffect-amp-modeler/audio-engine.sock
  data directory  : ${DATA_DIR}
  logs             : journalctl -u multieffect-control-daemon -f
                       journalctl -u multieffect-audio-engine -f

Still NOT functional yet (see deploy/README.md "What this deploys, honestly"):
  - control-daemon does not yet talk to audio-engine (AudioEngineClient is
    still the null/logging implementation).
  - audio-engine has no real audio I/O backend and no real NAM inference
    yet -- it only proves out the control-plane/preset-loading logic.
  - No footswitch or onboard display client exists yet.

SUMMARY
