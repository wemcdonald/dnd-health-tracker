# Deploying a health bar on a Raspberry Pi Zero 2 W

This covers turning a blank Pi Zero 2 W into a finished health-bar unit:
SD-card image prep, SSH-over-USB-C, wiring the LED strip, and provisioning the
software. Do the image prep once per SD card; provisioning is one command.

---

## 1. Flash the OS

Use **Raspberry Pi Imager** with **Raspberry Pi OS (Bookworm) Lite (64-bit)**.
In the imager's settings (gear / Ctrl-Shift-X):

- Set hostname (e.g. `aldric.local`) — unique per unit.
- Enable SSH (password or, better, your public key).
- Set username/password.
- Optionally pre-seed your home WiFi (the bar also manages WiFi itself later).

64-bit OS → cross-compile with `PLATFORM=linux/arm64` (the default in
`build-arm.sh`). For 32-bit OS use `linux/arm/v7`.

## 2. SSH over USB-C (gadget mode)

You solder a USB-C breakout to the Pi's **data** USB port (the inner one, not
PWR-IN) so a single cable gives both power and SSH.

After flashing, mount the **bootfs** partition and edit two files:

`config.txt` — add under `[all]`:

```
dtoverlay=dwc2
```

`cmdline.txt` — it is ONE line; insert right after `rootwait`:

```
modules-load=dwc2,g_ether
```

This brings up a USB network gadget. On your computer a new `usb0`/RNDIS
interface appears; SSH in with the hostname you set:

```sh
ssh <user>@aldric.local
```

(macOS/Linux resolve `.local` via mDNS out of the box; on Windows install
Bonjour or use the link-local IPv6 address.)

### Soldering the USB-C breakout

| Breakout pin | Pi Zero 2 W |
|--------------|-------------|
| VBUS (5V)    | 5V (PP1 / pin 2 or 4) |
| GND          | GND (PP6 / pin 6)     |
| D+           | USB D+ (PP23)         |
| D-           | USB D- (PP22)         |

The D+/D- pads belong to the **USB** port (the data one). Wiring power+data to
the PWR-IN port will power the Pi but give no USB gadget.

## 3. Wire the WS2812B strip

- **DIN** → **GPIO18** (physical pin 12) through a ~330 Ω series resistor.
- **5V** → external 5V supply (16–17 LEDs at full white ≈ 1 A; don't power a
  full strip from the Pi's 5V rail).
- **GND** → strip GND **and** Pi GND must be common.
- Data is 3.3 V from the Pi; for short runs it usually drives the strip fine.
  For reliability add a level shifter (e.g. 74AHCT125) or power the strip at
  ~4.5 V so its logic-high threshold drops.

GPIO18 is the default in `device.toml` (`gpio_pin = 18`) because it is the
hardware-PWM pin the DMA driver prefers.

## 4. Provision the software

Copy this repo to the Pi (git clone or `scp`), then:

```sh
sudo ./deploy/provision.sh
```

It builds the `rpi_ws281x` C library, builds the `healthbar` binary with the
hardware backend, installs `/etc/healthbar/{device,theme}.toml`, and enables the
`healthbar` systemd service (runs as root for GPIO DMA + port 80).

No Go on the Pi? Cross-compile on your dev machine and copy the binary first:

```sh
./deploy/build-arm.sh          # produces ./healthbar
scp healthbar <user>@aldric.local:~/dnd-health-tracker/
# then run provision.sh on the Pi (it installs the prebuilt binary)
```

## 5. Configure

Browse to the unit's web UI (shown by `systemctl status healthbar`, or
`http://aldric.local/`):

- **Player**: name + D&D Beyond character id.
- **D&D Beyond**: easiest path is to set the sheet to **Public** on D&D Beyond —
  then no cookie is needed. For private sheets, use the bookmarklet for the id
  and paste the `CobaltSession` cookie.
- **WiFi**: add each house's network; highest priority wins when several are in
  range. If the bar can't find a known network it raises the
  **`healthbar-setup`** access point — join it and the setup page opens
  automatically (captive portal).
- **Theme**: tweak colors/animation; changes apply live.

Config lives in `/etc/healthbar/*.toml` and can also be hand-edited.

## Per-device checklist (building several)

Everything is identical except `device.toml`:

1. Flash + image-prep the SD card (steps 1–2), unique hostname per unit.
2. `sudo ./deploy/provision.sh`.
3. Set player name + character id (web UI or `device.toml`).
4. Add the shared house WiFi networks once via the web UI (or copy a prepared
   `wifi.toml` into `/etc/healthbar/`).

## Troubleshooting

- `journalctl -u healthbar -f` — live logs.
- LEDs dark but service running → check common ground and the GPIO18 wiring.
- "init ws2811 (root required)" → the service must run as root (it does by
  default; only relevant if you run it by hand).
- Can't reach the web UI → check `systemctl status healthbar` for the bound
  address and the unit's IP.
