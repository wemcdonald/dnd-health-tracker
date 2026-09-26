"""Over-the-air firmware update, against the server's existing OTA endpoints.

The server publishes (see server/src/routes/firmware.ts, served over plain HTTP
with no auth):

    GET /firmware/esp32/latest -> manifest.txt:  "<version> <size>\\n<sha256>\\n<imagePath>\\n"
    GET /firmware/esp32/image.bin -> the raw application image

On the ESP32 we consume that with the built-in dual-app OTA (`esp32.Partition`):

  1. Fetch + parse the manifest; skip if its version <= our FIRMWARE_VERSION,
     or if it's a version that already rolled back here (see below).
  2. Stream image.bin straight into the *next* OTA partition (never buffered
     whole — a full image is far larger than free heap), hashing as we go.
  3. Verify streamed size + SHA-256 against the manifest; abort on mismatch.
  4. ``set_boot`` the new partition; the caller reboots.
  5. After the new image boots healthy (WiFi up + one good poll), the app calls
     ``mark_valid()``. If it never does (crash/hang), the bootloader rolls back
     to the previous partition — provided the build enables rollback
     (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). See README.md#ota.

Skipping a version that rolled back: without this, the previous image would
confirm on its first good poll, see the same newer manifest and download the
bad image again every few minutes. ``/data/ota.json`` records it:

  - ``apply_update`` adds ``"pending": v`` after ``set_boot`` (keeping any
    failure record).
  - ``note_boot`` (early on every boot): pending but we are *not* running v ->
    the bootloader rolled back, so record ``{"failed": v, "attempts": n}``
    (n counts up for the same v, restarts at 1 for a different one).
  - ``note_confirmed`` (when the image is confirmed): clear the marker.
    Clearing happens on confirmation, not on boot, because a booted image can
    still roll back until it reaches the server.
  - ``check`` offers ``failed`` again until it has rolled back
    ``OTA_MAX_ATTEMPTS`` (2) times, so one transient outage during probation
    can't blacklist a good image; after that it is skipped until a different
    version is published. A full flash erase clears the marker.

``parse_manifest`` and the marker transitions are pure and host-tested.
Everything touching flash is guarded so importing this module is harmless
off-device.
"""

import config
import version

# This build's board namespace on the server. Firmware images are served per
# board (/firmware/<board>/...) so a Pico image can never reach an ESP32.
BOARD = "esp32"

_SHA_RE_LEN = 64

# A version that rolls back this many times is skipped until a different one is
# published. 2 = one retry, so one transient outage can't blacklist a good image.
OTA_MAX_ATTEMPTS = 2


def parse_manifest(text):
    """Parse manifest.txt. Returns dict(version,size,sha256,image_path) or None."""
    if text is None:
        return None
    if isinstance(text, (bytes, bytearray)):
        try:
            text = text.decode()
        except Exception:
            return None
    lines = text.split("\n")
    if len(lines) < 3:
        return None
    head = lines[0].split()
    if len(head) < 2:
        return None
    try:
        ver = int(head[0])
        size = int(head[1])
    except ValueError:
        return None
    sha = lines[1].strip().lower()
    image_path = lines[2].strip()
    if ver < 0 or size <= 0:
        return None
    if len(sha) != _SHA_RE_LEN or any(c not in "0123456789abcdef" for c in sha):
        return None
    if not image_path.startswith("/"):
        return None
    return {"version": ver, "size": size, "sha256": sha, "image_path": image_path}


def _hexdigest(h):
    try:
        import binascii
        return binascii.hexlify(h.digest()).decode()
    except Exception:
        return "".join("%02x" % b for b in h.digest())


# ----- rollback marker (/data/ota.json) -----------------------------------

def _ver(v):
    """v if it's a real non-negative JSON int (not a bool), else None."""
    if isinstance(v, bool) or not isinstance(v, int) or v < 0:
        return None
    return v


def parse_state(obj):
    """Normalise a loaded marker to its known keys: pending, failed, attempts. Pure.

    ``attempts`` only means something alongside ``failed``; an old marker
    without a valid count (>= 1) counts as one attempt.
    """
    out = {}
    if not isinstance(obj, dict):
        return out
    for k in ("pending", "failed"):
        v = _ver(obj.get(k))
        if v is not None:
            out[k] = v
    if "failed" in out:
        n = _ver(obj.get("attempts"))
        out["attempts"] = n if n else 1
    return out


def state_on_boot(state, running):
    """Marker after a boot of version `running`. Pure.

    A pending version we aren't running means the bootloader rolled back:
    count one more failed attempt for it (restarting the count for a version
    other than the one already recorded).
    """
    pending = state.get("pending")
    if pending is None or pending == running:
        return state
    if state.get("failed") == pending:
        return {"failed": pending, "attempts": state.get("attempts", 1) + 1}
    return {"failed": pending, "attempts": 1}


def state_on_stage(state, staged):
    """Marker once `staged` is set to boot next. Keeps any failure record. Pure."""
    new = dict(state)
    new["pending"] = staged
    return new


def state_on_confirm(state, running):
    """Marker once the running image is confirmed healthy. Pure.

    The pending update took, so the whole marker goes, including a failure
    record for this version (a retry that worked) or an older one.
    """
    if state.get("pending") == running:
        return {}
    return state


def is_available(manifest_version, running, state):
    """Offer an update iff it's newer than us and hasn't already rolled back
    OTA_MAX_ATTEMPTS times on this board."""
    if manifest_version <= running:
        return False
    return not (manifest_version == state.get("failed")
                and state.get("attempts", 1) >= OTA_MAX_ATTEMPTS)


def _state_path(data_dir):
    return data_dir + "/ota.json"


