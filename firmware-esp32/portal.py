"""Setup mode: captive portal (DNS) + minimal web config UI.

Hand-rolled, same as the Pico build:

  - a tiny UDP DNS responder that answers every A query with the AP's own IP,
    so a phone's captive-portal probe lands on us;
  - a minimal async HTTP server that serves a config form, 302-redirects OS
    probe URLs to it, and on save writes config to flash and reboots into RUN
    mode.

The config surface is now just WiFi + which server/slug to poll — the device no
longer talks to D&D Beyond directly, so there are no character/user/game IDs or
Cobalt cookie fields (all of that moved server-side).

The DNS packet builder, form parser, and save handler are pure functions so they
can be unit-tested without sockets.
"""

import config

# OS connectivity-probe URLs: redirect these to the portal so the "sign in to
# network" sheet pops automatically on phones/laptops.
_PROBE_PATHS = (
    "/generate_204", "/gen_204", "/hotspot-detect.html", "/ncsi.txt",
    "/connecttest.txt", "/redirect", "/canonical.html", "/success.txt",
    "/library/test/success.html",
)


# ----- DNS ----------------------------------------------------------------

def ip_to_bytes(ip):
    return bytes(int(p) for p in ip.split("."))


def build_dns_response(query, ip_bytes):
    """Build an A-record reply pointing the queried name at ip_bytes (wildcard)."""
    if len(query) < 12:
        return b""
    tid = query[0:2]
    header = tid + b"\x81\x80" + b"\x00\x01" + b"\x00\x01" + b"\x00\x00" + b"\x00\x00"
    i = 12
    n = len(query)
    while i < n and query[i] != 0:
        i += 1 + query[i]
    i += 1  # skip the zero-length root label
    question = query[12:i + 4]  # QNAME + QTYPE(2) + QCLASS(2)
    answer = (
        b"\xc0\x0c"          # pointer to the name at offset 12
        b"\x00\x01"          # TYPE A
        b"\x00\x01"          # CLASS IN
        b"\x00\x00\x00\x3c"  # TTL 60s
        b"\x00\x04"          # RDLENGTH 4
        + ip_bytes
    )
    return header + question + answer


# ----- form parsing -------------------------------------------------------

def _unquote(s):
    s = s.replace("+", " ")
    out = []
    i = 0
    while i < len(s):
        if s[i] == "%" and i + 2 < len(s):
            try:
                out.append(chr(int(s[i + 1:i + 3], 16)))
                i += 3
                continue
            except ValueError:
                pass
        out.append(s[i])
        i += 1
    return "".join(out)


def parse_form(body):
    """Parse application/x-www-form-urlencoded body into a dict."""
    form = {}
    if not body:
        return form
    if isinstance(body, (bytes, bytearray)):
        body = body.decode()
    for pair in body.split("&"):
        if not pair:
            continue
        k, _, v = pair.partition("=")
        form[_unquote(k)] = _unquote(v)
    return form


# ----- save handler -------------------------------------------------------

BAD_SLUG = "bad_slug"
BAD_SLUG_MSG = ("Slug must be 1-64 characters of a-z, 0-9, '.', '_' or '-'. "
                "Nothing was saved.")


def apply_save(form, data_dir="data"):
    """Apply a submitted form to flash config. Returns one of:
    'wifi' (a network was added -> reboot into RUN mode),
    'removed', 'device', '' (nothing actionable), or BAD_SLUG (the slug is
    invalid; nothing was saved, so the page can say so instead of rebooting).
    """
    action = ""
    remove = form.get("remove_ssid", "").strip()
    if remove:
        nets = config.remove_wifi(config.load_wifi(data_dir), remove)
        config.save_wifi(nets, data_dir)
        return "removed"

    # The slug ends up in the X-Slug header, so a CR/LF must never get through.
    # Validate before writing anything; blank still means "keep the current one".
    slug = form.get("slug", "").strip().lower()
    if slug and not config.valid_slug(slug):
        return BAD_SLUG

    ssid = form.get("ssid", "").strip()
    if ssid:
        priority = 0
        try:
            priority = int(form.get("priority", "0") or "0")
        except ValueError:
            pass
        nets = config.upsert_wifi(config.load_wifi(data_dir), ssid,
                                  form.get("psk", ""), priority)
        config.save_wifi(nets, data_dir)
        action = "wifi"

    dev = config.load_device(data_dir)
    changed = False
    if slug and slug != dev.slug:
        dev.slug = slug
        changed = True
    host = form.get("server_host", "").strip()
    if host and host != dev.server_host:
        dev.server_host = host
        changed = True
    if form.get("player_name", "").strip():
        dev.player_name = form["player_name"].strip()
        changed = True
    if "brightness" in form:
        try:
            dev.brightness = max(0.0, min(1.0, float(form["brightness"])))
            changed = True
        except ValueError:
            pass
    if changed:
        config.save_device(dev, data_dir)
        if action == "":
            action = "device"
    return action


# ----- HTML ---------------------------------------------------------------

def _esc(v):
    return str(v).replace("&", "&amp;").replace("<", "&lt;").replace('"', "&quot;")


