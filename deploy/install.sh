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
# NetworkManager profile name for the pedal's own AP. Also the name this
# script borrows the radio back from when it needs internet (see step 0b).
AP_CON_NAME="multieffect-ap"
# Client Wi-Fi profile to borrow the radio for when the AP has it and we
# need internet. Empty = auto-detect (any Wi-Fi profile that isn't the AP).
CLIENT_WIFI=""
# Branch to fast-forward the checkout to before building (--pull BRANCH).
# Empty = don't touch the checkout at all.
PULL_BRANCH=""
# Skip the interactive "this will disconnect you" confirmation (-y/--yes).
ASSUME_YES="false"
# Substring of the audio interface's name for audio-engine to capture/play
# through (--audio-device). Empty = let it use whatever ALSA calls the
# default device, which on a Pi is the onboard bcm2835 -- output-only, so
# the engine will fail to open an input. Set this on any Pi with a USB
# interface; `audio_engine --list-devices` prints the available names.
AUDIO_DEVICE=""

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
  --client-wifi NAME      NetworkManager profile for the *client* Wi-Fi
                           network (your home/dev network). When the pedal's
                           AP already holds the radio and there's no
                           internet, this script borrows the radio for that
                           network to install packages, then hands it back.
                           Default: auto-detect any Wi-Fi profile that isn't
                           the AP's. Disconnects you mid-run if you're on
                           the AP -- it re-execs itself detached so that's
                           safe; see deploy/README.md.
  --pull BRANCH           Fast-forward this checkout to origin/BRANCH before
                           building, run as the checkout's owner (not root).
                           Happens *after* the radio has been borrowed above,
                           so this is the one-command way to update a pedal
                           whose AP owns the Wi-Fi: a 'git pull' you run by
                           hand beforehand has no internet to use. Refuses
                           rather than clobbering uncommitted changes, and
                           never creates a merge commit (--ff-only).
  -y, --yes               Don't ask before dropping the AP (which disconnects
                           you). Required when there's no terminal to ask on.
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
  --audio-device NAME       Substring of the audio interface's name the
                             engine should capture/play through (e.g.
                             'Scarlett', 'USB Audio'). Matched
                             case-insensitively against a device that has
                             the needed channels, so it survives USB card
                             renumbering across reboots -- which a card
                             index would not, on a pedal that gets power-cut
                             constantly. Default: ALSA's default device,
                             which on a Pi is the onboard bcm2835 and has no
                             capture side at all -- so set this if you've got
                             a USB interface. List the names with:
                             audio-engine/build/audio_engine --list-devices
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

# Kept verbatim so step 0b can re-exec this script detached with the same
# options it was originally invoked with.
ORIGINAL_ARGS=("$@")

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ssid) WIFI_SSID="$2"; shift 2 ;;
    --client-wifi) CLIENT_WIFI="$2"; shift 2 ;;
    --pull)
      [[ $# -ge 2 && "$2" != --* ]] \
        || die "--pull requires a branch name, e.g. --pull main"
      PULL_BRANCH="$2"; shift 2 ;;
    -y|--yes) ASSUME_YES="true"; shift ;;
    --wifi-password) WIFI_PASSWORD="$2"; shift 2 ;;
    --wifi-country) WIFI_COUNTRY="$2"; shift 2 ;;
    --wifi-iface) WIFI_IFACE="$2"; shift 2 ;;
    --skip-ap) SKIP_AP="true"; shift ;;
    --skip-build) SKIP_BUILD="true"; shift ;;
    --enable-rt-kernel) ENABLE_RT_KERNEL="true"; shift ;;
    --audio-device) AUDIO_DEVICE="$2"; shift 2 ;;
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
# 0b. Internet access vs. the pedal's own Wi-Fi AP
#
#     Once step 7 has turned ${WIFI_IFACE} into the pedal's AP, that AP has
#     no uplink of its own, so the Pi has no internet -- and the profile's
#     `autoconnect yes` means it re-claims the radio on every boot. A
#     redeploy then can't apt-get anything, and on a Pi whose Ethernet isn't
#     working there's no second way in either: you're down to a keyboard and
#     a monitor. (A real deploy hit exactly that.) So borrow the radio back
#     for the client Wi-Fi network while packages are needed, then hand it
#     straight back.
#
#     Two hazards this has to survive:
#       * Dropping the AP kills any SSH session running over it -- very
#         likely *this* one. Bash would take SIGHUP mid-apt-get and the AP
#         would never come back. So re-exec detached under systemd first,
#         where losing the connection is harmless.
#       * Anything can fail between "AP down" and "AP back up", so restoring
#         it is an EXIT/HUP/INT/TERM trap, not just a line at the end of
#         step 7.
# --------------------------------------------------------------------------
AP_BORROWED="false"

