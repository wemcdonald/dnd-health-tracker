"""Per-board config pushed from the server: GET /device/<mac>/config.

Pushable fields: slug, brightness, poll_seconds, and WiFi networks (upsert by
SSID, or explicit remove). Never pushed: num_leds, server_host/port, gpio_pin
(a bad value there could lock the board out of its own config channel).

Merge rules (see ../docs/firmware-contract.md section 4):
  - an absent or null field means "not managed from the server": keep local value
  - an invalid field is skipped, but the payload rev is still accepted, so one
    typo on the server can't cause an endless retry loop
  - WiFi is add/update only; a remove never touches the connected SSID. A
    remove of the SSID the board is connected to at apply time is ignored and
    not retried (the rev is still accepted); re-save it after the board has
    moved to another network, which bumps the rev.

apply() is pure and host-tested; check() fetches, applies and persists.
"""

import json

import config
import device

CHECK_EVERY_S = 300


class Result:
    def __init__(self, dev, nets, rev, dev_changed, wifi_changed):
        self.dev = dev
        self.nets = nets
        self.rev = rev
        self.dev_changed = dev_changed
        self.wifi_changed = wifi_changed


def _number(v):
    """float(v) for real JSON numbers (not bools/strings), else None."""
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return None
    return float(v)


def _clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def parse_body(text):
    """Parse a 200 body into a payload dict with an int rev, else None."""
    try:
        p = json.loads(text)
    except (ValueError, TypeError):
        return None
    if not isinstance(p, dict) or isinstance(p.get("rev"), bool) or not isinstance(p.get("rev"), int):
        return None
    return p


def apply(payload, dev, nets, connected_ssid=None):
    """Merge a server payload over the local Device + network list. Pure."""
    d = dev.as_dict()
    dev_changed = False

    slug = payload.get("slug")
    if config.valid_slug(slug) and slug != d["slug"]:
        d["slug"] = slug
        dev_changed = True

    b = _number(payload.get("brightness"))
    if b is not None:
        b = _clamp(b, 0.0, 1.0)
        if b != d["brightness"]:
            d["brightness"] = b
            dev_changed = True

    p = _number(payload.get("poll_seconds"))
    if p is not None:
        p = _clamp(p, 2.0, 300.0)
        if p != d["poll_seconds"]:
            d["poll_seconds"] = p
            dev_changed = True

    before = config._sorted_nets(nets)
    out = list(before)
    wifi = payload.get("wifi")
    if isinstance(wifi, dict):
        upsert = wifi.get("upsert")
        if isinstance(upsert, list):
            for n in upsert:
                if not isinstance(n, dict):
                    continue
                ssid, psk = n.get("ssid"), n.get("psk", "")
                if not (isinstance(ssid, str) and ssid and isinstance(psk, str)):
                    continue
                try:
                    pr = int(n.get("priority", 0))
                except (TypeError, ValueError):
                    pr = 0
                out = config.upsert_wifi(out, ssid, psk, pr)
        remove = wifi.get("remove")
        if isinstance(remove, list):
            for ssid in remove:
                if isinstance(ssid, str) and ssid != connected_ssid:
                    out = config.remove_wifi(out, ssid)

    return Result(config.Device(d), out, int(payload.get("rev", 0)), dev_changed, out != before)


def check(dev, ident, mac, fw_version, connected_ssid, local_ip, data_dir, http=None):
    """Fetch this board's config and apply it. Never raises.

    Returns the new Device if any Device field changed (caller swaps it in and
    applies brightness live), else None. WiFi changes are saved to wifi.json and
    take effect on the next (re)connect.
    """
    try:
        if http is None:
            import poll
            http = poll.http_request
        resp = http(dev.server_host, "/device/%s/config" % mac, port=dev.server_port, timeout=8,
                    headers={"X-Device-Token": ident["token"],
                             "X-Config-Rev": str(ident["config_rev"]),
                             "X-Firmware-Version": str(fw_version),
                             "X-Slug": dev.slug,
                             "X-Local-IP": local_ip or ""})
        if resp is None or resp[0] != 200:
            return None  # 304 up to date, 403 token mismatch, or network error
        payload = parse_body(resp[2])
        if payload is None:
            return None
        r = apply(payload, dev, config.load_wifi(data_dir), connected_ssid)
        if r.wifi_changed:
            config.save_wifi(r.nets, data_dir)
        if r.dev_changed:
            config.save_device(r.dev, data_dir)
        ident["config_rev"] = r.rev
        device.save_identity(ident, data_dir)
        return r.dev if r.dev_changed else None
    except Exception:
        return None