def load_state(data_dir):
    """Read the marker. Missing/unreadable -> {}. Never raises."""
    try:
        return parse_state(config._read_json(_state_path(data_dir), {}))
    except Exception:
        return {}


def _save_state(data_dir, state):
    try:
        config._write_json(_state_path(data_dir), state)
    except Exception:
        # Never raise into the poll loop. But this isn't harmless: a lost
        # {"pending"} write means a rollback goes unnoticed, so a bad image can
        # be downloaded again on every OTA check, as before this marker existed.
        pass


def _transition(data_dir, fn, running):
    state = load_state(data_dir)
    new = fn(state, version.FIRMWARE_VERSION if running is None else running)
    if new != state:  # only touch flash when something changed
        _save_state(data_dir, new)


def note_boot(data_dir, running=None):
    """Call early on every boot: turns a pending marker into failed after a rollback."""
    _transition(data_dir, state_on_boot, running)


def note_confirmed(data_dir, running=None):
    """Call when the running image is confirmed: the pending update took."""
    _transition(data_dir, state_on_confirm, running)


def note_staged(data_dir, staged_version):
    """Record that staged_version is set to boot next."""
    _save_state(data_dir, state_on_stage(load_state(data_dir), staged_version))


# ----- check + apply -------------------------------------------------------

def check(dev, data_dir):
    """Fetch the manifest and decide if an update is available.

    Returns (manifest_dict, available_bool). available is True iff the server
    version is strictly newer than ours and isn't a version that already
    rolled back on this board.
    """
    import poll
    body = poll.http_get(dev.server_host, "/firmware/%s/latest" % BOARD,
                         port=dev.server_port, timeout=8)
    m = parse_manifest(body)
    if not m:
        return (None, False)
    return (m, is_available(m["version"], version.FIRMWARE_VERSION, load_state(data_dir)))


class _BlockWriter:
    """Chop a byte stream into fixed-size flash blocks for write_block(n, buf).

    Fills one preallocated block buffer in place: MicroPython's bytearray has
    no slice deletion, so trimming a growing buffer (del buf[:n]) raises
    TypeError on the device even though it works under CPython. The last
    partial block is padded with 0xFF (erased-flash value).
    """

    def __init__(self, write_block, block_size):
        self._write_block = write_block
        self._size = block_size
        self._buf = bytearray(block_size)
        self._fill = 0
        self._next = 0

    def write(self, data):
        mv = memoryview(data)
        i = 0
        while i < len(mv):
            take = min(self._size - self._fill, len(mv) - i)
            self._buf[self._fill:self._fill + take] = mv[i:i + take]
            self._fill += take
            i += take
            if self._fill == self._size:
                self._flush()

    def finish(self):
        if self._fill:
            self._buf[self._fill:] = b"\xff" * (self._size - self._fill)
            self._flush()

    def _flush(self):
        self._write_block(self._next, self._buf)
        self._next += 1
        self._fill = 0


def _stream_image_to_partition(dev, m):
    """Download image.bin straight into the next OTA partition, verifying hash.

    Returns the partition on success, else None. Raises nothing — all failures
    (network, size, hash, flash) return None.
    """
    import socket
    import hashlib
    from esp32 import Partition

    part = Partition(Partition.RUNNING).get_next_update()
    # Block size the partition expects (bytes). Fall back to 4096.
    try:
        block_size = part.ioctl(5, 0) or 4096   # 5 = MP_BLOCKDEV_IOCTL_BLOCK_SIZE
    except Exception:
        block_size = 4096

    hbare = dev.server_host.partition(":")[0]
    try:
        addr = socket.getaddrinfo(hbare, dev.server_port or 80)[0][-1]
    except Exception:
        return None

    s = socket.socket()
    try:
        try:
            s.settimeout(20)
        except Exception:
            pass
        s.connect(addr)
        req = ("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n"
               % (m["image_path"], hbare))
        s.send(req.encode())

        # Read past headers, keeping any body bytes that arrived with them.
        header = b""
        while b"\r\n\r\n" not in header:
            chunk = s.recv(512)
            if not chunk:
                return None
            header += chunk
        status = header.split(b"\r\n", 1)[0]
        if b" 200" not in status:
            return None
        body_start = header.split(b"\r\n\r\n", 1)[1]

        h = hashlib.sha256()
        writer = _BlockWriter(part.writeblocks, block_size)
        received = 0

        chunk = body_start  # may be empty if the headers arrived alone
        while True:
            if chunk:
                h.update(chunk)
                received += len(chunk)
                writer.write(chunk)
            if received >= m["size"]:
                break
            chunk = s.recv(1024)
            if not chunk:
                break
        writer.finish()

        if received != m["size"]:
            return None
        if _hexdigest(h) != m["sha256"]:
            return None
        return part
    except Exception:
        return None
    finally:
        try:
            s.close()
        except Exception:
            pass


def apply_update(dev, m, data_dir):
    """Stream + verify + set_boot the image described by manifest m.

    Returns True if the new partition is staged and the caller should reboot;
    False on any failure (the current image stays bootable). On success the
    staged version is recorded as pending in data_dir/ota.json.
    """
    part = _stream_image_to_partition(dev, m)
    if part is None:
        return False
    try:
        part.set_boot()
    except Exception:
        return False
    note_staged(data_dir, m["version"])
    return True


def mark_valid():
    """Confirm the freshly-booted OTA image is healthy (cancels rollback).

    Safe no-op if OTA/rollback isn't in play (e.g. first factory image, or off
    device).
    """
    try:
        from esp32 import Partition
        Partition.mark_app_valid_cancel_rollback()
    except Exception:
        pass


def reboot():
    import machine
    machine.reset()
