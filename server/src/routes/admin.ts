/**
 * Admin UI (server-rendered, no client framework).
 *
 *   GET  /                              -> dashboard: characters + live state + forms
 *   POST /admin/characters             -> add/update a character
 *   POST /admin/characters/:slug/delete-> remove a character
 *   POST /admin/settings               -> set the Cobalt cookie (for WSS)
 *   POST /admin/devices/:mac           -> set a board's managed config (label/slug/brightness/pollSeconds)
 *   POST /admin/devices/:mac/wifi      -> add/update or remove a managed WiFi row for a board
 *   POST /admin/devices/:mac/wifi/delete -> stop managing a WiFi row for a board
 *   POST /admin/devices/:mac/forget    -> clear a board's token and managed WiFi (next check-in re-registers)
 *   POST /admin/devices/:mac/delete    -> remove a board and its managed settings
 *
 * SECURITY: this UI has no built-in auth and exposes setting the Cobalt cookie (a
 * full DDB account credential). Run it behind your reverse proxy's auth / on a
 * trusted network. As a light guard, if ADMIN_PASSWORD is set, a matching
 * `?key=` (or `key` form field) is required. The cookie is never rendered back.
 */

import type { FastifyInstance, FastifyReply, FastifyRequest } from "fastify";
import {
  type Character,
  type Device,
  listCharacters,
  upsertCharacter,
  deleteCharacter,
  getSetting,
  setSetting,
  COBALT_COOKIE_KEY,
  listDevices,
  getDevice,
  listDeviceWifi,
  updateDeviceConfig,
  upsertDeviceWifi,
  deleteDeviceWifi,
  forgetDevice,
  deleteDevice,
} from "../db.js";
import { allLiveStates, syncManagers } from "../manager.js";
import { formatMac, MAC_RE, SLUG_RE } from "../devices.js";

const ADMIN_PASSWORD = process.env["ADMIN_PASSWORD"] ?? "";

