"""Setup mode: captive portal (DNS) + minimal web config UI.

Replaces the Go captive-portal logic (``internal/netcfg`` + ``internal/web``),
which leaned on dnsmasq and a full HTTP server. Here we hand-roll:

  - a tiny UDP DNS responder that answers every A query with the AP's own IP,
    so a phone's captive-portal probe lands on us;
  - a minimal async HTTP server that serves a config form, 302-redirects OS
    probe URLs to it, and on save writes config to flash and reboots into RUN
    mode.

Setup mode and RUN mode are mutually exclusive, so this server is never resident
while the TLS poll loop is running — that is the main RAM lever (see design doc).

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
    """Build an A-record reply pointing the queried name at ip_bytes.

    Answers any name (wildcard) — the essence of a captive portal.
    """
    if len(query) < 12:
        return b""
    tid = query[0:2]
    header = tid + b"\x81\x80" + b"\x00\x01" + b"\x00\x01" + b"\x00\x00" + b"\x00\x00"
    # walk the QNAME labels to find where the question's type/class begin
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

def apply_save(form, data_dir="data"):
    """Apply a submitted form to flash config. Returns one of:
    'wifi' (a network was added -> caller should reboot into RUN mode),
    'removed', 'device', or '' (nothing actionable).
    """
    action = ""
    remove = form.get("remove_ssid", "").strip()
    if remove:
        nets = config.remove_wifi(config.load_wifi(data_dir), remove)
        config.save_wifi(nets, data_dir)
        return "removed"

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
    cid = form.get("character_id", "").strip()
    if cid and cid != dev.character_id:
        dev.character_id = cid
        changed = True
    if "brightness" in form:
        try:
            dev.brightness = max(0.0, min(1.0, float(form["brightness"])))
            changed = True
        except ValueError:
            pass
    if form.get("player_name", "").strip():
        dev.player_name = form["player_name"].strip()
        changed = True
    # Optional websocket fields.
    for field in ("user_id", "game_id"):
        v = form.get(field, "").strip()
        if v != getattr(dev, field):
            setattr(dev, field, v)
            changed = True
    if changed:
        config.save_device(dev, data_dir)
        if action == "":
            action = "device"

    # Cobalt cookie -> secrets (only when provided; empty leaves it untouched).
    cookie = form.get("cobalt_cookie", "").strip()
    if cookie:
        config.save_secrets({"cobalt_cookie": cookie}, data_dir)
        if action == "":
            action = "device"
    return action


# ----- HTML ---------------------------------------------------------------

def _esc(v):
    return str(v).replace("&", "&amp;").replace("<", "&lt;").replace('"', "&quot;")


def render_page(data_dir="data", ssids=None, message=""):
    dev = config.load_device(data_dir)
    nets = config.load_wifi(data_dir)
    has_cookie = bool(config.cobalt_cookie(data_dir))
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

    cookie_note = "saved ✓ (leave blank to keep)" if has_cookie else "not set"
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
        "<label>D&amp;D Beyond character ID (sheet must be public)",
        "<input name=character_id placeholder='e.g. 12345678' value=\"%s\"></label>" % _esc(dev.character_id),
        "<label>Brightness (0-1)<input name=brightness value=\"%s\"></label></fieldset>" % _esc(dev.brightness),
        "<fieldset><legend>Live updates (optional)</legend>",
        "<small>Instant updates via your D&amp;D Beyond game log. Needs a campaign. "
        "The cookie is your account login &mdash; only add it on trusted devices.</small>",
        "<label>Game ID<input name=game_id value=\"%s\"></label>" % _esc(dev.game_id),
        "<label>User ID<input name=user_id value=\"%s\"></label>" % _esc(dev.user_id),
        "<label>Cobalt cookie (%s)<input name=cobalt_cookie type=password></label>" % cookie_note,
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


# ----- async server -------------------------------------------------------

class Portal:
    def __init__(self, net, data_dir="data", reset=None):
        self.net = net
        self.data_dir = data_dir
        self.ip = net.start_ap() if net else "192.168.4.1"
        self._ssids = net.scan() if net else []
        self._reset = reset
        self._should_reboot = False

    async def serve(self):
        import uasyncio
        uasyncio.create_task(self._dns_task())
        server = await uasyncio.start_server(self._http_client, "0.0.0.0", 80)
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
            # A new network or character could make RUN mode viable -> reboot so
            # the supervisor re-evaluates. A bare 'removed' just re-renders.
            if action in ("wifi", "device"):
                self._should_reboot = True
                return _ok_html("<h1>Saved &mdash; rebooting&hellip;</h1>"
                                "<p>The bar is applying your settings now.</p>")
            return _ok_html(render_page(self.data_dir, self._ssids, "Saved."))
        # We only serve the form at "/"; every other path (incl. OS probe URLs)
        # 302-redirects there, which is what triggers the captive-portal sheet.
        if path != "/":
            return _redirect(self.ip)
        return _ok_html(render_page(self.data_dir, self._ssids))
