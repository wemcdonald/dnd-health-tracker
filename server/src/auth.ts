/**
 * D&D Beyond Cobalt-token auth (for the game-log websocket only).
 *
 * Port of the firmware's `auth.py` / `internal/ddb/auth.go`. Exchanges a durable
 * Cobalt session cookie for the short-lived bearer ("stt") token the WSS push
 * needs, caching it and refreshing ~30s before expiry. The HP fetch itself stays
 * public/unauthenticated, so a missing/expired cookie degrades to polling-only.
 *
 * The Cobalt cookie is a full account credential: it is stored in the DB, never
 * echoed back by the admin UI, and only read here.
 */

import { USER_AGENT } from "./ddb.js";

const AUTH_HOST = "auth-service.dndbeyond.com";
const TOKEN_PATH = "/v1/cobalt-token";
const ORIGIN = "https://www.dndbeyond.com";
const TOKEN_SAFETY_S = 30; // refresh this many seconds before actual expiry
const DEFAULT_TTL_S = 300; // fallback lifetime if the response omits ttl

/** Turns a Cobalt cookie into a cached, auto-refreshing bearer token. */
export class CookieAuth {
  private token: string | null = null;
  private expiresAtMs = 0;

  constructor(private readonly cookie: string) {}

  /** Return a currently-valid bearer token, refreshing if needed. */
  async getToken(): Promise<string> {
    if (this.token && Date.now() < this.expiresAtMs) return this.token;
    return this.refresh();
  }

  private async refresh(): Promise<string> {
    if (!this.cookie) throw new Error("ddb: no cobalt cookie configured");

    const res = await fetch(`https://${AUTH_HOST}${TOKEN_PATH}`, {
      method: "POST",
      headers: {
        Accept: "*/*",
        Origin: ORIGIN,
        Referer: `${ORIGIN}/`,
        "User-Agent": USER_AGENT,
        // The cobalt cookie is passed verbatim; it already includes name=value.
        Cookie: this.cookie,
        "Content-Length": "0",
      },
    });
    if (res.status !== 200) throw new Error(`ddb: cobalt-token http status ${res.status}`);

    const data = (await res.json()) as { token?: string; ttl?: number };
    if (!data.token) throw new Error("ddb: cobalt-token response had no token");

    let ttl = data.ttl ?? 0;
    if (ttl <= TOKEN_SAFETY_S) ttl = DEFAULT_TTL_S;
    this.token = data.token;
    this.expiresAtMs = Date.now() + (ttl - TOKEN_SAFETY_S) * 1000;
    return this.token;
  }
}
