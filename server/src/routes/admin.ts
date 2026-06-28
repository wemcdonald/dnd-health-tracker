/**
 * Admin UI (server-rendered, no client framework).
 *
 *   GET  /                              -> dashboard: characters + live state + forms
 *   POST /admin/characters             -> add/update a character
 *   POST /admin/characters/:slug/delete-> remove a character
 *   POST /admin/settings               -> set the Cobalt cookie (for WSS)
 *
 * SECURITY: this UI has no built-in auth and exposes setting the Cobalt cookie (a
 * full DDB account credential). Run it behind your reverse proxy's auth / on a
 * trusted network. As a light guard, if ADMIN_PASSWORD is set, a matching
 * `?key=` (or `key` form field) is required. The cookie is never rendered back.
 */

import type { FastifyInstance, FastifyReply, FastifyRequest } from "fastify";
import {
  type Character,
  listCharacters,
  upsertCharacter,
  deleteCharacter,
  getSetting,
  setSetting,
  COBALT_COOKIE_KEY,
} from "../db.js";
import { allLiveStates, syncManagers } from "../manager.js";

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
}
