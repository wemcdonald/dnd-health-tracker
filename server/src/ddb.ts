/**
 * D&D Beyond character-service client.
 *
 * Fetches a public character's v5 document over HTTPS and returns the parsed
 * `data` object for `hpFromCharacterData`. Unlike the firmware's `ddb.py`, the
 * server has TLS + gzip + plenty of RAM, so this is a plain `fetch` + `json()`.
 * No auth is required for public characters (auth is only for the WSS push).
 */

import { hpFromCharacterData, type Hp } from "./hp.js";

const HOST = "character-service.dndbeyond.com";
const PATH_BASE = "/character/v5/character/";
const ORIGIN = "https://www.dndbeyond.com";
export const USER_AGENT =
  "dnd-health-tracker/2.0 (server; +https://github.com/will/dnd-health-tracker)";

export class HttpStatusError extends Error {
  constructor(public readonly status: number) {
    super(`ddb: http status ${status}`);
    this.name = "HttpStatusError";
  }
  requiresAuth(): boolean {
    return this.status === 401 || this.status === 403;
  }
}

export class PrivateSheetError extends Error {
  constructor() {
    super("ddb: character sheet is private (401/403)");
    this.name = "PrivateSheetError";
  }
}

/**
 * Fetch and compute HP for a character id.
 *
 * Public sheets need no auth. For a *private* sheet, pass `token` — the
 * short-lived bearer minted from the Cobalt cookie (see `CookieAuth`) — and the
 * character-service authorizes the same account that owns the sheet.
 * @param opts.token  optional `Bearer` token for private sheets.
 * @param opts.timeoutMs  abort the request after this long (default 10s).
 */
export async function fetchHp(
  characterId: string,
  opts: { token?: string | undefined; timeoutMs?: number } = {},
): Promise<Hp> {
  if (!characterId) throw new Error("ddb: empty character id");
  const { token, timeoutMs = 10_000 } = opts;

  const url = `https://${HOST}${PATH_BASE}${encodeURIComponent(characterId)}`;
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeoutMs);
  let res: Response;
  try {
    res = await fetch(url, {
      method: "GET",
      headers: {
        Accept: "application/json, text/plain, */*",
        Origin: ORIGIN,
        Referer: `${ORIGIN}/`,
        "User-Agent": USER_AGENT,
        ...(token ? { Authorization: `Bearer ${token}` } : {}),
      },
      signal: ctrl.signal,
    });
  } finally {
    clearTimeout(timer);
  }

  if (res.status === 401 || res.status === 403) throw new PrivateSheetError();
  if (res.status !== 200) throw new HttpStatusError(res.status);

  const body = (await res.json()) as { data?: Record<string, unknown> | null };
  if (body == null || body.data == null) {
    throw new Error("ddb: response had no character data (private sheet or bad id?)");
  }
  return hpFromCharacterData(body.data);
}
