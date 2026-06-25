"""D&D Beyond integration: fetch a public character's HP over HTTPS and poll it.

Ports the relevant parts of the Go ``internal/ddb`` package (client.go + the
adaptive cadence of poller.go), minus the websocket and Cobalt-token auth which
the Pico build drops (public characters only).

The HTTP/TLS layer reads the response body as a stream straight into the
``hp`` scanner, so the large character document is never buffered in full. It
handles both ``Content-Length`` and ``Transfer-Encoding: chunked`` bodies and
forces ``Accept-Encoding: identity`` (the device cannot gunzip).

TLS uses the platform default (no certificate verification on MicroPython): this
is read-only public HP data, a deliberate tradeoff documented in the design.
"""

import hp

HOST = "character-service.dndbeyond.com"
PORT = 443
PATH_BASE = "/character/v5/character/"
ORIGIN = "https://www.dndbeyond.com"
USER_AGENT = "dnd-health-tracker/2.0 (Pico 2 W; +https://github.com/will/dnd-health-tracker)"


class HTTPStatusError(Exception):
    def __init__(self, code):
        self.code = code
        super().__init__("ddb: http status %d" % code)

    def requires_auth(self):
        return self.code in (401, 403)


class AuthError(Exception):
    """Raised for a private sheet (401/403). Public-only build cannot recover."""


def _default_connect(host, port):
    import socket
    import ssl
    ai = socket.getaddrinfo(host, port)[0]
    s = socket.socket(ai[0], ai[1], ai[2])
    s.connect(ai[-1])
    try:
        return ssl.wrap_socket(s, server_hostname=host)  # SNI where supported
    except TypeError:
        return ssl.wrap_socket(s)


def _build_request(character_id):
    return (
        "GET " + PATH_BASE + character_id + " HTTP/1.1\r\n"
        "Host: " + HOST + "\r\n"
        "Accept: application/json, text/plain, */*\r\n"
        "Accept-Encoding: identity\r\n"
        "Origin: " + ORIGIN + "\r\n"
        "Referer: " + ORIGIN + "/\r\n"
        "User-Agent: " + USER_AGENT + "\r\n"
        "Connection: close\r\n"
        "\r\n"
    ).encode()


class _Response:
    """Reads HTTP status + headers from a sock, then streams the (de-chunked) body."""

    def __init__(self, sock):
        self.sock = sock
        self._buf = b""
        self.status = 0
        self.chunked = False
        self.length = None
        self._remaining = None      # bytes left in current chunk / content-length
        self._chunk_eof = False
        self._parse_head()

    def _fill(self):
        data = self.sock.read(512)
        if not data:
            return False
        self._buf += data
        return True

    def _read_line(self):
        while b"\r\n" not in self._buf:
            if not self._fill():
                break
        idx = self._buf.find(b"\r\n")
        if idx < 0:
            line, self._buf = self._buf, b""
            return line
        line = self._buf[:idx]
        self._buf = self._buf[idx + 2:]
        return line

    def _parse_head(self):
        status_line = self._read_line()
        parts = status_line.split(b" ", 2)
        self.status = int(parts[1]) if len(parts) >= 2 else 0
        while True:
            line = self._read_line()
            if line == b"":
                break
            k, _, v = line.partition(b":")
            k = k.strip().lower()
            v = v.strip()
            if k == b"transfer-encoding" and b"chunked" in v.lower():
                self.chunked = True
            elif k == b"content-length":
                try:
                    self.length = int(v)
                except ValueError:
                    pass
        if self.chunked:
            self._remaining = 0  # forces reading a chunk-size line first
        else:
            self._remaining = self.length  # None => read until EOF

    def read(self, n):
        """Return up to n bytes of decoded body, or b"" at end of body."""
        if self.chunked:
            return self._read_chunked(n)
        return self._read_plain(n)

    def _read_plain(self, n):
        if self._remaining is not None:
            if self._remaining <= 0:
                return b""
            n = min(n, self._remaining)
        if not self._buf and not self._fill():
            return b""
        out = self._buf[:n]
        self._buf = self._buf[n:]
        if self._remaining is not None:
            self._remaining -= len(out)
        return out

    def _read_chunked(self, n):
        if self._chunk_eof:
            return b""
        if self._remaining <= 0:
            size_line = self._read_line()
            if size_line == b"" and self._remaining == 0:
                size_line = self._read_line()  # consume CRLF after a chunk
            try:
                size = int(size_line.split(b";")[0].strip() or b"0", 16)
            except ValueError:
                size = 0
            if size == 0:
                self._chunk_eof = True
                return b""
            self._remaining = size
        want = min(n, self._remaining)
        while len(self._buf) < want and self._fill():
            pass
        out = self._buf[:want]
        self._buf = self._buf[want:]
        self._remaining -= len(out)
        return out

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def fetch_hp(character_id, connect=None):
    """Fetch and compute (cur, max, temp) for a public character id."""
    if not character_id:
        raise ValueError("ddb: empty character id")
    connect = connect or _default_connect
    sock = connect(HOST, PORT)
    try:
        sock.write(_build_request(character_id))
        resp = _Response(sock)
        if resp.status in (401, 403):
            raise AuthError("ddb: character sheet is private")
        if resp.status != 200:
            raise HTTPStatusError(resp.status)
        return hp.parse_hp_stream(resp.read)
    finally:
        try:
            sock.close()
        except Exception:
            pass


