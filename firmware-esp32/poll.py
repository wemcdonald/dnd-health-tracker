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

Anything that isn't a 200 with a parseable line-1 of >=4 ints with max>=1 is
treated as **offline** by the caller (breathing animation), so a 404, garbage,
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


def http_get(host, path, port=80, timeout=8):
    """Minimal plain-HTTP/1.1 GET. Returns the response body string, or None.

    Kept tiny and dependency-free (no urequests) to match the C poller. Returns
    None on any DNS/connect/HTTP error or non-200 status.
    """
    import socket

    # host may already carry a :port; an explicit port arg wins if given non-80.
    hbare, hport = _split_host_port(host)
    if port and port != 80:
        hport = port

    addr = None
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
        req = "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n" % (path, hbare)
        s.send(req.encode())
        chunks = []
        while True:
            b = s.recv(512)
            if not b:
                break
            chunks.append(b)
        raw = b"".join(chunks)
    except Exception:
        return None
    finally:
        try:
            s.close()
        except Exception:
            pass

    # Split status line / headers / body.
    try:
        head, _, body = raw.partition(b"\r\n\r\n")
        status_line = head.split(b"\r\n", 1)[0]
        # e.g. b"HTTP/1.1 200 OK"
        if b" 200" not in status_line:
            return None
        return body.decode()
    except Exception:
        return None


def fetch(dev, timeout=8):
    """Fetch + parse the configured device feed. Returns (cur,max,temp,age)|None."""
    body = http_get(dev.server_host, "/%s.txt" % dev.slug,
                    port=dev.server_port, timeout=timeout)
    return parse_feed(body)
