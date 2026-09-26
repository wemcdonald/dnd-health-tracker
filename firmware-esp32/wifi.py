"""WiFi management for the ESP32 (native 2.4 GHz radio).

We talk to the radio directly through MicroPython's ``network.WLAN``. Unlike the
Pico 2 W's cyw43 part, ESP32 WiFi is native to the SoC, so the cold-load hang
that plagued the Pico build (see firmware/wifi.py history) does not apply here.

  - STA: try each known network in priority order, first to get an IP wins.
  - AP : bring up the ``healthbar-setup`` access point for the setup portal.

The connect logic takes injectable ``sta`` / ``sleep`` / ``now`` so the ordering
and timeout behaviour can be unit-tested without a radio.
"""

AP_SSID = "healthbar-setup"
AP_PASSWORD = "dndhealthbar"  # >= 8 chars (WPA2 minimum)
AP_IP = "192.168.4.1"        # ESP32 AP default gateway/host IP


def _real_sta():
    import network, time
    sta = network.WLAN(network.STA_IF)
    sta.active(True)
    # Small settle after activation before any scan/AP toggling. ESP32 doesn't
    # need the long cyw43 cold-load wait, but a brief pause keeps scans reliable.
    time.sleep(0.2)
    return sta


def _blocking_sleep(d):
    import time
    time.sleep(d)


def _monotonic():
    try:
        import time
        return time.ticks_ms() / 1000
    except (ImportError, AttributeError):
        import time
        return time.monotonic()


def connect_known(networks, timeout=15, settle=0.3, sta=None, sleep=None, now=None):
    """Try each known network (already priority-sorted) until one connects.

    Returns the SSID that connected, or None if none did. ``networks`` is the
    list produced by ``config.load_wifi``.
    """
    if not networks:
        return None
    sta = sta if sta is not None else _real_sta()
    sleep = sleep or _blocking_sleep
    now = now or _monotonic
    sta.active(True)
    for n in networks:
        ssid = n.get("ssid")
        if not ssid:
            continue
        try:
            sta.connect(ssid, n.get("psk", ""))
        except Exception:
            continue
        deadline = now() + timeout
        while now() < deadline:
            if sta.isconnected():
                return ssid
            sleep(settle)
        try:
            sta.disconnect()
        except Exception:
            pass
    return None


class NetManager:
    """Owns the STA and AP interfaces and the current online state."""

    def __init__(self):
        self._sta = None
        self._ap = None
        self._connected_ssid = None

    # -- station mode ------------------------------------------------------

    def sta(self):
        if self._sta is None:
            self._sta = _real_sta()
        return self._sta

    def connect_known(self, networks, timeout=15):
        ssid = connect_known(networks, timeout=timeout, sta=self.sta())
        self._connected_ssid = ssid
        return ssid

    def is_online(self):
        try:
            return self._sta is not None and self._sta.isconnected()
        except Exception:
            return False

    def sta_ip(self):
        try:
            return self._sta.ifconfig()[0]
        except Exception:
            return None

    def connected_ssid(self):
        try:
            if self._sta is not None and self._sta.isconnected():
                try:
                    ssid = self._sta.config("ssid")
                except Exception:
                    ssid = None
                # A falsy/failed live read while connected must not read as "no
                # SSID" -- that would let a pushed wifi remove delete the network
                # the board is currently on. Fall back to what connect_known saw.
                return ssid or self._connected_ssid
        except Exception:
            pass
        return None

    # -- access-point (setup) mode ----------------------------------------

    def start_ap(self):
        import network
        sta = self.sta()
        try:
            sta.active(False)  # free the radio for AP use
        except Exception:
            pass
        ap = network.WLAN(network.AP_IF)
        ap.active(True)  # ESP32 rejects config() on an inactive AP ("Wifi Invalid Mode")
        try:
            ap.config(essid=AP_SSID, password=AP_PASSWORD,
                      security=network.AUTH_WPA_WPA2_PSK)
        except (ValueError, AttributeError, OSError):
            ap.config(essid=AP_SSID, password=AP_PASSWORD)  # port-dependent kwargs
        self._ap = ap
        return AP_IP

    def stop_ap(self):
        if self._ap is not None:
            try:
                self._ap.active(False)
            except Exception:
                pass
            self._ap = None

    def scan(self):
        """Return a sorted list of unique visible SSIDs (best-effort)."""
        try:
            seen = []
            for res in self.sta().scan():
                ssid = res[0].decode() if isinstance(res[0], (bytes, bytearray)) else res[0]
                if ssid and ssid not in seen:
                    seen.append(ssid)
            return sorted(seen)
        except Exception:
            return []
