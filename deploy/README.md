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

**If you're SSH'd in over the Pi's own Wi-Fi radio (not Ethernet), this
script will disconnect you partway through, on purpose.** Unless
`--skip-ap`, step 7 switches `wlan0` from being a Wi-Fi *client* on your
home network into *being* the AP itself -- and that's the same radio your
SSH session is riding on, so it drops the instant the switch happens. This
is expected, not a failure, but it also means anything the script still
had left to log (notably a generated Wi-Fi password, if you leave
`WIFI_PASSWORD` blank) never reaches your terminal. Two ways to avoid the
surprise:

- Plug in Ethernet first, so the SSH session survives Wi-Fi switching to
  AP mode -- you'll want this for redeploys later anyway (see
  "Redeploying after code changes" below).
- Or run the script inside `tmux`/`screen`, so a dropped SSH session
  doesn't kill it partway through -- reattach afterward to see the rest of
  the output.

On *later* re-runs (when the AP already exists and owns the radio) the
script detaches itself automatically for exactly this reason -- see
"Redeploying after code changes". It's only this first run, which still has
a working uplink when it starts, where you have to think about it yourself.

Either way, set an explicit `WIFI_PASSWORD` in `deploy/config.env` (rather
than leaving it blank) so you're never dependent on catching that one log
line live -- and re-runs stay idempotent instead of silently rotating the
password. If you already lost it: `sudo nmcli -s -g
802-11-wireless-security.psk connection show multieffect-ap` shows the
live PSK (`-s` is required -- nmcli redacts it by default).

```bash
cp deploy/config.env.example deploy/config.env
nano deploy/config.env        # set WIFI_SSID, WIFI_PASSWORD, WIFI_COUNTRY
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
a Unix socket, and owned by `${SERVICE_USER}:${SERVICE_USER}` at mode
`0770` -- the whole pipeline needs root, not just the `echo`, or you'll get
"permission denied" from `nc` even though the command "ran"):

```bash
sudo sh -c "echo '{\"cmd\":\"get_state\"}' | nc -U /run/multieffect-amp-modeler/audio-engine.sock"
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

On a pedal whose AP owns the Wi-Fi radio, let the script do the `git pull`
too -- run by hand it has no internet to reach the remote with (see below):

```bash
cd multieffect-amp-modeler
sudo ./deploy/install.sh --pull main --skip-ap    # name whichever branch you deploy
```

