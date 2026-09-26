"""Device feed client: plain-HTTP GET of /<slug>.txt + wire parser.

The server precomputes one tiny line per character; the device just fetches it
over **plain HTTP** (no TLS on the ESP32 side either — we keep the device dumb)
and parses line 1.

Wire format (authoritative — see server/src/routes/dnd.ts):

    <cur> <max> <temp> <age_s>
    HP <cur>/<max> (+<temp> temp) · <pct>%     <- line 2+, ignored

    cur    current HP (0..max)
    max    max HP (>= 1 for a live character; 0 is the "no data" sentinel)
    temp   temporary HP (separate buffer on top of cur)
    age_s  seconds since the server last refreshed from D&D Beyond (99999 = never)

A 404 (server reachable but slug unknown) maps to UNKNOWN_SLUG so the caller
can treat it as a config problem rather than an outage. Anything else that
isn't a 200 with a parseable line-1 of >=4 ints with max>=1 is treated as
**offline** by the caller (breathing animation), so other non-200s, garbage,
or the "0 0 0 99999" sentinel all degrade safely.

``parse_feed`` is pure so it runs under CPython for the host tests.
"""


def parse_feed(body):
    """Parse the device-feed body. Returns (cur, max, temp, age) or None.

    None means "no usable data" (bad HTTP, garbage, or the max==0 sentinel) and
    the caller should show the offline animation.
    """
    if body is None:
        return None
    if isinstance(body, (bytes, bytearray)):
        try:
            body = body.decode()
        except Exception:
            return None
    line = body.split("\n", 1)[0]
    parts = line.split()
    if len(parts) < 4:
        return None
    try:
        cur = int(parts[0])
        mx = int(parts[1])
        temp = int(parts[2])
        age = int(parts[3])
    except ValueError:
        return None
    if mx < 1:
        return None  # "0 0 0 99999" sentinel: server has no upstream data yet
    if cur < 0 or temp < 0 or age < 0:
        return None
    return (cur, mx, temp, age)


def _split_host_port(host):
    if ":" in host:
        h, _, p = host.partition(":")
        try:
            return h, int(p)
        except ValueError:
            return h, 80
    return host, 80


# Returned by classify_feed/fetch when the server answered 404: it is reachable
# but doesn't know our slug. That's a config problem, not an outage, so the
# caller must not count it toward the offline reset (and it proves an OTA image
# healthy).
UNKNOWN_SLUG = "unknown-slug"

# ESP32 heap; feed and config responses are tiny, but an intermediary error
# page can be tens of KB.
MAX_RESPONSE_BYTES = 16384


def parse_response(raw):
    """Split a raw HTTP/1.x response. Returns (status, headers, body) or None.

    headers keys are lowercased. Pure: host-tested.
    """
    if not raw:
        return None
    head, sep, body = raw.partition(b"\r\n\r\n")
    if not sep:
        return None
    lines = head.split(b"\r\n")
    parts = lines[0].split()
    if len(parts) < 2 or not parts[0].startswith(b"HTTP/"):
        return None
    try:
        status = int(parts[1])
    except ValueError:
        return None
    try:
        headers = {}
        for line in lines[1:]:
            k, _, v = line.partition(b":")
            if k:
                headers[k.strip().lower().decode()] = v.strip().decode()
        text = body.decode()
    except Exception:
        return None  # non-UTF-8 header/body byte, or any other decode hiccup
    return (status, headers, text)


def http_request(host, path, port=80, timeout=8, headers=None):
    """Minimal plain-HTTP/1.1 GET. Returns (status, headers, body) or None.

    None means a network-level failure (DNS/connect/timeout/unparseable). Kept
    tiny and dependency-free (no urequests) to match the C poller.

    Assumes no chunked Transfer-Encoding: we send no Accept-Encoding and the
    origin (Fastify) sets Content-Length, so a chunked body isn't expected. A
    chunked response would simply fail to parse and be treated as offline/None
    -- we don't implement chunked decoding.
    """
    import socket

    # host may already carry a :port; an explicit port arg wins if given non-80.
    hbare, hport = _split_host_port(host)
    if port and port != 80:
        hport = port

    try:
        addr = socket.getaddrinfo(hbare, hport)[0][-1]
    except Exception:
        return None

    s = socket.socket()
    try:
        try:
            s.settimeout(timeout)
        except Exception:
            pass
        s.connect(addr)
        req = "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n" % (path, hbare)
        for k, v in (headers or {}).items():
            req += "%s: %s\r\n" % (k, v)
        req += "\r\n"
        s.send(req.encode())
        chunks = []
        total = 0
        while True:
            b = s.recv(512)
            if not b:
                break
            chunks.append(b)
            total += len(b)
            if total > MAX_RESPONSE_BYTES:
                return None
        raw = b"".join(chunks)
    except Exception:
        return None
    finally:
        try:
            s.close()
        except Exception:
            pass
    return parse_response(raw)


def http_get(host, path, port=80, timeout=8):
    """Body string of a 200 response, else None (used by ota.check)."""
    r = http_request(host, path, port=port, timeout=timeout)
    if r is None or r[0] != 200:
        return None
    return r[2]


def classify_feed(resp):
    """Map an http_request result to (cur,max,temp,age) | UNKNOWN_SLUG | None."""
    if resp is None:
        return None
    status, _, body = resp
    if status == 404:
        return UNKNOWN_SLUG
    if status != 200:
        return None
    return parse_feed(body)


def fetch(dev, timeout=8):
    """Poll the device feed. Returns (cur,max,temp,age) | UNKNOWN_SLUG | None."""
    return classify_feed(http_request(dev.server_host, "/%s.txt" % dev.slug,
                                      port=dev.server_port, timeout=timeout))