def render_page(data_dir="data", ssids=None, message=""):
    dev = config.load_device(data_dir)
    nets = config.load_wifi(data_dir)
    ssids = ssids or []

    scan = ""
    if ssids:
        opts = "".join('<option value="%s">%s</option>' % (_esc(s), _esc(s)) for s in ssids)
        scan = ("<select name=ssid_pick onchange=\"document.getElementById('ssid')"
                ".value=this.value\"><option value=''>-- pick from scan --</option>"
                + opts + "</select>")

    known = "".join(
        '<li>%s (priority %d) <form method=POST style=display:inline>'
        '<input type=hidden name=remove_ssid value="%s"><button>remove</button></form></li>'
        % (_esc(n["ssid"]), n.get("priority", 0), _esc(n["ssid"]))
        for n in nets
    ) or "<li><em>none yet</em></li>"

    msg = '<p class="msg">%s</p>' % _esc(message) if message else ""

    parts = [
        "<!doctype html><html><head><meta charset=utf-8>",
        "<meta name=viewport content='width=device-width,initial-scale=1'>",
        "<title>Health Bar Setup</title><style>",
        "body{font-family:sans-serif;max-width:30em;margin:1em auto;padding:0 1em}",
        "input,select,button{font-size:1em;padding:.3em;margin:.2em 0;width:100%}",
        ".msg{background:#dfd;padding:.5em;border-radius:4px}fieldset{margin:1em 0}",
        "small{color:#666}</style></head><body><h1>D&amp;D Health Bar</h1>",
        msg,
        "<form method=POST>",
        "<fieldset><legend>WiFi</legend><label>Network", scan,
        "<input id=ssid name=ssid placeholder='SSID'></label>",
        "<label>Password<input name=psk type=password placeholder='WiFi password'></label>",
        "<label>Priority<input name=priority type=number value=0></label></fieldset>",
        "<fieldset><legend>Character</legend>",
        "<label>Character slug (the bar polls http://server/&lt;slug&gt;.txt)",
        "<input name=slug placeholder='e.g. alishba' value=\"%s\"></label>" % _esc(dev.slug),
        "<label>Server host",
        "<input name=server_host value=\"%s\"></label>" % _esc(dev.server_host),
        "<label>Brightness (0-1)<input name=brightness value=\"%s\"></label>" % _esc(dev.brightness),
        "</fieldset>",
        "<button type=submit>Save</button></form>",
        "<h3>Known networks</h3><ul>", known, "</ul>",
        "<p><small>Add a network and Save; the bar reboots and connects "
        "automatically.</small></p></body></html>",
    ]
    return "".join(parts)


def _redirect(ip):
    return (
        "HTTP/1.1 302 Found\r\nLocation: http://%s/\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n" % ip
    )


def _ok_html(html):
    body = html.encode()
    return b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\n" \
           b"Connection: close\r\n\r\n" % len(body) + body


def is_activity(method, path):
    """Does this request mean someone is using the portal? Any POST (a save) or
    a GET of the form page; OS captive-probe redirects don't count."""
    return method == "POST" or path == "/"


def _default_ticks():
    import time
    try:
        return time.ticks_ms()
    except AttributeError:
        return int(time.monotonic() * 1000)


# ----- async server -------------------------------------------------------

class Portal:
    def __init__(self, net, data_dir="data", reset=None, ticks=None):
        self.net = net
        self.data_dir = data_dir
        self._ssids = net.scan() if net else []
        self.ip = net.start_ap() if net else "192.168.4.1"
        self._reset = reset
        self._should_reboot = False
        self._ticks = ticks or _default_ticks
        # Tick of the last real use of the portal (see is_activity), or None.
        # main.py reboots out of setup mode once it has been idle long enough.
        self.last_activity = None

    async def serve(self):
        import uasyncio
        uasyncio.create_task(self._dns_task())
        await uasyncio.start_server(self._http_client, "0.0.0.0", 80)
        while not self._should_reboot:
            await uasyncio.sleep(0.2)
        await uasyncio.sleep(1)  # let the "saved" page flush to the browser
        if self._reset:
            self._reset()

    async def _dns_task(self):
        import socket
        import uasyncio
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setblocking(False)
        s.bind(("0.0.0.0", 53))
        ipb = ip_to_bytes(self.ip)
        while not self._should_reboot:
            try:
                data, addr = s.recvfrom(256)
                if data:
                    s.sendto(build_dns_response(data, ipb), addr)
            except OSError:
                await uasyncio.sleep(0.05)
        s.close()

    async def _http_client(self, reader, writer):
        try:
            line = await reader.readline()
            parts = line.split(b" ")
            method = parts[0].decode() if parts else "GET"
            path = parts[1].decode() if len(parts) > 1 else "/"
            if is_activity(method, path):
                # Stamp before reading the body / saving, so the idle reset
                # can't fire while a save is in flight.
                self.last_activity = self._ticks()
            length = 0
            while True:
                h = await reader.readline()
                if h in (b"\r\n", b"", b"\n"):
                    break
                k, _, v = h.partition(b":")
                if k.strip().lower() == b"content-length":
                    try:
                        length = int(v.strip())
                    except ValueError:
                        length = 0
            body = await reader.readexactly(length) if length else b""
            writer.write(self._respond(method, path, body))
            await writer.drain()
        except Exception:
            pass
        finally:
            try:
                await writer.aclose()
            except Exception:
                pass

    def _respond(self, method, path, body):
        if method == "POST":
            action = apply_save(parse_form(body), self.data_dir)
            if action == BAD_SLUG:
                return _ok_html(render_page(self.data_dir, self._ssids, BAD_SLUG_MSG))
            if action in ("wifi", "device"):
                self._should_reboot = True
                return _ok_html("<h1>Saved &mdash; rebooting&hellip;</h1>"
                                "<p>The bar is applying your settings now.</p>")
            return _ok_html(render_page(self.data_dir, self._ssids, "Saved."))
        if path != "/":
            return _redirect(self.ip)  # OS probes etc.: not a sign anyone is here
        return _ok_html(render_page(self.data_dir, self._ssids))
