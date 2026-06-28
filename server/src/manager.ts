/**
 * Per-character manager: owns the live HP state, polls character-service on an
 * adaptive cadence, and (when a Cobalt cookie + user/game ids are present)
 * subscribes to the game-log websocket to nudge an immediate refetch.
 *
 * Cadence mirrors the firmware's `Poller`: a steady ~5s base, relaxed to ~30s
 * while the WSS is alive (so live events drive freshness instead), with
 * exponential backoff on errors. WSS is never a hard dependency — if it drops,
 * polling tightens straight back to the responsive rate.
 */

import { fetchHp } from "./ddb.js";
import { litFromHp } from "./hp.js";
import { CookieAuth } from "./auth.js";
import { WsListener } from "./ws.js";
import {
  type Character,
  listCharacters,
  getSetting,
  COBALT_COOKIE_KEY,
} from "./db.js";

const BASE_INTERVAL_MS = 5_000;
const RELAXED_INTERVAL_MS = 30_000;
const ERR_BACKOFF_BASE_MS = 2_000;
const ERR_BACKOFF_CAP_MS = 60_000;

// Suspend a character entirely (no D&D Beyond polling, no game-log WSS) once its
// device file hasn't been requested for this long. We only play once a month, so
// there is no point polling when every device is powered off. The next request
// for the character wakes it and triggers an immediate refetch.
const IDLE_MS = Number(process.env["IDLE_TIMEOUT_MS"] ?? 5 * 60_000);

export interface LiveState {
  slug: string;
  lit: number;
  cur: number;
  max: number;
  temp: number;
  pct: number;
  online: boolean; // last upstream refresh succeeded
  wsConnected: boolean;
  suspended: boolean; // idle: no recent requests, so polling + WSS are paused
  lastUpstreamSuccessMs: number; // 0 = never
  lastError: string | null;
}

function jitter(ms: number): number {
  // +/-15% so a fleet of characters doesn't poll in lockstep.
  return ms + (Math.random() * 0.3 - 0.15) * ms;
}

class CharacterManager {
  private readonly state: LiveState;
  private ws: WsListener | null = null;
  // Set when a Cobalt cookie is configured. Mints the bearer token used for both
  // the HP fetch (so private sheets resolve) and the game-log WSS.
  private auth: CookieAuth | null = null;
  private stopped = false;
  private errorCount = 0;
  private pendingNudge = false;
  private wake: (() => void) | null = null;
  private wakeTimer: NodeJS.Timeout | null = null;
  // Idle suspension: last time a device asked for this character's file, and the
  // resolver the suspended loop is parked on until the next request wakes it.
  private lastRequestMs = Date.now();
  private suspended = false;
  private resumeWaiter: (() => void) | null = null;

  constructor(
    private readonly character: Character,
    cobaltCookie: string | undefined,
  ) {
    this.state = {
      slug: character.slug,
      lit: 0,
      cur: 0,
      max: 0,
      temp: 0,
      pct: 0,
      online: false,
      wsConnected: false,
      suspended: false,
      lastUpstreamSuccessMs: 0,
      lastError: null,
    };

    // A cookie alone authenticates the HP fetch (private sheets); the WSS push
    // additionally needs the user/game ids. Share one CookieAuth across both.
    if (cobaltCookie) {
      this.auth = new CookieAuth(cobaltCookie);
      if (character.userId && character.gameId) {
        this.ws = new WsListener(this.auth, character.gameId, character.userId, {
          onNudge: () => this.nudge(),
          onState: (connected) => {
            this.state.wsConnected = connected;
          },
        });
      }
    }
  }

  getState(): Readonly<LiveState> {
    return this.state;
  }

  start(): void {
    this.stopped = false;
    this.ws?.start();
    void this.runLoop();
  }

  stop(): void {
    this.stopped = true;
    this.ws?.stop();
    this.wake?.();
    this.resumeWaiter?.(); // unpark the loop if it's suspended
  }

  /**
   * Record a device request for this character. Keeps polling alive while
   * devices are on, and wakes the manager from idle suspension on the first
   * request after a quiet spell (which also triggers an immediate refetch).
   */
  touch(): void {
    this.lastRequestMs = Date.now();
    if (this.suspended) this.resumeWaiter?.();
  }

  /** Request an immediate refetch (from the WSS push). */
  nudge(): void {
    if (this.wake) this.wake();
    else this.pendingNudge = true;
  }

  private async runLoop(): Promise<void> {
    while (!this.stopped) {
      // If no device has asked for this character recently, suspend completely:
      // drop the WSS and park here (no D&D Beyond polling) until a request wakes
      // us. On wake we fall straight through to an immediate refresh.
      if (Date.now() - this.lastRequestMs > IDLE_MS) {
        this.enterSuspended();
        await this.waitForResume();
        if (this.stopped) break;
        this.exitSuspended();
      }

      await this.refresh();
      if (this.stopped) break;
      if (this.pendingNudge) {
        this.pendingNudge = false;
        continue; // a nudge arrived mid-refresh; refetch without waiting
      }
      await this.waitInterruptible(this.nextIntervalMs());
    }
  }

