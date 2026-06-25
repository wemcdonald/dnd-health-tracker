"""Configuration: load/save the health bar's JSON config from flash.

Ported from the Go ``internal/config`` package. TOML is replaced with JSON so we
can use MicroPython's built-in ``json`` module (there is no TOML parser on the
device). Three files live in the data directory:

  - config.json : per-unit identity + hardware (character, LEDs, pin, polling)
  - theme.json  : look-and-feel constants (colors, thresholds, timing)
  - wifi.json   : known WiFi networks ({ssid, psk, priority})

Loading is tolerant: a missing or unreadable file yields built-in defaults so a
fresh unit boots into a sane state and straight into the setup portal.
"""

import json
import os

import colors

# Built-in defaults, mirroring config.DefaultDevice / config.DefaultTheme in Go.
DEFAULT_DEVICE = {
    "player_name": "",
    "character_id": "",
    "num_leds": 16,
    "gpio_pin": 18,
    "brightness": 0.5,    # hardware brightness 0..1
    "poll_seconds": 5.0,  # steady HP poll interval (no idle backoff)
}

DEFAULT_THEME = {
    "hp_high": "#00dc3c",   # full health  (0,220,60)
    "hp_mid": "#e6b400",    # mid health   (230,180,0)
    "hp_low": "#dc1414",    # near death   (220,20,20)
    "temp_hp": "#3c8cff",   # temp overshield (60,140,255)
    "status": "#5050a0",    # connecting/status color (80,80,160)
    "mid_fraction": 0.5,
    "low_fraction": 0.25,
    "brightness": 1.0,      # render-time multiplier 0..1
    "fps": 30,              # 30fps is plenty on a microcontroller (Go used 60)
    "flash_millis": 220,    # damage/heal flash duration
    "adjust_millis": 600,   # bar grow/shrink duration
    "idle_shimmer": 0.08,   # breathing amplitude 0..1
    "idle_shimmer_hz": 0.25,
    "low_pulse_hz": 1.4,
}


class Device:
    """Per-unit identity and hardware configuration."""

    def __init__(self, d):
        self.player_name = d.get("player_name", "")
        self.character_id = str(d.get("character_id", "") or "")
        self.num_leds = int(d.get("num_leds") or DEFAULT_DEVICE["num_leds"])
        self.gpio_pin = int(d.get("gpio_pin", DEFAULT_DEVICE["gpio_pin"]))
        self.brightness = float(d.get("brightness", DEFAULT_DEVICE["brightness"]))
        self.poll_seconds = float(d.get("poll_seconds") or DEFAULT_DEVICE["poll_seconds"])

    def as_dict(self):
        return {
            "player_name": self.player_name,
            "character_id": self.character_id,
            "num_leds": self.num_leds,
            "gpio_pin": self.gpio_pin,
            "brightness": self.brightness,
            "poll_seconds": self.poll_seconds,
        }


class Theme:
    """Look-and-feel constants. Colors are stored as #RRGGBB and parsed to tuples."""

    def __init__(self, d):
        self.hp_high = colors.from_hex(d.get("hp_high", DEFAULT_THEME["hp_high"]))
        self.hp_mid = colors.from_hex(d.get("hp_mid", DEFAULT_THEME["hp_mid"]))
        self.hp_low = colors.from_hex(d.get("hp_low", DEFAULT_THEME["hp_low"]))
        self.temp = colors.from_hex(d.get("temp_hp", DEFAULT_THEME["temp_hp"]))
        self.status = colors.from_hex(d.get("status", DEFAULT_THEME["status"]))
        self.mid_fraction = float(d.get("mid_fraction", DEFAULT_THEME["mid_fraction"]))
        self.low_fraction = float(d.get("low_fraction", DEFAULT_THEME["low_fraction"]))
        self.brightness = float(d.get("brightness", DEFAULT_THEME["brightness"]))
        self.fps = int(d.get("fps") or DEFAULT_THEME["fps"])
        self.flash_millis = int(d.get("flash_millis", DEFAULT_THEME["flash_millis"]))
        self.adjust_millis = int(d.get("adjust_millis", DEFAULT_THEME["adjust_millis"]))
        self.idle_shimmer = float(d.get("idle_shimmer", DEFAULT_THEME["idle_shimmer"]))
        self.idle_shimmer_hz = float(d.get("idle_shimmer_hz", DEFAULT_THEME["idle_shimmer_hz"]))
        self.low_pulse_hz = float(d.get("low_pulse_hz", DEFAULT_THEME["low_pulse_hz"]))


def _read_json(path, default):
    """Read a JSON object, merged over defaults. Missing/invalid -> defaults."""
    merged = dict(default)
    try:
        with open(path) as f:
            data = json.load(f)
        if isinstance(data, dict):
            merged.update(data)
    except (OSError, ValueError):
        pass  # missing file or bad JSON: keep defaults
    return merged


def _write_json(path, obj):
    """Atomically write obj as JSON: write a temp file then rename over path."""
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f)
    os.rename(tmp, path)


def load_device(data_dir="data"):
    return Device(_read_json(data_dir + "/config.json", DEFAULT_DEVICE))


def load_theme(data_dir="data"):
    return Theme(_read_json(data_dir + "/theme.json", DEFAULT_THEME))


def save_device(dev, data_dir="data"):
    _write_json(data_dir + "/config.json", dev.as_dict())


def save_theme_dict(theme_dict, data_dir="data"):
    merged = dict(DEFAULT_THEME)
    merged.update(theme_dict)
    _write_json(data_dir + "/theme.json", merged)


# ----- WiFi networks -------------------------------------------------------

def load_wifi(data_dir="data"):
    """Return the list of known networks (list of {ssid, psk, priority}),
    sorted by descending priority then SSID. Missing file -> empty list."""
    try:
        with open(data_dir + "/wifi.json") as f:
            data = json.load(f)
        nets = data.get("networks", []) if isinstance(data, dict) else []
    except (OSError, ValueError):
        nets = []
    return _sorted_nets(nets)


def save_wifi(networks, data_dir="data"):
    _write_json(data_dir + "/wifi.json", {"networks": _sorted_nets(networks)})


def upsert_wifi(networks, ssid, psk, priority=0):
    """Add or update a network by SSID (mirrors WiFi.Upsert in Go)."""
    out = [n for n in networks if n.get("ssid") != ssid]
    out.append({"ssid": ssid, "psk": psk, "priority": int(priority)})
    return _sorted_nets(out)


def remove_wifi(networks, ssid):
    return [n for n in networks if n.get("ssid") != ssid]


def _sorted_nets(nets):
    return sorted(
        nets,
        key=lambda n: (-int(n.get("priority", 0)), n.get("ssid", "")),
    )