It asks before dropping the AP (pass `-y`/`--yes` to skip the prompt, which
is also required when there's no terminal to ask on). The pull runs as the
checkout's owner rather than root -- so git's dubious-ownership check doesn't
trip and you don't end up with root-owned files -- refuses rather than
clobbering uncommitted changes, and uses `--ff-only` so a diverged branch
fails loudly instead of growing a merge commit.

If the Pi already has an uplink (working Ethernet), the plain two-step form
is fine too, since `git pull` can reach the network on its own:

```bash
git pull
sudo ./deploy/install.sh --skip-ap    # skip-ap: no need to touch networking again
```

**You will be disconnected partway through, and that's expected.** Once the
pedal's AP owns the Wi-Fi radio, the Pi has no internet of its own — the AP
serves its own network, it doesn't uplink anywhere — so a redeploy that needs
packages has nowhere to fetch them from. `install.sh` handles this itself
(step 0b): if the AP is active and there's no internet, it borrows the radio
back for your client Wi-Fi network, does its work, and hands it straight back
to the AP at the end.

Four things make that safe to run while you're connected over the AP:

- **It asks first.** The confirmation comes before anything changes, so
  declining is a true no-op. It has to be asked *before* the detach below,
  since that's the last moment there's still a terminal to ask on.
- **It re-execs itself detached** as a transient systemd unit before touching
  the radio, so your connection dropping can't kill the update halfway.
  Follow along with `journalctl -u multieffect-install -f`, and rejoin the
  pedal's Wi-Fi a minute or two later.
- **Restoring the AP is a trap**, not just the last line — if a build fails,
  or the run is interrupted, the AP still comes back. You don't get stranded
  with no AP *and* no SSH.
- **The client network is auto-detected** (any Wi-Fi profile that isn't the
  AP's). If the Pi knows more than one, name it: `--client-wifi NAME`, or set
  `CLIENT_WIFI` in `deploy/config.env`. `nmcli connection show` lists them.

If the Pi has working Ethernet, none of this triggers — there's already an
uplink, so the AP is left alone entirely and your SSH session survives.

## Uninstalling

```bash
sudo ./deploy/uninstall.sh            # stops/disables services, keeps your data + AP config
sudo ./deploy/uninstall.sh --purge    # also deletes /var/lib/multieffect-amp-modeler and the AP profile
```

## What this deploys, honestly

Getting `systemctl status` to say `active (running)` is not the same as a
working guitar pedal. As of this V1:

The signal path **is** switched on as of this version: the engine is built
with real audio I/O and real NAM inference, started with `--audio`, and the
daemon is pointed at its control socket. What that changed, and what is
still genuinely missing:

**Now enabled (was deployment configuration, not missing code):**

- **The daemon talks to the engine.** `control-daemon.service` sets
  `CONTROL_DAEMON_AUDIO_ENGINE_SOCKET`, so it uses the real
  `UnixSocketAudioEngineClient` instead of falling back to
  `NullAudioEngineClient`. This is also what makes the app's effects
  usable at all: the block palette is built purely from the engine's
  `list_block_types` reply, so under the null client it came up **empty** --
  no Big Muff, Tube Screamer, noise gate, tone stack or delay, even though
  all of them were registered in the engine the whole time.
- **Real audio I/O.** `install.sh` installs `portaudio19-dev`, builds with
  `-DAUDIO_ENGINE_WITH_PORTAUDIO=ON`, and the unit passes `--audio` plus
  `SupplementaryGroups=audio` (for `/dev/snd`) and `LimitRTPRIO`/
  `LimitMEMLOCK` (so the callback can actually get real-time priority
  instead of silently competing with everything else and crackling).
- **Real NAM inference.** Built with `-DAUDIO_ENGINE_WITH_REAL_NAM=ON`, so
  `.nam` files run a real forward pass rather than the pass-through
  `StubNamModel`. This fetches NeuralAmpModelerCore + Eigen at configure
  time and therefore needs internet **during the build** -- which is what
  the radio-borrow step exists for, and why you cannot deploy this with
  `--skip-ap` on a Pi that has no other uplink.

**You almost certainly need `--audio-device`.** ALSA's default device on a
Pi is the onboard bcm2835, which has no capture side at all, so the engine
will fail to open an input and log every device it did find. Get the name
and set it:

```bash
# on the Pi, after plugging the interface in
sudo systemctl stop multieffect-audio-engine
./audio-engine/build/audio_engine --list-devices
sudo ./deploy/install.sh --audio-device "USB Audio" --skip-build --skip-ap
```

It matches a case-insensitive substring against a device that has the
channels being asked for -- a substring rather than a card index because USB
card numbers shuffle between reboots, and this pedal gets power-cut rather
than shut down. `AUDIO_DEVICE` in `deploy/config.env` is the persistent
place for it.

**Still genuinely missing or unverified:**

- **NAM inference has never been benchmarked on ARM.** The per-block timing
  margins quoted in `audio-engine/README.md` were measured on a dev machine.
  A WaveNet model at the default 64-sample block may simply not hit
  real-time on a Pi 4. Measure offline first with the `nam_render` tool
  rather than debugging it as live-audio dropouts, expect to raise
  `--block-size`, and watch the engine's own `[health]` xrun lines in
  `journalctl -u multieffect-audio-engine -f`.
- **Model sample-rate mismatch is unhandled.** A model captured at 44.1kHz
  driven at 48kHz runs at the wrong rate -- audible as a shifted frequency
  response, not as an error. See "Sample rate policy" in
  `audio-engine/README.md`.
- **No footswitch or onboard display client** -- `footswitch/` and
  `display/` are still just planned. `GpioZeroFootswitchBackend` exists in
  the daemon but is never instantiated (nothing builds a
  `FootswitchInputController`, and `create_app` takes no footswitch
  backend). This does not block testing: `footswitch_press` arrives over
  the WebSocket from a client with role `footswitch`, which is how the web
  and mobile apps drive the switches today.
- **No PREEMPT_RT kernel by default.** `docs/open-questions.md` #1 (Pi 4 vs
  5, latency) is still open and needs this kernel (or Elk Audio OS) plus
  real benchmarking -- now finally possible, since audio I/O is on.
  `--enable-rt-kernel` installs `linux-image-rt-arm64` if you want a head
  start, but does not reboot into it or verify anything for you.
- **No protection against hard power cuts to the SD card.** If the physical
  power switch cuts power directly (like a real stompbox, no clean shutdown
  -- see `docs/open-questions.md` #6), this script does not set up the
  read-only-root overlay that mitigates it. Rigs/presets/assets are already
  power-cut-safe (atomic writes); the OS partition is not.

In short: this deploys both processes as reliable system services on a Pi
broadcasting its own Wi-Fi, with the guitar-to-speaker path actually
running. What's unproven is whether a Pi keeps up with real NAM inference
at low latency -- that's now a measurement you can take, not a gap in the
code.

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
  systemd/journald/apt write can leave the root filesystem unbootable,
  needing a re-flash. ext4's journal keeps metadata *consistent* but does
  not make a cut mid-write harmless, and SD cards add their own failure
  mode: a power loss while the card's controller is updating its internal
  block mapping can corrupt data that was never being written, which `fsck`
  cannot always repair. This is a real risk for anything power-cycled this
  often, independent of anything in this repo.
- **journald is the main write source** on an otherwise idle pedal, so it is
  also the main exposure. `Storage=volatile` in
  `/etc/systemd/journald.conf` cuts it to near zero -- but it also means no
  logs survive a reboot, so don't do it while you're still debugging audio.

### What comes back on its own after a power cut

Both services are `WantedBy=multi-user.target` and `Restart=on-failure`, so
they start unattended -- nothing needs typing after a power cut.

The Wi-Fi AP needs one thing to be deterministic, which `install.sh` now
sets: `connection.autoconnect-priority 100` on the `multieffect-ap` profile.
A dev Pi usually also has a client Wi-Fi profile that autoconnects, and at
equal priority NetworkManager breaks the tie by *whichever was used most
recently* -- so the pedal comes up on your home network instead of its own
AP, depending on what the last run happened to do. Check it after a reboot:

```bash
nmcli -t -f NAME,AUTOCONNECT,AUTOCONNECT-PRIORITY connection show
nmcli -t -f NAME connection show --active      # expect multieffect-ap
```

If an older install left the AP at priority 0, fix it without a full
redeploy:

```bash
sudo nmcli connection modify multieffect-ap connection.autoconnect-priority 100
sudo nmcli connection up multieffect-ap
```

The client profile is left autoconnecting on purpose: it only gets the radio
if the AP fails to come up, which is the one moment you want a way in that
isn't a keyboard and a monitor.

### The root filesystem

The standard mitigation is a read-only root with a tmpfs overlay. On
Raspberry Pi OS that's built in (`sudo raspi-config` -> "Overlay
Filesystem"). **On plain Debian -- which is what at least one of these pedals
is actually running -- there is no `raspi-config`**, so it's either the
`overlayroot` package (Ubuntu-originated; confirm it's available on your
release before planning around it) or a hand-written initramfs hook. Budget
real time for this; it is not a five-minute step.

Either way **this script does not enable it**, on purpose: that overlay's writable layer is tmpfs and discarded on every
reboot, so `${DATA_DIR}` would need to live on its own real, separately
mounted partition *excluded* from the overlay first -- otherwise every
preset you save vanishes on the next boot. That's an SD-card partition
layout decision this guide can't make for you, and isn't something to
automate without verifying it on real hardware first (see this guide's own
"honesty check" at the top). If you set this up yourself, verify with
`mount | grep " / "` after rebooting (expect `overlay`, not the SD card's
partition) and confirm a saved preset actually survives a hard power cut to
`${DATA_DIR}`'s own mount before trusting it on a gig.

## Wiring an on/off switch (and what the Pi needs for it)

Short answer: **a plain switch in the power line needs no Pi setup at all,
and a button that shuts down cleanly needs exactly one line in
`config.txt`.** Which you pick decides whether the SD-card risk above
applies to you, so it's worth picking deliberately rather than by what's in
the parts drawer.

Find your firmware config first -- the path differs between images, and on
plain Debian it is *not* where most Pi tutorials say:

```bash
ls /boot/firmware/config.txt /boot/config.txt 2>/dev/null
ls /boot/firmware/overlays/ | grep -E 'gpio-shutdown|gpio-poweroff'
```

If those overlays aren't listed, the rest of this section can't work as
written -- check which package supplies `/boot/firmware/overlays` on your
release before planning a design around it.

### Option 1 -- latching switch in the power line

Nothing to configure. Every power-off is the abrupt kind, so everything in
the section above applies at full strength, and you'd want the read-only
overlay before gigging it.

### Option 2 -- momentary button on GPIO3 (recommended)

One line in `config.txt`:

```
dtoverlay=gpio-shutdown
```

Wire a **momentary** (not latching) SPST button between physical **pin 5
(GPIO3)** and any ground pin (6, 9, 14...). Reboot once for the overlay to
load.

> **The switch type is not a detail -- it decides whether this works at
> all.** A rocker/toggle sold as "ON/OFF" latches: held closed, it keeps
> GPIO3 tied to ground, so you get one shutdown and then a Pi that won't
> boot while it sits in that position. `gpio-shutdown` needs a
> press-and-release edge. Look for "momentary SPST, normally open" (12mm
> tactile and anti-vandal buttons are the usual formats).
>
> A mains-rated latching switch (e.g. 3A 250V AC) is still useful -- just
> not here. Put it on the **AC input, ahead of the PSU**, where that rating
> is exactly right, and use it as a master cutoff *after* the Pi has
> halted. Don't put it in the 5V DC line: contacts rated for AC carry much
> less DC, and a Pi 5 can draw 5A.
>
> The two-switch build is the good one -- momentary button for daily
> on/off, mains rocker for true zero standby draw.

What you get is the stompbox behaviour you'd want from both directions:

- **Press while running** -> the overlay emits a `KEY_POWER` event,
  `systemd-logind` sees it (`HandlePowerKey=poweroff` is the default) and
  runs a clean shutdown. Same safety as typing `sudo poweroff`.
- **Press while halted** -> the Pi boots. GPIO3 is special: it doubles as
  the wake line, which is exactly why the overlay defaults to it. One
  momentary button is your whole power switch.

Verify without gambling an SD card on it:

```bash
grep -E 'gpio-shutdown|gpio-poweroff' /boot/firmware/config.txt
grep -B2 -A3 -i 'shutdown' /proc/bus/input/devices   # the overlay's input device
journalctl -b -u systemd-logind | tail              # after a test press
```

The button press should produce a normal, logged shutdown. If nothing
happens, `logind` isn't picking up the key -- check
`loginctl show-seat seat0` and `HandlePowerKey` in
`/etc/systemd/logind.conf` before rewiring anything.

**Cost:** a halted Pi still draws a little current (the 5V rail stays up so
GPIO3 can wake it). Fine for a pedal that lives on a board with a mains
supply; not fine for battery.

### Option 3 -- Option 2 plus a real power cut

For true zero standby draw, add:

```
dtoverlay=gpio-poweroff
```

That asserts a GPIO at the very *end* of shutdown, which is the signal an
external latching circuit (MOSFET or relay) needs to cut power only once the
filesystem is already flushed and unmounted. This is the correct design for
a production pedal and the most work: it needs real circuitry, not just a
button.

**On a Pi 5 this is mostly solved in hardware.** It has an onboard power
button that already does a clean shutdown and wake, and
`POWER_OFF_ON_HALT=1` (via `sudo rpi-eeprom-config --edit`) makes it
genuinely cut its own rails on halt, down to a few mA. Bringing that button
out to the enclosure is far less work than Option 3 on a Pi 4 -- worth
weighing in `docs/open-questions.md` #1 (Pi 4 vs Pi 5), because it's a real
argument for the 5 that has nothing to do with CPU headroom.

### The GPIO3 conflict -- and why an HDMI screen avoids it

**GPIO3 is also I2C1 SCL** (GPIO2 is SDA, pin 3). An I2C display -- which most
small Pi panels are -- wants that exact pin, and it cannot share it with the
shutdown button. You can move the button
(`dtoverlay=gpio-shutdown,gpio_pin=17`), but **only GPIO3 wakes a halted Pi
4**, so moving it costs the press-to-boot half and leaves you a
shutdown-only button.

**An HDMI/USB panel sidesteps this entirely**: it uses no GPIO, so GPIO3
stays free and press-to-boot works even on a Pi 4. If the display is
undecided, that's a real point in HDMI's favour beyond screen size -- see
`docs/open-questions.md` #4. The other escapes are an SPI panel or a Pi 5
whose own power button leaves GPIO3 alone.

Two things to budget for with an HDMI panel: roughly 0.5-1A over USB on top
of the Pi, and an explicit video mode. Cheap 3.5" panels are usually
480x320, and on plain Debian's KMS driver that's a `video=` parameter in
`cmdline.txt` -- **not** the legacy `hdmi_group`/`hdmi_mode` lines in
`config.txt` that most Pi tutorials still show.

### Cooling, and the 5V rail your audio interface shares

A fan wired to GPIO pins 4/6 is power-only: always on, full speed, no
control, spinning from the moment the Pi is powered. Two of them is likely
overkill -- a decent heatsink plus one quiet fan generally covers sustained
DSP load -- and for an audio device there are two costs worth weighing:

- **Acoustic.** This is a pedal. Constant fan noise is audible in a quiet
  room and in front of a microphone.
- **Electrical.** Fan motors put ripple on the 5V rail, which is the *same
  rail* feeding the USB audio interface. That's the path that can actually
  reach the signal, so it belongs in the interface/power decision rather
  than being treated as a thermal question -- see
  `docs/open-questions.md` #2.

On a **Pi 5**, prefer its dedicated 4-pin fan header: PWM-controlled and
thermally managed, so it stays silent until it genuinely needs to spin.
Mechanically, a connector on pins 4/6 sits immediately beside pin 5 -- check
its housing doesn't block the GPIO3 button wire.

Whole-system power: a Pi 5 (up to 5A) plus a panel (~1A) plus fans (~0.4A)
needs a genuinely solid 5V supply, and anything switching the DC side has to
carry all of it.

### What this changes about the SD-card risk

Options 2 and 3 mean the normal power-off path is a clean shutdown, which
removes the main source of corruption above. The read-only overlay becomes
defence against accidents (yanked cable, blown supply) rather than against
your own on/off switch -- still worth doing eventually, no longer the thing
standing between you and a usable pedal.

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

**Both services show `status=203/EXEC` or `status=200/CHDIR` in
`systemctl status`, right after install.** Fixed as of this checkout -- if
you're still seeing it, you're on an older version of this repo. Both are
the same underlying cause: step 3 above has you `git clone` straight into
your own home directory, and the dedicated `${SERVICE_USER}` the units run
as (not you) then can't reach it -- `203/EXEC` if the units still set
`ProtectHome=true` (hides all of `/home`, not just write access);
`200/CHDIR` if a personal account's default permissions simply don't allow
*any* other user to traverse into it, `ProtectHome` aside. `git pull` to
pick up both fixes (`ProtectHome=read-only`, and `install.sh` now grants
`${SERVICE_USER}` bare traversal -- `o+x`, never read access -- on whatever
ancestor directory was blocking it), then re-run
`sudo ./deploy/install.sh --skip-build` to apply them and restart both
services.

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
