"""D&D Beyond Cobalt-token auth (optional, for the websocket push).

Port of ``legacy-go/internal/ddb/auth.go``. Exchanges a durable Cobalt session
cookie for the short-lived bearer ("stt") token the game-log websocket needs,
caching it and refreshing ~30 s before expiry.

This is only used when the live-update websocket is enabled. The HP fetch itself
stays public/unauthenticated. The cookie is a full account credential, so it is
stored separately (``secrets.json``), never echoed back by the web UI, and only
read here.
"""

import json

import ddb

AUTH_HOST = "auth-service.dndbeyond.com"
TOKEN_PATH = "/v1/cobalt-token"
ORIGIN = "https://www.dndbeyond.com"
TOKEN_SAFETY = 30          # refresh this many seconds before actual expiry
DEFAULT_TTL = 300          # fallback token lifetime if the response omits ttl


def _monotonic():
    try:
        import time
        return time.ticks_ms() / 1000
    except (ImportError, AttributeError):
        import time
        return time.monotonic()


def _read_all(resp, cap=4096):
    out = bytearray()
    while len(out) < cap:
        d = resp.read(256)
        if not d:
            break
        out += d
    return bytes(out)


class CookieAuth:
    """Turns a Cobalt cookie into a cached, auto-refreshing bearer token."""

    def __init__(self, cookie, connect=None, now=None):
        self.cookie = cookie
        self._connect = connect or ddb._default_connect
        self._now = now or _monotonic
        self.token = None
        self.expires = 0.0

    def get_token(self):
        """Return a currently-valid bearer token, refreshing if needed."""
        if self.token and self._now() < self.expires:
            return self.token
        self._refresh()
        return self.token

    def _request(self):
        return (
            "POST " + TOKEN_PATH + " HTTP/1.1\r\n"
            "Host: " + AUTH_HOST + "\r\n"
            "Accept: */*\r\n"
            "Origin: " + ORIGIN + "\r\n"
            "Referer: " + ORIGIN + "/\r\n"
            "User-Agent: " + ddb.USER_AGENT + "\r\n"
            "Cookie: " + self.cookie + "\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n"
        ).encode()

    def _refresh(self):
        if not self.cookie:
            raise ValueError("ddb: no cobalt cookie configured")
        sock = self._connect(AUTH_HOST, 443)
        try:
            sock.write(self._request())
            resp = ddb._Response(sock)
            if resp.status != 200:
                raise ddb.HTTPStatusError(resp.status)
            data = json.loads(_read_all(resp))
        finally:
            try:
                sock.close()
            except Exception:
                pass
        tok = data.get("token")
        if not tok:
            raise ValueError("ddb: cobalt-token response had no token")
        ttl = data.get("ttl", 0) or 0
        if ttl <= TOKEN_SAFETY:
            ttl = DEFAULT_TTL
        self.token = tok
        self.expires = self._now() + ttl - TOKEN_SAFETY
        return tok