have_internet() {
  # DNS resolution stands in for "apt can work". It fails in exactly the
  # state we care about: the AP's own dnsmasq answers on the pedal network
  # but has no upstream to forward to.
  timeout 5 getent hosts deb.debian.org >/dev/null 2>&1
}

ap_is_active() {
  nmcli -t -f NAME connection show --active 2>/dev/null \
    | grep -qx "${AP_CON_NAME}"
}

detect_client_wifi() {
  # Any Wi-Fi profile that isn't ours; on a Pi set up per deploy/README.md
  # step 1 that's the home/dev network the imager configured.
  nmcli -t -f NAME,TYPE connection show 2>/dev/null \
    | awk -F: -v ap="${AP_CON_NAME}" \
        '$2 == "802-11-wireless" && $1 != ap { print $1; exit }'
}

restore_ap() {
  [[ "${AP_BORROWED}" == "true" ]] || return 0
  AP_BORROWED="false"
  log "handing ${WIFI_IFACE} back to the AP '${WIFI_SSID}'"
  nmcli connection up "${AP_CON_NAME}" >/dev/null 2>&1 \
    || warn "could not bring '${AP_CON_NAME}' back up -- run" \
            "'sudo nmcli connection up ${AP_CON_NAME}' from a console."
}

# Idempotent (guarded on AP_BORROWED), so running from both a signal trap and
# the EXIT trap is harmless. The signal traps exit deliberately rather than
# letting bash resume: carrying on with the AP restored but the internet gone
# would just fail the next apt-get halfway through anyway.
trap restore_ap EXIT
trap 'restore_ap; die "interrupted (SIGHUP) -- AP restored, nothing further applied"' HUP
trap 'restore_ap; die "interrupted (SIGINT) -- AP restored, nothing further applied"' INT
trap 'restore_ap; die "interrupted (SIGTERM) -- AP restored, nothing further applied"' TERM

if command -v nmcli >/dev/null 2>&1 && ap_is_active && ! have_internet; then
  [[ -n "${CLIENT_WIFI}" ]] || CLIENT_WIFI="$(detect_client_wifi)"

  if [[ -z "${CLIENT_WIFI}" ]]; then
    warn "the AP '${WIFI_SSID}' is holding ${WIFI_IFACE} and there is no" \
         "internet, but no client Wi-Fi profile was found to borrow it for." \
         "Package installs below will likely fail -- pass --client-wifi NAME," \
         "or connect Ethernet."
  else
    # Re-exec detached before touching the radio, so losing the connection
    # this is running over can't abandon the update half-applied.
    if [[ -z "${INSTALL_DETACHED:-}" ]] && command -v systemd-run >/dev/null 2>&1; then
      log "-------------------------------------------------------------"
      log "This run needs internet, so it must take ${WIFI_IFACE} away from"
      log "the AP '${WIFI_SSID}' for a few minutes. THAT WILL DISCONNECT YOU"
      log "if you are connected over the AP right now."
      log ""
      log "Re-running detached as a systemd unit so your disconnection"
      log "cannot kill the update midway. The AP is restored at the end"
      log "automatically, even if a step fails in between."
      log ""
      log "  watch progress : journalctl -u multieffect-install -f"
      log "  then reconnect : rejoin '${WIFI_SSID}' in a minute or two"
      log "-------------------------------------------------------------"

      # Asked here, before the detach, because this is the last moment there
      # is still a terminal to ask on -- and before anything has changed, so
      # declining is a true no-op.
      if [[ "${ASSUME_YES}" != "true" ]]; then
        if [[ -t 0 ]]; then
          printf '[deploy] Drop the AP and continue? [y/N] '
          read -r REPLY_CONFIRM || REPLY_CONFIRM=""
          case "${REPLY_CONFIRM}" in
            y|Y|yes|YES) ;;
            *) die "aborted at your request -- nothing changed, AP untouched." ;;
          esac
        else
          die "this run needs to drop the AP '${WIFI_SSID}' but has no" \
              "terminal to confirm on. Re-run with --yes to allow it."
        fi
      fi

      exec systemd-run \
        --unit=multieffect-install \
        --collect \
        --description="multieffect-amp-modeler install/redeploy" \
        --property=WorkingDirectory="${REPO_DIR}" \
        --setenv=INSTALL_DETACHED=1 \
        "${BASH_SOURCE[0]}" "${ORIGINAL_ARGS[@]}"
    fi

    log "no internet and AP '${WIFI_SSID}' holds ${WIFI_IFACE}: borrowing it" \
        "for client Wi-Fi '${CLIENT_WIFI}'"
    nmcli connection down "${AP_CON_NAME}" >/dev/null 2>&1 || true
    # Set before `up` so a failure there still restores the AP via the trap.
    AP_BORROWED="true"
    nmcli connection up "${CLIENT_WIFI}" >/dev/null 2>&1 \
      || warn "could not activate client Wi-Fi '${CLIENT_WIFI}'"

    for _ in $(seq 1 20); do
      have_internet && break
      sleep 2
    done
    have_internet \
      || warn "still no internet after switching to '${CLIENT_WIFI}' --" \
              "package installs may fail. The AP will be restored regardless."
  fi