# ----- adaptive poller (poller.go) ----------------------------------------

def _monotonic():
    try:
        import time
        return time.ticks_ms() / 1000  # MicroPython
    except (ImportError, AttributeError):
        import time
        return time.monotonic()


async def _async_sleep(d):
    try:
        import uasyncio
        await uasyncio.sleep(d)
    except ImportError:
        import asyncio
        await asyncio.sleep(d)


class Poller:
    """Repeatedly fetches a character's HP at a steady interval, with +/-15%
    jitter and exponential backoff on errors.

    We deliberately do NOT back off to a slow idle cadence: tabletop HP events
    can be minutes apart even mid-combat, so an idle tier would mean the *first*
    hit after a quiet stretch is always seen late. A single steady rate (~5s)
    keeps the first event as responsive as every other one.

    A ``nudge()`` cuts the current wait short and forces an immediate fetch — used
    by the optional websocket push (ws.py) so the steady interval can be relaxed
    to a slow safety-net when live events are arriving.

    ``fetch(character_id)`` is a blocking callable returning (cur, max, temp).
    Callbacks: ``on_hp((cur,max,temp))`` only on change, ``on_status(online)``.
    """

    def __init__(self, fetch, character_id, on_hp, on_status,
                 interval=5.0, err_backoff=2.0, rng=None, sleep=None, event=None):
        self.fetch = fetch
        self.character_id = character_id
        self.on_hp = on_hp
        self.on_status = on_status
        self.interval = interval
        self.err_backoff = err_backoff
        self._sleep = sleep or _async_sleep
        self._event = event  # optional async Event used for interruptible waits
        if rng is not None:
            self._rand = rng
        else:
            try:
                import random
                self._rand = random.random
            except ImportError:
                self._rand = lambda: 0.5

    def nudge(self):
        """Request an immediate fetch (e.g. from a websocket push)."""
        if self._event is not None:
            self._event.set()

    def _jitter(self, d):
        # +/-15% so a fleet of bars does not poll in lockstep.
        return d + (self._rand() * 0.3 - 0.15) * d

    def _error_backoff(self, n):
        d = self.err_backoff * (2 ** min(n - 1, 5))
        if d > 60:
            d = 60.0
        return self._jitter(d)

    async def _wait(self, d):
        """Sleep for d, returning early if nudged. Falls back to plain sleep when
        no Event was supplied (poll-only mode)."""
        if self._event is None:
            await self._sleep(d)
            return
        try:
            import uasyncio as _aio
        except ImportError:
            import asyncio as _aio
        try:
            await _aio.wait_for(self._event.wait(), d)
        except Exception:
            pass  # timeout: normal interval elapsed
        self._event.clear()

    async def run(self, should_stop=None):
        last = None
        err_count = 0
        while not (should_stop and should_stop()):
            try:
                snap = self.fetch(self.character_id)
            except Exception:
                self.on_status(False)
                err_count += 1
                await self._sleep(self._error_backoff(err_count))
                continue
            err_count = 0
            self.on_status(True)
            if snap != last:
                last = snap
                self.on_hp(snap)
            await self._wait(self._jitter(self.interval))