function esc(s: unknown): string {
  return String(s ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

/** Extract the numeric character id from a DDB URL or accept a bare id. */
export function parseCharacterId(input: string): string | null {
  const trimmed = input.trim();
  if (/^\d+$/.test(trimmed)) return trimmed;
  const m = trimmed.match(/characters\/(\d+)/);
  return m ? (m[1] as string) : null;
}

function authorized(req: FastifyRequest): boolean {
  if (!ADMIN_PASSWORD) return true;
  const q = (req.query as Record<string, unknown>)["key"];
  const b = (req.body as Record<string, unknown> | undefined)?.["key"];
  return q === ADMIN_PASSWORD || b === ADMIN_PASSWORD;
}

function denied(reply: FastifyReply): FastifyReply {
  return reply.code(401).type("text/plain").send("unauthorized (ADMIN_PASSWORD required)\n");
}

/** Parse an optional numeric form field: blank -> null (unmanaged); NaN/out of range -> "invalid". */
function optionalNumber(raw: string | undefined, lo: number, hi: number): number | null | "invalid" {
  const s = (raw ?? "").trim();
  if (s === "") return null;
  const n = Number(s);
  return Number.isFinite(n) && n >= lo && n <= hi ? n : "invalid";
}

function ago(ms: number): string {
  const s = Math.max(0, Math.floor((Date.now() - ms) / 1000));
  return s < 120 ? `${s}s ago` : s < 7200 ? `${Math.floor(s / 60)}m ago` : `${Math.floor(s / 3600)}h ago`;
}

function boardsSection(knownSlugs: ReadonlySet<string>): string {
  const keyField = ADMIN_PASSWORD ? '<label>admin key <input type="text" name="key"></label>' : "";
  const cards = listDevices().map((d: Device) => {
    const base = `/admin/devices/${esc(d.mac)}`;
    const slugWarn = d.reportedSlug && !knownSlugs.has(d.reportedSlug)
      ? ` <span class="err">(not a known character)</span>` : "";
    const wifiRows = listDeviceWifi(d.mac).map((w) => `<tr>
        <td>${esc(w.ssid)}</td><td>${esc(w.action)}</td><td>${w.action === "upsert" ? esc(w.priority) : ""}</td>
        <td><form method="POST" action="${base}/wifi/delete" style="display:inline">${keyField}
          <input type="hidden" name="ssid" value="${esc(w.ssid)}"><button>drop</button></form></td>
      </tr>`).join("\n");
    const rejectedWarn =
      d.lastRejected !== null && d.lastRejected > d.lastSeen
        ? `<p><small class="err">rejected check-in ${esc(ago(d.lastRejected))} — board was probably re-flashed ` +
          `(new token); click &quot;forget token&quot; so it can re-register</small></p>`
        : "";
    return `<fieldset>
<legend><code>${esc(formatMac(d.mac))}</code> ${esc(d.label)}</legend>
<p><small>last seen ${esc(ago(d.lastSeen))} · fw v${esc(d.fwVersion || "?")} · LAN ${esc(d.localIp || "?")}
 · showing <code>${esc(d.reportedSlug || "—")}</code>${slugWarn} · rev ${d.configRev}
 ${d.tokenSha256 === null ? " · <b>forgotten: re-registers on next check-in (managed WiFi cleared)</b>" : ""}</small></p>
${rejectedWarn}
<form method="POST" action="${base}">${keyField}
  <label>label <input type="text" name="label" value="${esc(d.label)}"></label>
  <label>slug (blank = not managed) <input type="text" name="slug" value="${esc(d.slug ?? "")}" list="slugs"></label>
  <label>brightness 0–1 (blank = not managed) <input type="text" name="brightness" value="${esc(d.brightness ?? "")}"></label>
  <label>poll seconds 2–300 (blank = not managed) <input type="text" name="pollSeconds" value="${esc(d.pollSeconds ?? "")}"></label>
  <button type="submit">save</button>
</form>
<table><tr><th>wifi ssid</th><th>action</th><th>priority</th><th></th></tr>
${wifiRows || '<tr><td colspan="4"><em>no managed networks</em></td></tr>'}</table>
<form method="POST" action="${base}/wifi">${keyField}
  <label>ssid <input type="text" name="ssid" required></label>
  <label>password (leave blank to keep the saved password) <input type="password" name="psk"></label>
  <label><input type="checkbox" name="open"> open network (no password)</label>
  <label>priority -100–100 <input type="text" name="priority" value="0"></label>
  <label>action <select name="action"><option value="upsert">add/update</option><option value="remove">remove from board</option></select></label>
  <button type="submit">save network</button>
</form>
<form method="POST" action="${base}/forget" style="display:inline" onsubmit="return confirm('Forget this board? Its next check-in re-registers; managed WiFi networks (and their passwords) are deleted.')">${keyField}<button>forget token</button></form>
<form method="POST" action="${base}/delete" style="display:inline" onsubmit="return confirm('Delete this board and its settings?')">${keyField}<button>delete board</button></form>
</fieldset>`;
  }).join("\n");
  const options = [...knownSlugs].map((s) => `<option value="${esc(s)}">`).join("");
  return `<h2>Boards</h2>
<p><small>Boards register themselves on first check-in (<code>/device/&lt;mac&gt;/config</code>) and pick up changes within ~5 min.
WiFi rows are add/update or remove; dropping a row just stops managing it. If a board was re-flashed with a new
token, its check-ins get rejected until you click "forget token" on that card.</small></p>
<datalist id="slugs">${options}</datalist>
${cards || "<p><em>no boards have checked in yet</em></p>"}`;
}

function page(): string {
  const states = new Map(allLiveStates().map((s) => [s.slug, s]));
  const chars = listCharacters();
  const cobaltSet = Boolean(getSetting(COBALT_COOKIE_KEY));

  const rows = chars
    .map((c) => {
      const s = states.get(c.slug);
      const dot = s?.online ? "🟢" : "🔴";
      const ws = s?.wsConnected ? "ws✓" : "ws–";
      const hp = s ? `${s.cur}/${s.max} (+${s.temp}) ${s.pct}%` : "—";
      const lit = s ? s.lit : "—";
      const age =
        s && s.lastUpstreamSuccessMs
          ? `${Math.floor((Date.now() - s.lastUpstreamSuccessMs) / 1000)}s`
          : "never";
      const err = s?.lastError ? `<div class="err">${esc(s.lastError)}</div>` : "";
      return `<tr>
        <td><code>${esc(c.slug)}</code></td>
        <td>${esc(c.characterId)}</td>
        <td>${dot} ${esc(hp)}<br><small>lit ${esc(lit)} · age ${esc(age)} · ${ws}</small>${err}</td>
        <td>${c.enabled ? "yes" : "no"}</td>
        <td>
          <a href="/${esc(c.slug)}.txt" target="_blank">.txt</a>
          <form method="POST" action="/admin/characters/${esc(c.slug)}/delete" style="display:inline"
                onsubmit="return confirm('Delete ${esc(c.slug)}?')">
            ${ADMIN_PASSWORD ? '<input type="hidden" name="key" value="">' : ""}
            <button>delete</button>
          </form>
        </td>
      </tr>`;
    })
    .join("\n");

  return `<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>D&D Health Tracker — admin</title>
<style>
  body { font: 15px/1.5 system-ui, sans-serif; max-width: 880px; margin: 2rem auto; padding: 0 1rem; }
  table { border-collapse: collapse; width: 100%; margin: 1rem 0; }
  th, td { border: 1px solid #ddd; padding: .4rem .6rem; text-align: left; vertical-align: top; }
  th { background: #f5f5f5; }
  code { background: #f0f0f0; padding: 0 .2rem; }
  .err { color: #b00; font-size: 12px; }
  fieldset { margin: 1.5rem 0; }
  label { display: block; margin: .4rem 0; }
  input[type=text] { width: 100%; max-width: 460px; padding: .3rem; }
  button { padding: .3rem .8rem; }
  small { color: #666; }
</style></head><body>
<h1>D&D Health Tracker</h1>
<p>Each character publishes <code>/&lt;slug&gt;.txt</code> for the LED bars to poll.</p>
${ADMIN_PASSWORD ? '<p><small>ADMIN_PASSWORD is set — append <code>?key=…</code> and fill the key field on forms.</small></p>' : ""}

<table>
<tr><th>slug</th><th>char id</th><th>state</th><th>enabled</th><th></th></tr>
${rows || '<tr><td colspan="5"><em>no characters yet</em></td></tr>'}
</table>

<fieldset>
<legend>Add / update character</legend>
<form method="POST" action="/admin/characters">
  ${ADMIN_PASSWORD ? '<label>admin key <input type="text" name="key"></label>' : ""}
  <label>slug (used in the URL, e.g. <code>thorin</code>) <input type="text" name="slug" required></label>
  <label>character URL or id (e.g. https://www.dndbeyond.com/characters/12345678) <input type="text" name="characterRef" required></label>
  <label>user id (optional, for WSS) <input type="text" name="userId"></label>
  <label>game id (optional, for WSS) <input type="text" name="gameId"></label>
  <label><input type="checkbox" name="enabled" checked> enabled</label>
  <button type="submit">save</button>
</form>
</fieldset>

${boardsSection(new Set(chars.map((c) => c.slug)))}

<fieldset>
<legend>Cobalt cookie (for the WSS fast path)</legend>
<p><small>Currently ${cobaltSet ? "<b>set</b>" : "<b>not set</b>"} — polling works without it. Paste the full <code>Cobalt</code> cookie (e.g. <code>CobaltSession=…</code>). Never displayed back.</small></p>
<form method="POST" action="/admin/settings">
  ${ADMIN_PASSWORD ? '<label>admin key <input type="text" name="key"></label>' : ""}
  <label>cobalt cookie <input type="text" name="cobaltCookie"></label>
  <button type="submit">save cookie</button>
</form>
</fieldset>
</body></html>`;
}

export async function adminRoutes(app: FastifyInstance): Promise<void> {
  app.get("/", async (req, reply) => {
    if (!authorized(req)) return denied(reply);
    return reply.type("text/html; charset=utf-8").send(page());
  });

  app.post<{ Body: Record<string, string> }>("/admin/characters", async (req, reply) => {
    if (!authorized(req)) return denied(reply);
    const b = req.body ?? {};
    const slug = (b["slug"] ?? "").trim().toLowerCase();
    const characterId = parseCharacterId(b["characterRef"] ?? "");
    if (!slug || !/^[a-z0-9._-]+$/.test(slug)) {
      return reply.code(400).type("text/plain").send("invalid slug (use a-z 0-9 . _ -)\n");
    }
    if (!characterId) {
      return reply.code(400).type("text/plain").send("could not parse a character id from input\n");
    }
    const character: Character = {
      slug,
      characterId,
      userId: (b["userId"] ?? "").trim(),
      gameId: (b["gameId"] ?? "").trim(),
      enabled: b["enabled"] === "on" || b["enabled"] === "true",
    };
    upsertCharacter(character);
    syncManagers();
    return reply.redirect("/");
  });

  app.post<{ Params: { slug: string } }>("/admin/characters/:slug/delete", async (req, reply) => {
    if (!authorized(req)) return denied(reply);
    deleteCharacter(req.params.slug);
    syncManagers();
    return reply.redirect("/");
  });

  app.post<{ Body: Record<string, string> }>("/admin/settings", async (req, reply) => {
    if (!authorized(req)) return denied(reply);
    const cookie = (req.body?.["cobaltCookie"] ?? "").trim();
    if (cookie) {
      setSetting(COBALT_COOKIE_KEY, cookie);
      syncManagers(); // rewire WSS with the new cookie
    }
    return reply.redirect("/");
  });

  /** Validate a MAC param and look up the board, replying 400/404 as needed. Returns undefined on failure. */
  function knownBoard(mac: string, reply: FastifyReply): Device | undefined {
    if (!MAC_RE.test(mac)) {
      reply.code(400).type("text/plain").send("bad mac\n");
      return undefined;
    }
    const d = getDevice(mac);
    if (!d) {
      reply.code(404).type("text/plain").send("unknown board\n");
      return undefined;
    }
    return d;
  }

  app.post<{ Params: { mac: string }; Body: Record<string, string> }>("/admin/devices/:mac", async (req, reply) => {
    if (!authorized(req)) return denied(reply);
    const mac = req.params.mac;
    if (!knownBoard(mac, reply)) return reply;
    const b = req.body ?? {};
    const slugRaw = (b["slug"] ?? "").trim().toLowerCase();
    if (slugRaw && !SLUG_RE.test(slugRaw)) {
      return reply.code(400).type("text/plain").send("invalid slug (use a-z 0-9 . _ -)\n");
    }
    const brightness = optionalNumber(b["brightness"], 0, 1);
    const pollSeconds = optionalNumber(b["pollSeconds"], 2, 300);
    if (brightness === "invalid" || pollSeconds === "invalid") {
      return reply.code(400).type("text/plain").send("brightness must be 0–1, poll seconds 2–300\n");
    }
    updateDeviceConfig(mac, {
      label: (b["label"] ?? "").trim(),
      slug: slugRaw || null,
      brightness,
      pollSeconds,
    });
    return reply.redirect("/");
  });

  app.post<{ Params: { mac: string }; Body: Record<string, string> }>("/admin/devices/:mac/wifi", async (req, reply) => {
    if (!authorized(req)) return denied(reply);
    const mac = req.params.mac;
    if (!knownBoard(mac, reply)) return reply;
    const b = req.body ?? {};
    const ssid = (b["ssid"] ?? "").trim();
    const action = b["action"] === "remove" ? "remove" : "upsert";
    const open = b["open"] === "on";
    const priority = Number((b["priority"] ?? "0").trim() || "0");
    if (
      !ssid ||
      Buffer.byteLength(ssid, "utf8") > 32 ||
      !Number.isInteger(priority) ||
      priority < -100 ||
      priority > 100
    ) {
      return reply.code(400).type("text/plain").send("need a valid ssid (<=32 bytes) and integer priority -100..100\n");
    }
    upsertDeviceWifi({ mac, ssid, psk: action === "upsert" ? b["psk"] ?? "" : "", priority, action }, { open });
    return reply.redirect("/");
  });

  app.post<{ Params: { mac: string }; Body: Record<string, string> }>("/admin/devices/:mac/wifi/delete", async (req, reply) => {
    if (!authorized(req)) return denied(reply);
    const mac = req.params.mac;
    if (!knownBoard(mac, reply)) return reply;
    deleteDeviceWifi(mac, (req.body?.["ssid"] ?? "").trim());
    return reply.redirect("/");
  });

  app.post<{ Params: { mac: string } }>("/admin/devices/:mac/forget", async (req, reply) => {
    if (!authorized(req)) return denied(reply);
    const mac = req.params.mac;
    if (!knownBoard(mac, reply)) return reply;
    forgetDevice(mac);
    return reply.redirect("/");
  });

  app.post<{ Params: { mac: string } }>("/admin/devices/:mac/delete", async (req, reply) => {
    if (!authorized(req)) return denied(reply);
    const mac = req.params.mac;
    if (!knownBoard(mac, reply)) return reply;
    deleteDevice(mac);
    return reply.redirect("/");
  });
}