fi

# --------------------------------------------------------------------------
# 0c. Optional: fast-forward the checkout itself (--pull BRANCH)
#
#     Deliberately *after* step 0b: on a pedal whose AP owns the radio there
#     is no internet until 0b has borrowed it back, so a `git pull` you run
#     by hand beforehand can't reach the remote at all. Doing it here makes
#     "update this pedal to the latest commit" a single command.
#
#     Runs as the checkout's owner rather than as root: root would both trip
#     git's dubious-ownership check on a repo owned by someone else, and
#     leave root-owned files behind that the owner's next `git pull` can't
#     touch.
# --------------------------------------------------------------------------
if [[ -n "${PULL_BRANCH}" ]]; then
  command -v git >/dev/null 2>&1 || die "--pull needs git installed"
  REPO_OWNER="$(stat -c '%U' "${REPO_DIR}")"

  git_as_owner() { sudo -u "${REPO_OWNER}" git -C "${REPO_DIR}" "$@"; }

  # Refuse rather than silently discarding work in progress. deploy/config.env
  # is gitignored, so a configured pedal is not "dirty" by virtue of that.
  if [[ -n "$(git_as_owner status --porcelain)" ]]; then
    die "the checkout at ${REPO_DIR} has uncommitted changes; --pull won't" \
        "clobber them. Commit/stash them (as ${REPO_OWNER}), or re-run" \
        "without --pull to build exactly what's there now."
  fi

  INSTALLER_BEFORE="$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')"

  log "fetching origin as ${REPO_OWNER}"
  git_as_owner fetch --prune origin \
    || die "git fetch failed -- no internet, or the remote needs credentials" \
           "this non-interactive run can't supply (an SSH key with a" \
           "passphrase won't work here; an https remote or an agent-less key" \
           "will)."

  git_as_owner rev-parse --verify --quiet "refs/remotes/origin/${PULL_BRANCH}" >/dev/null \
    || die "origin has no branch '${PULL_BRANCH}'"

  log "fast-forwarding to origin/${PULL_BRANCH}"
  git_as_owner checkout "${PULL_BRANCH}" \
    || die "could not check out '${PULL_BRANCH}'"
  # --ff-only: a deploy should never invent a merge commit, and should fail
  # loudly if the local branch has diverged from the remote.
  git_as_owner merge --ff-only "origin/${PULL_BRANCH}" \
    || die "'${PULL_BRANCH}' has diverged from origin/${PULL_BRANCH} --" \
           "resolve it by hand (as ${REPO_OWNER}); refusing to merge here."

  log "now at $(git_as_owner log --oneline -1)"

  if [[ "$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')" != "${INSTALLER_BEFORE}" ]]; then
    warn "that pull changed deploy/install.sh itself, but this run is still" \
         "executing the version it started with. Re-run install.sh to apply" \
         "the new one."
  fi
fi

