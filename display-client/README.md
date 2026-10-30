# display-client

Read-only client for the pedal's onboard SPI TFT (spec section 4.4). It
connects to the control daemon as `role: "display"` and draws the active
rig and preset, the signal chain, and the connection, bypass and engine
status. It also shows a boot progress bar while the services start and a
"shutting down" screen when the Pi halts (spec section 5.4).

It cannot change anything. The display role is read-only in the daemon's
protocol, and this client never sends anything after `hello`.

## Screens

| Screen | When |
|---|---|
| Boot | From the first frame until the daemon's state has arrived *and* the audio engine has finished starting. The bar follows real readiness (see "Boot progress" below). |
| Live | Header: `RIG n/N`, rig name, `BYPASS` badge, `LIVE`/`OFFLINE`. Body: large preset number and name. Strip: the rig's chain in order, as component images, washed out if the block is off in this preset (a text chip for types without an image). Input gain and volume are not in the strip: below it come the amp and cab names, then `IN` and `VOL` in dB. Bypass also draws a red frame round the whole screen. |
| Offline | Same live screen, keeping the last known state, with `OFFLINE` and "Reconnecting…". This covers a daemon crash or restart. The client reconnects on its own. |
| Shutting down | When the system is going down. The frame stays on the panel until the power board cuts the supply. |

### Component images

The strip's images are PNGs in `src/display_client/icons/`, one per block
type (`BLOCK_ICONS` in `view.py`). They are rendered from the SVG sources in
`control-daemon/data/img/` because Pillow cannot read SVG, and committed so
the Pi needs no SVG renderer. After changing or adding an SVG:

```bash
sudo apt install librsvg2-bin   # once, on the dev machine
display-client/scripts/render_icons.sh
```

A new image also needs its block type mapped in `BLOCK_ICONS`.

## Installing the Waveshare 3.5" RPi LCD (C) driver

**You only need the vendor's device-tree overlay.** Waveshare's wiki
procedure for Bookworm/Trixie also installs Xorg, LightDM, a desktop and
touch calibration. Skip all of that: the spec rules out an X server or
kiosk next to the real-time engine, and this client writes straight to the
framebuffer. Also skip the old `LCD-show` scripts. They target pre-Bookworm
images and rewrite `config.txt`/`cmdline.txt` wholesale.

The overlay (`waveshare35c.dtbo`) declares the panel as
`compatible = "ilitek,ili9486"`. That binds the kernel's fbtft `fb_ili9486`
driver, which still ships with Raspberry Pi OS kernels. The touch
controller (`ti,ads7846`) is declared too but goes unused.

`sudo ./deploy/install.sh --lcd waveshare35c` does the following for you
(and is idempotent). By hand:

```bash
curl -fsSLO https://files.waveshare.com/wiki/common/Waveshare35c.zip
python3 -m zipfile -e Waveshare35c.zip .
# Bookworm and newer: /boot/firmware, NOT /boot as the vendor page says
sudo cp waveshare35c.dtbo /boot/firmware/overlays/
sudo tee -a /boot/firmware/config.txt <<'EOF'
[all]
dtparam=spi=on
dtoverlay=waveshare35c
EOF
sudo reboot
```

Overlay parameters, if needed: `dtoverlay=waveshare35c,rotate=90,speed=...,fps=...`
(it also accepts `bgr`, `txbuflen`, `swapxy`). Rotation can instead be done
by this client (`DISPLAY_CLIENT_ROTATE`) with no reboot.

Leave the stock `dtoverlay=vc4-kms-v3d` line in place. Whether the panel
ends up as `/dev/fb0` or `/dev/fb1` depends on probe order (and on whether
HDMI is plugged in); the client finds it either way.

### Keeping the Linux console off the panel

Left alone, the kernel's framebuffer console draws boot messages, the
login prompt and a blinking cursor on top of the client's frames. Two
things prevent it, both set up by `install.sh --lcd waveshare35c`:

- The unit's `ExecStartPre` unbinds the framebuffer console
  (`/sys/class/vtconsole/vtconN/bind`) before the client starts. Switching
  the VT to graphics mode instead doesn't last: `getty@tty1` resets its
  terminal to text mode when it starts. This also blanks the HDMI console,
  so use SSH. To get the console back until the next boot:
  `echo 1 | sudo tee /sys/class/vtconsole/vtcon1/bind`.
- `quiet loglevel=3 logo.nologo vt.global_cursor_default=0` are added to
  `cmdline.txt` (backed up as `cmdline.txt.bak-multieffect`), so the few
  seconds before the client starts don't scroll text across the panel. An
  option you've already set, such as your own `loglevel=`, is kept as is.

Check after the reboot:

```bash
cat /sys/class/graphics/fb1/name              # expect: fb_ili9486
cat /sys/class/graphics/fb1/virtual_size      # expect: 480,320
dmesg | grep -iE 'fbtft|ili9486'
sudo sh -c 'cat /dev/urandom > /dev/fb1'      # noise on the panel = driver works
```

