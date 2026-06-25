"""Minimal websocket client for the D&D Beyond game-log push (optional).

Port of ``legacy-go/internal/ddb/ws.go``. MicroPython ships no websocket client,
so this implements just enough of RFC 6455 to:

  - open a TLS connection and do the HTTP Upgrade handshake,
  - read server frames (unmasked), answer pings, honor close,
  - treat **every data frame as a "something changed" nudge** — bodies are never
    parsed, so we're immune to the unofficial event schema.

It connects to ``wss://game-log-api-live.dndbeyond.com/v1?gameId&userId&stt`` and
calls ``on_nudge`` per data frame and ``on_state(connected)`` on connect/drop, so
the poller can relax to a slow safety-net while live and tighten back if the
socket dies. Any ``MemoryError`` (the dual-TLS risk) marks the socket down and
backs off — the poller then resumes responsive polling, so WSS is never a
dependency.

Frame encode/decode and the handshake are pure functions so they're unit-tested
without a network; the async glue is thin.
"""

WS_HOST = "game-log-api-live.dndbeyond.com"
WS_PATH = "/v1"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"  # RFC 6455 magic value
ORIGIN = "https://www.dndbeyond.com"

OP_CONT = 0x0
OP_TEXT = 0x1
OP_BIN = 0x2
OP_CLOSE = 0x8
OP_PING = 0x9
OP_PONG = 0xA


def _b64(data):
    import binascii
    return binascii.b2a_base64(data).strip().decode()


def _rand_bytes(n):
    try:
        import os
        return os.urandom(n)
    except (ImportError, AttributeError):
        import random
        return bytes(random.getrandbits(8) for _ in range(n))


def ws_key():
    """A fresh base64 Sec-WebSocket-Key (16 random bytes)."""
    return _b64(_rand_bytes(16))


def accept_key(key):
    """Compute the expected Sec-WebSocket-Accept for a client key (RFC 6455)."""
    import hashlib
    h = hashlib.sha1((key + WS_GUID).encode())
    return _b64(h.digest())


def build_handshake(host, path, key, query=""):
    """Build the HTTP Upgrade request for the websocket handshake."""
    target = path + ("?" + query if query else "")
    return (
        "GET " + target + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: " + ORIGIN + "\r\n"
        "\r\n"
    ).encode()


def build_client_frame(opcode, payload=b"", mask=None):
    """Encode a client->server frame. Client frames MUST be masked (RFC 6455)."""
    if isinstance(payload, str):
        payload = payload.encode()
    mask = mask if mask is not None else _rand_bytes(4)
    n = len(payload)
    b0 = 0x80 | opcode  # FIN + opcode
    if n < 126:
        hdr = bytes([b0, 0x80 | n])
    elif n < 65536:
        hdr = bytes([b0, 0x80 | 126]) + n.to_bytes(2, "big")
    else:
        hdr = bytes([b0, 0x80 | 127]) + n.to_bytes(8, "big")
    masked = bytes(payload[i] ^ mask[i % 4] for i in range(n))
    return hdr + bytes(mask) + masked


def query_string(game_id, user_id, token):
    return "gameId=%s&userId=%s&stt=%s" % (game_id, user_id, token)


async def read_frame(reader):
    """Read one frame from an asyncio StreamReader. Returns (opcode, payload)."""
    h = await reader.readexactly(2)
    b0, b1 = h[0], h[1]
    opcode = b0 & 0x0F
    masked = b1 & 0x80
    ln = b1 & 0x7F
    if ln == 126:
        ln = int.from_bytes(await reader.readexactly(2), "big")
    elif ln == 127:
        ln = int.from_bytes(await reader.readexactly(8), "big")
    mask = await reader.readexactly(4) if masked else b""
    payload = await reader.readexactly(ln) if ln else b""
    if masked:
        payload = bytes(payload[i] ^ mask[i % 4] for i in range(ln))
    return opcode, payload


class WSListener:
    """Maintains the game-log websocket, nudging the poller on every event."""

    def __init__(self, token_provider, game_id, user_id,
                 on_nudge, on_state=None, reconnect_backoff=5.0):
        self.token_provider = token_provider  # callable -> bearer token
        self.game_id = game_id
        self.user_id = user_id
        self.on_nudge = on_nudge
        self.on_state = on_state or (lambda connected: None)
        self.reconnect_backoff = reconnect_backoff

    async def run(self, should_stop=None):
        if not (self.game_id and self.user_id):
            return  # nothing to subscribe to
        import uasyncio as aio
        while not (should_stop and should_stop()):
            try:
                await self._connect_once(aio, should_stop)
            except MemoryError:
                # The dual-TLS RAM risk materialised: stay down and let the
                # poller fall back to responsive polling. Back off generously.
                self.on_state(False)
                await aio.sleep(self.reconnect_backoff * 6)
            except Exception:
                self.on_state(False)
            await aio.sleep(self.reconnect_backoff)

    async def _connect_once(self, aio, should_stop):
        token = self.token_provider()
        key = ws_key()
        reader, writer = await aio.open_connection(WS_HOST, 443, ssl=True)
        try:
            writer.write(build_handshake(
                WS_HOST, WS_PATH, key,
                query_string(self.game_id, self.user_id, token)))
            await writer.drain()
            status = await reader.readline()
            if b"101" not in status:
                raise OSError("ws: handshake failed: %s" % status)
            while True:  # consume the rest of the handshake response headers
                line = await reader.readline()
                if line in (b"\r\n", b"", b"\n"):
                    break
            self.on_state(True)
            while not (should_stop and should_stop()):
                opcode, payload = await read_frame(reader)
                if opcode == OP_CLOSE:
                    return
                if opcode == OP_PING:
                    writer.write(build_client_frame(OP_PONG, payload))
                    await writer.drain()
                elif opcode in (OP_TEXT, OP_BIN, OP_CONT):
                    self.on_nudge()
        finally:
            self.on_state(False)
            try:
                writer.write(build_client_frame(OP_CLOSE))
                await writer.drain()
                await writer.aclose()
            except Exception:
                pass