# --------------------------------------------------------------------------
# 1. System packages
# --------------------------------------------------------------------------
log "apt-get update && install build/runtime dependencies"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# dnsmasq-base is only a *Recommends* of network-manager, not a hard
# dependency, so --no-install-recommends below skips it -- silently
# breaking step 7's Wi-Fi AP: NetworkManager's ipv4.method=shared spawns
# dnsmasq itself to actually hand out DHCP leases to connected clients,
# so without it the AP still accepts Wi-Fi connections (WPA2 handshake
# succeeds, nmcli shows "connected") but never gives a joining phone/laptop
# an IP address at all. Stock Raspberry Pi OS images ship it regardless;
# a minimal Debian install (this script's other target) does not.
# portaudio19-dev backs the engine's real-time audio I/O (step 4 builds
# with -DAUDIO_ENGINE_WITH_PORTAUDIO=ON); git is needed because that same
# step's -DAUDIO_ENGINE_WITH_REAL_NAM=ON fetches NeuralAmpModelerCore and
# Eigen via CMake FetchContent, which shells out to git.
apt-get install -y --no-install-recommends \
  python3 python3-venv python3-pip \
  build-essential cmake pkg-config nlohmann-json3-dev \
  portaudio19-dev git \
  network-manager dnsmasq-base \
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
# 2c. The 'audio' group must exist for audio-engine.service's
#     SupplementaryGroups=audio to resolve. ${SERVICE_USER} is created above
#     with no supplementary groups at all, and /dev/snd/* is group-owned by
#     'audio', so without it the engine cannot open any audio device -- it
#     fails at Pa_Initialize/Pa_OpenStream with a permissions error rather
#     than anything that names the real cause. Granting it via the unit
#     (SupplementaryGroups=) rather than usermod keeps the privilege scoped
#     to the service instead of the account.
# --------------------------------------------------------------------------
if ! getent group audio >/dev/null 2>&1; then
  warn "no 'audio' group on this system -- audio-engine.service declares" \
       "SupplementaryGroups=audio and will fail to start. Create it" \
       "(groupadd -r audio) or drop that line from the unit."
fi

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
  # A redeploy that only means to touch config (e.g. no --pull, or a
  # deliberate --skip-build to avoid the slow real-NAM fetch) can otherwise
  # leave a binary that silently predates the checkout it's running next to
  # -- no build error, no crash, just a control socket that answers
  # `list_block_types` from whatever code was compiled last time. Catch that
  # here rather than have it surface as "some feature I definitely pushed
  # isn't in the app."
  ENGINE_BIN="${REPO_DIR}/audio-engine/build/audio_engine"
  if [[ -x "${ENGINE_BIN}" ]]; then
    NEWEST_SOURCE="$(find "${REPO_DIR}/audio-engine/src" "${REPO_DIR}/audio-engine/include" \
      -type f -newer "${ENGINE_BIN}" 2>/dev/null | head -1)"
    if [[ -n "${NEWEST_SOURCE}" ]]; then
      warn "audio-engine source is newer than the built binary" \
           "(e.g. ${NEWEST_SOURCE#"${REPO_DIR}/"}) -- this is running a STALE build." \
           "Re-run without --skip-build (add --pull main too if the checkout" \
           "itself might be behind) to pick up the current source."
    fi
  else
    warn "--skip-build set but no built binary found at" \
         "${ENGINE_BIN#"${REPO_DIR}/"} -- audio-engine.service will fail to start." \
         "Re-run without --skip-build at least once."
  fi
else
  # ------------------------------------------------------------------------
  # 4. Build audio-engine (Release)
  # ------------------------------------------------------------------------
  # Both feature options default to OFF in CMakeLists.txt (they were added
  # while this project was developed in a sandbox with no GitHub access).
  # A pedal needs both of them ON, or the engine builds into a control-plane
  # -only process that opens no audio device and passes audio through
  # unmodified -- it starts cleanly and does nothing, which is a confusing
  # way to fail. WITH_REAL_NAM fetches NeuralAmpModelerCore + Eigen at
  # configure time and so needs internet: that's exactly what step 0b's
  # radio borrow exists to provide, and why this runs after it.
  log "building audio-engine (Release, real audio I/O + real NAM inference)" \
      "at commit $(git -C "${REPO_DIR}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  cmake -S "${REPO_DIR}/audio-engine" -B "${REPO_DIR}/audio-engine/build" \
    -DCMAKE_BUILD_TYPE=Release -DAUDIO_ENGINE_BUILD_TESTS=OFF \
    -DAUDIO_ENGINE_WITH_PORTAUDIO=ON \
    -DAUDIO_ENGINE_WITH_REAL_NAM=ON

  # Built as its own explicit target, not the default `all`: `all` also
  # includes nam_render (tools/nam_render.cpp), a dev diagnostic that is not
  # part of the running pedal and links the exact same nam_core archive
  # WHOLE_ARCHIVE. Under `set -e`, a build-`all` invocation dies on ANY
  # target failing -- including nam_render alone -- and never reaches the
  # systemd restart below, leaving whatever (or nothing) was previously
  # installed running instead of the audio_engine that DID just build fine.
  # Building this target alone means the one binary the pedal actually needs
  # is what gates success.
  cmake --build "${REPO_DIR}/audio-engine/build" -j"$(nproc)" --target audio_engine

  # nam_render best-effort: worth having when it works, never worth failing
  # a deploy over. See its own CMakeLists.txt comment for why it can fail
  # independently of audio_engine (WHOLE_ARCHIVE forces the whole nam_core
  # archive into both, so a corrupt member -- e.g. from an earlier build
  # that hit ENOSPC/OOM mid-archive -- can take out one link and not the
  # other depending on which objects each executable's own symbols pull in
  # first).
  if ! cmake --build "${REPO_DIR}/audio-engine/build" -j"$(nproc)" --target nam_render; then
    warn "nam_render (the dry/wet WAV diagnostic tool) failed to build --" \
         "harmless, it isn't part of the running pedal. If this persists" \
         "across a clean 'rm -rf audio-engine/build' rebuild, check" \
         "'df -h' and 'journalctl -k | grep -i \"killed process\"' for" \
         "disk space / OOM during the nam_core compile."
  fi

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
# Quoted in the substitution, not here, so a device name with spaces ("USB
# Audio CODEC") survives into ExecStart as one argument. Empty when no
# device was configured, leaving the engine on the OS default.
if [[ -n "${AUDIO_DEVICE}" ]]; then
  log "audio-engine will capture/play through the device matching '${AUDIO_DEVICE}'"
  AUDIO_DEVICE_ARGS="--device \"${AUDIO_DEVICE}\""
