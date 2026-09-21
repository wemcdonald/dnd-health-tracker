"""Over-the-air firmware update, against the server's existing OTA endpoints.

The server publishes (see server/src/routes/firmware.ts, served over plain HTTP
with no auth):

    GET /firmware/latest      -> manifest.txt:  "<version> <size>\\n<sha256>\\n<imagePath>\\n"
    GET /firmware/image.bin   -> the raw application image

On the ESP32 we consume that with the built-in dual-app OTA (`esp32.Partition`):

  1. Fetch + parse the manifest; skip if its version <= our FIRMWARE_VERSION.
  2. Stream image.bin straight into the *next* OTA partition (never buffered
     whole — a full image is far larger than free heap), hashing as we go.
  3. Verify streamed size + SHA-256 against the manifest; abort on mismatch.
  4. ``set_boot`` the new partition; the caller reboots.
  5. After the new image boots healthy (WiFi up + one good poll), the app calls
     ``mark_valid()``. If it never does (crash/hang), the bootloader rolls back
     to the previous partition — provided the build enables rollback
     (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). See README.md#ota.

``parse_manifest`` is pure and host-tested. Everything touching flash is guarded
so importing this module is harmless off-device.
"""

import version

# This build's board namespace on the server. Firmware images are served per
# board (/firmware/<board>/...) so a Pico image can never reach an ESP32.
BOARD = "esp32"

_SHA_RE_LEN = 64


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


def check(dev):
    """Fetch the manifest and decide if an update is available.

    Returns (manifest_dict, available_bool). available is True iff the server
    version is strictly newer than ours.
    """
    import poll
    body = poll.http_get(dev.server_host, "/firmware/%s/latest" % BOARD,
                         port=dev.server_port, timeout=8)
    m = parse_manifest(body)
    if not m:
        return (None, False)
    return (m, m["version"] > version.FIRMWARE_VERSION)


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
        buf = bytearray()
        block_num = 0
        received = 0

        def flush_full_blocks(final):
            nonlocal block_num
            while len(buf) >= block_size:
                part.writeblocks(block_num, bytes(buf[:block_size]))
                del buf[:block_size]
                block_num += 1
            if final and buf:
                pad = block_size - len(buf)
                part.writeblocks(block_num, bytes(buf) + b"\xff" * pad)
                block_num += 1
                del buf[:]

        for chunk in (body_start,):
            if chunk:
                h.update(chunk)
                received += len(chunk)
                buf.extend(chunk)
                flush_full_blocks(False)
        while received < m["size"]:
            chunk = s.recv(1024)
            if not chunk:
                break
            h.update(chunk)
            received += len(chunk)
            buf.extend(chunk)
            flush_full_blocks(False)
        flush_full_blocks(True)

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


def apply_update(dev, m):
    """Stream + verify + set_boot the image described by manifest m.

    Returns True if the new partition is staged and the caller should reboot;
    False on any failure (the current image stays bootable).
    """
    part = _stream_image_to_partition(dev, m)
    if part is None:
        return False
    try:
        part.set_boot()
        return True
    except Exception:
        return False


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
