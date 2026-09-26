"""Board identity for server-pushed config.

The board is addressed by its STA MAC (12 lowercase hex, no separators) and
proves itself with a random token it generates on first boot. The server records
sha256(token) on first contact ("self-registration") and afterwards serves this
board's config only to that token. See ../docs/firmware-contract.md section 4.

/data/device.json = {"token": "<32 hex>", "config_rev": <last applied rev>}.
Pure helpers are host-tested; mac_id() needs the radio.
"""

import json

import config

_HEX = "0123456789abcdef"


def format_mac(raw):
    return "".join("%02x" % b for b in raw)


def new_token():
    import binascii
    import os
    return binascii.hexlify(os.urandom(16)).decode()


def _valid_token(t):
    return isinstance(t, str) and len(t) == 32 and all(c in _HEX for c in t)


def load_identity(data_dir="data", token_factory=new_token):
    """Return {"token", "config_rev"}; creates and persists a token if missing."""
    path = data_dir + "/device.json"
    try:
        with open(path) as f:
            d = json.load(f)
        if not isinstance(d, dict):
            d = {}
    except (OSError, ValueError):
        d = {}
    try:
        rev = int(d.get("config_rev", 0) or 0)
    except (TypeError, ValueError):
        rev = 0
    ident = {"token": d.get("token"), "config_rev": rev}
    if not _valid_token(ident["token"]):
        ident["token"] = token_factory()
        try:
            save_identity(ident, data_dir)
        except OSError:
            pass  # unwritable flash: still boot, just with an in-memory-only identity
    return ident


def save_identity(ident, data_dir="data"):
    config._write_json(data_dir + "/device.json",
                       {"token": ident["token"], "config_rev": int(ident["config_rev"])})


def mac_id():
    import network
    return format_mac(network.WLAN(network.STA_IF).config("mac"))