else
  warn "no --audio-device set: the engine will use ALSA's default device," \
       "which on a Pi is the onboard bcm2835 and has NO capture side -- so" \
       "it will fail to open an input. List the real names with:" \
       "${REPO_DIR}/audio-engine/build/audio_engine --list-devices"
  AUDIO_DEVICE_ARGS=""
fi
for unit in control-daemon audio-engine; do
  sed \
    -e "s#__REPO_DIR__#${REPO_DIR}#g" \
    -e "s#__SERVICE_USER__#${SERVICE_USER}#g" \
    -e "s#__DATA_DIR__#${DATA_DIR}#g" \
    -e "s#__AUDIO_DEVICE_ARGS__#${AUDIO_DEVICE_ARGS}#g" \
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
      # `head -c 16` deliberately stops reading (and closes its end of the
      # pipe) the instant it has its 16 bytes, which sends `tr` a SIGPIPE
      # -- entirely normal, and $WIFI_PASSWORD still comes out correct --
      # but this script's own `pipefail` turns that into the pipeline
      # "failing" with status 141, which `set -e` then treats as a real
      # error and kills the whole script right here, silently, before ever
      # reaching the Wi-Fi AP setup below. `|| true` discards that
      # meaningless status; a real `tr`/`head` misbehavior would still
      # leave $WIFI_PASSWORD empty and get caught by the length check
      # right after this block.
      WIFI_PASSWORD="$(tr -dc 'A-Za-z0-9' </dev/urandom | head -c 16 || true)"
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
    nmcli connection delete "${AP_CON_NAME}" >/dev/null 2>&1 || true
    nmcli connection add \
      type wifi \
      ifname "${WIFI_IFACE}" \
      con-name "${AP_CON_NAME}" \
      autoconnect yes \
      ssid "${WIFI_SSID}"
    # autoconnect-priority, not just `autoconnect yes` above: a dev Pi
    # normally also has a client Wi-Fi profile (a netplan-rendered home
    # network, say) that autoconnects too, and both sit at the default
    # priority 0. NetworkManager then breaks the tie by whichever profile
    # was used most recently -- so which network the pedal comes up on
    # after a power cut depends on what the last run happened to do. A
    # pedal has to be predictable: highest priority always wins, so the AP
    # always claims the radio at boot. The client profile is deliberately
    # left autoconnecting as a fallback -- it only ever gets the radio if
    # the AP fails to come up, which is the one moment you want a way back
    # in that isn't a keyboard and a monitor.
    nmcli connection modify "${AP_CON_NAME}" \
      mode ap \
      802-11-wireless.band bg \
      ipv4.method shared \
      connection.autoconnect-priority 100 \
      wifi-sec.key-mgmt wpa-psk \
      wifi-sec.psk "${WIFI_PASSWORD}"
    nmcli connection up "${AP_CON_NAME}"
    # Step 7 has just (re)created and activated the AP itself, so there's
    # nothing left for the step-0b trap to restore on the way out.
    AP_BORROWED="false"

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