  private enterSuspended(): void {
    if (this.suspended) return;
    this.suspended = true;
    this.state.suspended = true;
    this.ws?.stop(); // stop the game-log websocket while idle
    this.state.wsConnected = false;
    console.log(`[${this.state.slug}] idle ${Math.round(IDLE_MS / 1000)}s — suspending poll + WSS`);
  }

  private exitSuspended(): void {
    this.suspended = false;
    this.state.suspended = false;
    this.ws?.start(); // resubscribe to the game-log websocket
    console.log(`[${this.state.slug}] request received — resuming poll + WSS`);
  }

  /** Park until the next touch() (or stop()) wakes the suspended loop. */
  private waitForResume(): Promise<void> {
    return new Promise<void>((resolve) => {
      this.resumeWaiter = () => {
        this.resumeWaiter = null;
        resolve();
      };
    });
  }

  private nextIntervalMs(): number {
    if (this.errorCount > 0) {
      const backoff = ERR_BACKOFF_BASE_MS * 2 ** Math.min(this.errorCount - 1, 5);
      return jitter(Math.min(backoff, ERR_BACKOFF_CAP_MS));
    }
    return jitter(this.state.wsConnected ? RELAXED_INTERVAL_MS : BASE_INTERVAL_MS);
  }

  private waitInterruptible(ms: number): Promise<void> {
    return new Promise<void>((resolve) => {
      this.wake = () => {
        if (this.wakeTimer) clearTimeout(this.wakeTimer);
        this.wakeTimer = null;
        this.wake = null;
        resolve();
      };
      this.wakeTimer = setTimeout(() => this.wake?.(), ms);
    });
  }

  private async refresh(): Promise<void> {
    try {
      // Authenticate the fetch when a cookie is configured (needed for private
      // sheets). If the token mint fails, degrade to an unauthenticated fetch —
      // public sheets still resolve, exactly as before.
      let token: string | undefined;
      if (this.auth) {
        try {
          token = await this.auth.getToken();
        } catch {
          token = undefined;
        }
      }
      const hp = await fetchHp(this.character.characterId, { token });
      const lit = litFromHp(hp.cur, hp.max);
      const pct = hp.max > 0 ? Math.round((100 * hp.cur) / hp.max) : 0;
      this.state.cur = hp.cur;
      this.state.max = hp.max;
      this.state.temp = hp.temp;
      this.state.pct = pct;
      this.state.lit = lit;
      this.state.online = true;
      this.state.lastError = null;
      this.state.lastUpstreamSuccessMs = Date.now();
      this.errorCount = 0;
    } catch (err) {
      this.state.online = false;
      this.state.lastError = err instanceof Error ? err.message : String(err);
      this.errorCount += 1;
    }
  }
}

// ---- registry ------------------------------------------------------------

const managers = new Map<string, CharacterManager>();

/**
 * Reconcile running managers with the DB: start enabled characters, stop removed
 * or disabled ones, and restart any whose config changed. Rebuilds every manager
 * on the simplest safe principle (stop all, start enabled) when the Cobalt cookie
 * changes, since that affects WSS wiring across the board.
 */
export function syncManagers(): void {
  const cobalt = getSetting(COBALT_COOKIE_KEY);
  const desired = new Map(listCharacters().filter((c) => c.enabled).map((c) => [c.slug, c]));

  // Stop managers that are no longer desired.
  for (const [slug, mgr] of managers) {
    if (!desired.has(slug)) {
      mgr.stop();
      managers.delete(slug);
    }
  }

  // Start/restart desired managers. We restart unconditionally on sync so config
  // edits (character id, user/game id, cobalt cookie) always take effect; the
  // set of characters is small, so the churn is negligible.
  for (const [slug, character] of desired) {
    const existing = managers.get(slug);
    if (existing) existing.stop();
    const mgr = new CharacterManager(character, cobalt);
    managers.set(slug, mgr);
    mgr.start();
  }
}

export function getLiveState(slug: string): Readonly<LiveState> | undefined {
  return managers.get(slug)?.getState();
}

/** Record a device request so the manager stays awake / wakes from idle. */
export function touchCharacter(slug: string): void {
  managers.get(slug)?.touch();
}

export function allLiveStates(): Readonly<LiveState>[] {
  return [...managers.values()].map((m) => m.getState());
}

export function stopAllManagers(): void {
  for (const mgr of managers.values()) mgr.stop();
  managers.clear();
}
