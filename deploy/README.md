# Deploying to a Raspberry Pi

Step-by-step guide to get `control-daemon` and `audio-engine` running as
systemd services on a real Raspberry Pi, with the Pi broadcasting its own
Wi-Fi access point for the mobile app to connect to (per
[`ARCHITECTURE.md`](../ARCHITECTURE.md)'s "Connectivity" decision).

**Honesty check before you start:** this script and guide were written and
shellchecked in a sandboxed dev container with no Raspberry Pi, no audio
hardware, and no way to test `apt`/`systemd`/`NetworkManager` behavior on
real Pi OS. Every command is standard and individually well-trodden, but the
script as a whole has **not been run against real hardware**. Treat this as
a strong first draft: read it before running it, and expect to file a fix
if something on your specific Pi/OS combination doesn't match. See "What
this deploys, honestly" below for exactly what does and doesn't work yet
regardless of deployment success.

## What you need

- A Raspberry Pi 4 or 5 (see `docs/open-questions.md` #1 -- which one is
  still an open question, either works for this deployment).
- A microSD card (16GB+) and a way to flash it (another computer with
  [Raspberry Pi Imager](https://www.raspberrypi.com/software/)).
- Power supply, and a case/cooling if you have one (not required to follow
  this guide).
- Either a monitor+keyboard for the Pi, or a way to SSH into it headlessly
  (covered below).
- A USB Audio 2.0 class-compliant interface, if you have one -- **not used
  by this deployment yet**: the engine has a real-time audio backend
  (PortAudio), but `install.sh` doesn't build or start it with audio enabled
  (see "What this deploys, honestly"). Worth having plugged in so it's there
  when that lands.

## 1. Flash Raspberry Pi OS

1. Open Raspberry Pi Imager on another computer, choose **Raspberry Pi OS
   Lite (64-bit)** (no desktop environment needed -- everything here is
   headless) for your Pi model.
2. Before writing, click the gear icon (Advanced Options) and set:
   - Hostname (e.g. `amppedal`).
   - Enable SSH, with a password or your SSH public key.
   - Username/password.
   - Wi-Fi (SSID/password of your home/dev network, plus the **Wi-Fi
     country** matching where you are) -- this is only so the Pi can reach
     the internet for `apt`/`git` during setup; the pedal's own AP (set up
     in step 4) replaces this once deployed, and you can remove this
     network afterward if you like.
   - Locale/timezone.
3. Write the image, insert the SD card into the Pi, power it on.

## 2. First boot and SSH in

```bash
ssh <username>@<hostname>.local          # or the Pi's IP address
```

If `.local` (mDNS) doesn't resolve, check your router's DHCP client list for
the Pi's IP.

```bash
sudo apt update && sudo apt full-upgrade -y
sudo reboot
```

## 3. Get the code onto the Pi

```bash
git clone https://github.com/bgregorutti/multieffect-amp-modeler.git
cd multieffect-amp-modeler
git checkout main   # or whichever branch you're deploying
```

(If the repo is private, use your usual GitHub auth -- an SSH remote or a
personal access token -- `git clone` works the same either way.)

## 4. Configure and run the install script

```bash
cp deploy/config.env.example deploy/config.env
nano deploy/config.env        # set WIFI_SSID, WIFI_COUNTRY at minimum
sudo ./deploy/install.sh
```

Or skip the config file and pass flags directly:

```bash
sudo ./deploy/install.sh --ssid MyPedal --wifi-country FR
```

Run `./deploy/install.sh --help` for the full flag list. What it does, in
order (see the script itself -- it's commented step by step):

1. Installs system packages (`python3`, build tools, `cmake`,
   `nlohmann-json3-dev`, NetworkManager).
2. Creates a dedicated, unprivileged system user (`ampmodeler` by default)
   that both services run as.
3. Creates `/var/lib/multieffect-amp-modeler/` (rigs/presets/footswitch
   mapping JSON + uploaded `.nam`/IR assets) -- **outside** the git
   checkout, so redeploys never touch your data.
4. Builds `audio-engine` in Release mode and sets up `control-daemon`'s
   Python virtualenv.
5. Installs and starts two systemd services: `multieffect-control-daemon`
   and `multieffect-audio-engine`.
6. Unless `--skip-ap`: turns the Pi's Wi-Fi into an access point via
   NetworkManager (`nmcli`) using its built-in `ipv4.method shared` mode --
   this runs a DHCP server + NAT for you, no manual `hostapd`/`dnsmasq`
   config needed on a current (Bookworm+) Raspberry Pi OS. See the
   appendix below if your image predates NetworkManager.

It's idempotent: re-run it after a `git pull` to rebuild and redeploy, or
after editing `deploy/config.env`. Use `--skip-build` for a config-only
re-run (e.g. you only changed the SSID) or `--skip-ap` to leave networking
alone (e.g. you're iterating on the services over Ethernet/your existing
Wi-Fi and don't want to switch networks each time).

## 5. Verify

```bash
systemctl status multieffect-control-daemon
systemctl status multieffect-audio-engine
journalctl -u multieffect-control-daemon -f    # Ctrl-C to stop following
```

From another machine on the same network as the Pi (or after connecting to
the pedal's own AP -- see below), sanity-check the control daemon is
actually answering:

```bash
curl -i http://<pi-ip>:8765/assets/upload   # expect a 405/400, not "connection refused"
```

or, for a real protocol check, install `websocat` on your dev machine and
run through the handshake described in `control-daemon/README.md`:

```bash
websocat ws://<pi-ip>:8765/ws
{"type": "hello", "role": "app"}
```
You should get a `state_snapshot` back.

For `audio-engine`'s control socket (only reachable on the Pi itself, it's
a Unix socket):

```bash
echo '{"cmd":"get_state"}' | nc -U /run/multieffect-amp-modeler/audio-engine.sock
```

Connect a phone to the `MultiEffectPedal` (or whatever `--ssid` you chose)
Wi-Fi network and confirm it gets an IP address -- that's NetworkManager's
DHCP-server-in-shared-mode working (the Pi itself is the gateway; shared
mode defaults to `10.42.0.1`). There's no prebuilt APK/IPA in this repo:
build and install the app from a dev machine per `mobile-app/README.md`
(e.g. `flutter build apk`), then set the pedal's address in the app's
Settings screen (gear icon on the Status tab) -- it's stored on the phone and
applied immediately, no rebuild needed.

## 6. Reboot test

```bash
sudo reboot
```

After it comes back up, `systemctl status multieffect-control-daemon` and
`multieffect-audio-engine` should both show `active (running)` without you
doing anything -- both units are `enable`d, and the AP connection profile
has `autoconnect yes`.

## Loading NAM/IR/VST3 asset packs

Copying a `.nam`/`.wav` file straight into `${DATA_DIR}/assets` (see
"Reference" below) does **not** make it usable: the daemon only knows
about an asset once it's *registered* in its JSON state (id, checksum,
display name -- see `control-daemon/README.md`'s state model), which
never happens as a side effect of a file merely existing on disk. The
mobile app's own upload flow does this registration for you, but until
its OS file-picker is wired up (see `mobile-app/README.md`), the
supported way to bulk-load a pack from the command line is
`scripts/upload_assets.py` -- it streams each file through the same
HTTP-upload + WebSocket-register calls the app makes, so whatever it
registers shows up in the app's asset picker immediately, no daemon
restart needed.

Run it over SSH, directly on the Pi (the daemon's own venv already has
everything the script needs):

```bash
ssh <username>@<hostname>.local
cd multieffect-amp-modeler
scp -r "you@yourmac:/path/to/your/NAM-IR-pack" /tmp/pack   # get the files onto the Pi first
control-daemon/.venv/bin/python3 scripts/upload_assets.py --dir /tmp/pack
rm -rf /tmp/pack                                            # daemon has its own copy now
```

Or skip the two-hop copy and run it from your dev machine instead, pointed
at the Pi over the network -- the script only needs to *reach* the daemon,
not run on the same box:

```bash
python3 scripts/upload_assets.py --host <pi-ip> --dir "/path/to/your/NAM-IR-pack"
```

Both forms accept `--nam`/`--ir`/`--vst3` for individual files instead of
`--dir` for a whole folder, are safe to re-run (content-deduped by
checksum, so registering the same pack twice is a no-op), and skip
individual unusable files rather than aborting the whole run -- see the
script's own `--help` for the full picture, including `.vst3` bundles.

## Redeploying after code changes

```bash
cd multieffect-amp-modeler
git pull
sudo ./deploy/install.sh --skip-ap    # skip-ap: no need to touch networking again
```

## Uninstalling

```bash
sudo ./deploy/uninstall.sh            # stops/disables services, keeps your data + AP config
sudo ./deploy/uninstall.sh --purge    # also deletes /var/lib/multieffect-amp-modeler and the AP profile
```

## What this deploys, honestly

Getting `systemctl status` to say `active (running)` is not the same as a
working guitar pedal. As of this V1:

The code for a working signal path exists; **this script doesn't switch
it on yet.** Three gaps, all in deployment configuration rather than in
the components themselves:

- **The daemon isn't pointed at the engine.** `control-daemon.service`
  doesn't set `CONTROL_DAEMON_AUDIO_ENGINE_SOCKET`, so the daemon falls
  back to `NullAudioEngineClient` (logs preset changes, calls no one) even
  though the real client, `UnixSocketAudioEngineClient`, exists and
  `audio-engine`'s socket is up at
  `/run/multieffect-amp-modeler/audio-engine.sock` (see step 5). Wiring
  them is an `Environment=` line plus `After=`/`Wants=` on the engine unit
  -- see "Audio engine wiring" in `control-daemon/README.md`.
- **The engine is built and started without audio I/O.** `install.sh`
  builds without `-DAUDIO_ENGINE_WITH_PORTAUDIO=ON` (and doesn't install
  `portaudio19-dev`), and `audio-engine.service` doesn't pass `--audio`, so
  no audio device is ever opened -- plugging a guitar in does nothing yet.
  The service user will also need the `audio` group to open the ALSA device.
  See "Real-time audio I/O" in `audio-engine/README.md`.
- **Real NAM inference isn't enabled.** Built without
  `-DAUDIO_ENGINE_WITH_REAL_NAM=ON`, so `.nam` files are parsed and
  validated but processed by the pass-through `StubNamModel`. See "Real NAM
  inference" in `audio-engine/README.md` (and build `Release`/
  `RelWithDebInfo`, never `Debug`, for anything real-time -- same README).
- **No footswitch or onboard display exists yet** -- `footswitch/` and
  `display/` in the repo layout are still just planned.
- **No PREEMPT_RT kernel by default.** `docs/open-questions.md` #1 (Pi 4 vs
  5, latency) is still open and needs this kernel (or Elk Audio OS) plus
  real benchmarking once audio I/O is enabled on the Pi. `--enable-rt-kernel`
  installs the `linux-image-rt-arm64` package if you want to get a head
  start, but does not reboot into it or verify anything for you -- see
  "Real-time kernel" below.
- **No protection against hard power cuts to the SD card.** If the
  physical power switch is going to cut power directly (like a real
  stompbox, no clean shutdown first -- see `docs/open-questions.md` #6),
  this script does not set up the read-only-root overlay that mitigates
  it. Rigs/presets/assets are already power-cut-safe (atomic writes); the
  OS partition currently is not.

In short: this deploys the two *processes* correctly and gets them running
reliably as system services, on a Pi actually broadcasting its own Wi-Fi
network -- but the guitar-to-speaker signal path isn't switched on in this
deployment yet, even though every piece of it exists in the code.

## Real-time kernel (optional, manual verification required)

`--enable-rt-kernel` installs `linux-image-rt-arm64` (a PREEMPT_RT-patched
kernel package, when available for your Raspberry Pi OS release) but
deliberately does **not** reboot the Pi or make any other change --
switching the kernel that boots is exactly the kind of hard-to-reverse,
whole-system change this script shouldn't do unattended. After installing
it:

```bash
sudo reboot
uname -r        # look for a "-rt" suffix confirming the RT kernel booted
```

If `linux-image-rt-arm64` isn't available for your OS/architecture, the
script says so and continues -- the alternative is
[Elk Audio OS](https://elk.audio/) (a separate OS image, not an apt
package; flashing it is a different path from this guide entirely, and
would replace Raspberry Pi OS rather than layer on top of it).

## Power-cut resilience (not yet automated -- read before wiring a bare power switch)

A pedal's power switch is expected to cut power to the Pi directly, the
same way unplugging a wall-wart does -- no clean `shutdown` first. See
`docs/open-questions.md` #6 for the full writeup; in short:

- **Your rigs/presets/assets are already safe.** `persistence.py` writes
  `state.json` via `fsync` + atomic `os.replace`, so a cut mid-save loses at
  most the last unsaved edit, never a corrupt file.
- **The OS partition is not.** Enough abrupt power cuts during a
  systemd/journald/apt write can leave Raspberry Pi OS's root filesystem
  unbootable, needing a re-flash. This is a real risk for anything power-
  cycled this often, independent of anything in this repo.

The standard mitigation is Raspberry Pi OS's built-in read-only-root
overlay (`sudo raspi-config` -> "Overlay Filesystem", or non-interactively
`sudo raspi-config nonint do_overlayfs 0`), which makes the root filesystem
immune to power-cut corruption. **This script does not enable it**, on
purpose: that overlay's writable layer is tmpfs and discarded on every
reboot, so `${DATA_DIR}` would need to live on its own real, separately
mounted partition *excluded* from the overlay first -- otherwise every
preset you save vanishes on the next boot. That's an SD-card partition
layout decision this guide can't make for you, and isn't something to
automate without verifying it on real hardware first (see this guide's own
"honesty check" at the top). If you set this up yourself, verify with
`mount | grep " / "` after rebooting (expect `overlay`, not the SD card's
partition) and confirm a saved preset actually survives a hard power cut to
`${DATA_DIR}`'s own mount before trusting it on a gig.

## Security notes

- Both services run as a dedicated, unprivileged system user
  (`ampmodeler`), not root, with systemd sandboxing (`ProtectSystem=strict`,
  `NoNewPrivileges`, `ProtectHome`) limiting filesystem access to exactly
  the data directory each one needs.
- **Change the default Wi-Fi AP password.** If you didn't pass
  `--wifi-password`/set it in `config.env`, `install.sh` generated a random
  16-character one and printed it once -- it is not stored anywhere else.
  Re-run `sudo ./deploy/install.sh --wifi-password 'new-password'` to
  change it later.
- `control-daemon`'s WebSocket API has no authentication (matching its
  current design -- see `control-daemon/README.md`); this is acceptable
  only because the Pi's own Wi-Fi AP is the trust boundary. Don't expose
  port 8765 to a network you don't control (e.g. don't put the Pi on your
  home Wi-Fi *and* port-forward 8765 to the internet).

## Troubleshooting

**`nmcli: command not found` / AP step warns and skips.** Your OS image
predates NetworkManager (pre-Bookworm Raspberry Pi OS uses `dhcpcd` +
`hostapd` + `dnsmasq` instead). Either upgrade to a current Raspberry Pi OS
image, or set it up manually -- see the appendix below for the equivalent
`hostapd`/`dnsmasq` config, then re-run `install.sh --skip-ap`.

**Phones can't see the `MultiEffectPedal` network at all.** Almost always a
missing/wrong Wi-Fi country code -- the radio won't transmit without one.
Run `sudo raspi-config nonint get_wifi_country` to check, or
`sudo raspi-config` > *Localisation Options* > *WLAN Country* to set it, then
`sudo ./deploy/install.sh --skip-build` to reapply networking.

**`multieffect-control-daemon` fails to start.** Check
`journalctl -u multieffect-control-daemon -e`. Common causes: port 8765
already bound by something else (`sudo ss -tlnp | grep 8765`); the venv at
`control-daemon/.venv` is missing/broken (`sudo ./deploy/install.sh` without
`--skip-build` rebuilds it).

**`multieffect-audio-engine` fails to start.** Check
`journalctl -u multieffect-audio-engine -e`. Common cause: the build is
missing/stale -- `sudo ./deploy/install.sh` without `--skip-build` rebuilds
it. If the build itself fails, check `nlohmann-json3-dev` actually
installed (`dpkg -l | grep nlohmann`).

**Both services show `status=203/EXEC` in `systemctl status`, right after
install.** Fixed as of this checkout -- if you're still seeing it, you're
on units generated by an older version of this repo. Cause: the systemd
units set `ProtectHome=true`, which makes `/home` entirely invisible (not
just unwritable) to the service -- and step 3 above has you `git clone`
straight into your home directory, so `ExecStart`/`WorkingDirectory`
couldn't be resolved at all. `git pull` to pick up the fix
(`ProtectHome=read-only` instead), then re-run
`sudo ./deploy/install.sh --skip-build` to regenerate the units (no
rebuild needed) and restart both services.

**Permission denied writing to the data directory.** Check ownership:
`ls -ld /var/lib/multieffect-amp-modeler` should be owned by the service
user (`ampmodeler` by default). `sudo chown -R ampmodeler:ampmodeler
/var/lib/multieffect-amp-modeler` fixes a mismatch (e.g. after manually
poking around as root).

**I changed `--service-user` and now nothing matches.** The service user is
baked into the generated systemd units and the data directory's ownership;
changing it after the fact requires either a full `uninstall.sh --purge`
then reinstall, or manually `chown -R`ing the data directory to the new
user and re-running `install.sh`.

## Appendix: Wi-Fi AP without NetworkManager (older Raspberry Pi OS)

If `nmcli` isn't available, the classic `dhcpcd` + `hostapd` + `dnsmasq`
recipe (not automated by `install.sh` -- do this manually, then run
`install.sh --skip-ap`):

```bash
sudo apt install -y hostapd dnsmasq
sudo systemctl unmask hostapd
```

`/etc/hostapd/hostapd.conf`:
```
interface=wlan0
driver=nl80211
ssid=MultiEffectPedal
hw_mode=g
channel=7
wmm_enabled=0
auth_algs=1
wpa=2
wpa_passphrase=<your-password>
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
country_code=<CC>
```

`/etc/default/hostapd`: set `DAEMON_CONF="/etc/hostapd/hostapd.conf"`.

`/etc/dnsmasq.conf` (append):
```
interface=wlan0
dhcp-range=192.168.4.2,192.168.4.20,255.255.255.0,24h
```

`/etc/dhcpcd.conf` (append):
```
interface=wlan0
static ip_address=192.168.4.1/24
nohook wpa_supplicant
```

Then:
```bash
sudo systemctl enable --now hostapd dnsmasq
sudo systemctl restart dhcpcd
```

## Reference: exactly what gets installed/changed on the Pi

For anyone auditing before running this on their own hardware:

| What | Where |
|---|---|
| apt packages | `python3`, `python3-venv`, `python3-pip`, `build-essential`, `cmake`, `pkg-config`, `nlohmann-json3-dev`, `network-manager`, `ca-certificates`, `curl` (+ `linux-image-rt-arm64` only with `--enable-rt-kernel`) |
| system user | `ampmodeler` (or `--service-user`), no login shell |
| data directory | `/var/lib/multieffect-amp-modeler/` (`state.json` + `assets/`) |
| systemd units | `/etc/systemd/system/multieffect-control-daemon.service`, `/etc/systemd/system/multieffect-audio-engine.service` |
| build output | `<repo>/audio-engine/build/`, `<repo>/control-daemon/.venv/` (inside your checkout, gitignored) |
| NetworkManager | one connection profile named `multieffect-ap` (skippable with `--skip-ap`) |