**Fallback if the vendor overlay stops working on a newer kernel:** the
in-tree `piscreen` overlay uses the same pinout (DC on GPIO24, reset on
GPIO25, LCD on CE0, touch on CE1) and can bind the DRM `ili9486` driver
instead of fbtft: `dtoverlay=piscreen,drm` (optionally with `speed=` and
`rotate=`). This is **untested on this panel**. Either way the client
works unchanged, because it only needs a `/dev/fbN`.

**Backlight:** always on as shipped. PWM dimming on GPIO 18 needs a
solder-pad bridge (vendor page) and isn't used.

**2.8" 320x240 alternative:** any ILI9341/ST7789 panel with a working
overlay works too. Set `--lcd-size 320x240`
(`DISPLAY_CLIENT_WIDTH`/`HEIGHT`) and the layout scales.

## Boot progress

Both services are `Type=notify`. The audio engine sends `READY=1` once its
control socket is listening and its audio stream is running. The daemon
sends it once uvicorn has bound the WS port. Until then systemd reports the
unit as `activating`, so the unit state is real readiness, not just "the
process exists". The client polls
`systemctl show -p LoadState -p ActiveState` every 0.5 s **during boot
only**:

1. display client running: 25%
2. audio engine ready (or failed, crash-looping, or not installed): 50%
3. control daemon ready: 75%
4. first state snapshot received: 100%, then the live screen

Between steps the bar creeps forward slowly, so a slow step still looks
alive, but it never passes the next real milestone. A step taking more than
5 s also shows its elapsed time.

The engine never holds the boot screen. With no audio interface plugged
in it exits, and systemd restarts it every 2 s. Between attempts systemd
reports it as `activating` (sub-state `auto-restart`), never `failed`, so
that sub-state counts as settled. The boot screen warns
"Audio engine not running" and moves on. An engine that's simply slow is
waited for at most 15 s after the daemon's state has arrived. The daemon
and the app work without an engine. If the daemon fails, the screen shows an error and the
`journalctl` command to run.

The bar can't cover the first few seconds after power-on. The panel stays
blank until the kernel has loaded the overlay, and this client is itself a
service (spec section 5.4, known limitation).

## Startup and shutdown order

`multieffect-display-client.service` declares
`Before=multieffect-audio-engine.service multieffect-control-daemon.service`,
so it starts first and stops last. At shutdown the daemon stops first. The
client sees the connection drop while `systemctl is-system-running` reports
`stopping`, and switches to the shutdown screen. Its own SIGTERM comes last
and leaves that frame in place.

## Footprint

- Dependencies: Pillow and websockets, in their own venv
  (`display-client/.venv`). Fonts come from `fonts-dejavu-core` (apt),
  with Pillow's bundled font as a fallback.
- Event-driven: after boot nothing polls, and a frame is drawn only when
  the daemon broadcasts a state change. An idle pedal costs no CPU and no
  SPI traffic.
- Only changed rows are written to the framebuffer. A preset change
  rewrites the body; a progress-bar tick rewrites a few rows.
- Pinned to CPU 0 at `Nice=10`. The audio engine is pinned to CPUs 1-3, so
  rendering never competes with the audio callback.

## Configuration

Environment variables, set in the systemd unit (see `config.py`):

| Variable | Default |
|---|---|
| `DISPLAY_CLIENT_URI` | `ws://127.0.0.1:8765/ws` |
| `DISPLAY_CLIENT_OUTPUT` | `auto`: the first framebuffer that isn't HDMI. Or `/dev/fbN`, or `png:DIR` for a dev machine. |
| `DISPLAY_CLIENT_WIDTH` / `_HEIGHT` | the framebuffer's own size |
| `DISPLAY_CLIENT_ROTATE` | `0` (clockwise: 0/90/180/270) |
| `DISPLAY_CLIENT_ENGINE_UNIT` / `_DAEMON_UNIT` | the units `deploy/install.sh` installs |

## Running on a dev machine

No panel needed: render to a PNG that's rewritten on every frame.

```bash
cd display-client
python3 -m venv .venv && .venv/bin/pip install -e ".[dev]"
DISPLAY_CLIENT_OUTPUT=png:/tmp/display .venv/bin/display-client
# open /tmp/display/frame.png; drive state with scripts/keyboard_footswitch.py
```

With no systemd present, boot completes as soon as the daemon's snapshot
arrives.

## Tests

```bash
.venv/bin/pytest -q
```

The tests cover the view model (mirroring the daemon's `resolve_preset`
rules), boot-progress logic, framebuffer writes against a fake sysfs and
device (RGB565 byte order, only changed rows written, padded stride,
rotation), rendering at both resolutions with oversized names, and the app
end to end against a stand-in daemon WebSocket server (hello role,
following broadcasts, no redraw when idle, shutdown detection, reconnect
with the last state kept).
