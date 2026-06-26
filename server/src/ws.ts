/**
 * D&D Beyond game-log websocket subscriber (optional fast path).
 *
 * Port of the firmware's `ws.py` / `internal/ddb/ws.go`, but the `ws` library
 * handles all of RFC 6455 (framing, ping/pong, close), so this is just glue.
 * We treat **every data frame as a "something changed" nudge** — bodies are
 * never parsed, so we're immune to the unofficial event schema. On connect/drop
 * we report state so the manager can relax/tighten its polling cadence; WSS is
 * never a hard dependency.
 */

import WebSocket from "ws";
import type { CookieAuth } from "./auth.js";

const WS_HOST = "game-log-api-live.dndbeyond.com";
const ORIGIN = "https://www.dndbeyond.com";
const RECONNECT_BACKOFF_MS = 5_000;

export interface WsCallbacks {
  onNudge: () => void;
  onState: (connected: boolean) => void;
}

/** Maintains the game-log websocket, nudging on every event, reconnecting forever. */
export class WsListener {
  private ws: WebSocket | null = null;
  private stopped = false;
  private reconnectTimer: NodeJS.Timeout | null = null;

  constructor(
    private readonly auth: CookieAuth,
    private readonly gameId: string,
    private readonly userId: string,
    private readonly cb: WsCallbacks,
  ) {}

  start(): void {
    if (!this.gameId || !this.userId) return; // nothing to subscribe to
    this.stopped = false;
    void this.connect();
  }

  stop(): void {
    this.stopped = true;
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
    if (this.ws) {
      try {
        this.ws.removeAllListeners();
        this.ws.close();
      } catch {
        /* ignore */
      }
      this.ws = null;
    }
  }

  private scheduleReconnect(): void {
    if (this.stopped || this.reconnectTimer) return;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      void this.connect();
    }, RECONNECT_BACKOFF_MS);
  }

  private async connect(): Promise<void> {
    if (this.stopped) return;
    let token: string;
    try {
      token = await this.auth.getToken();
    } catch {
      this.cb.onState(false);
      this.scheduleReconnect();
      return;
    }

    const url =
      `wss://${WS_HOST}/v1?gameId=${encodeURIComponent(this.gameId)}` +
      `&userId=${encodeURIComponent(this.userId)}&stt=${encodeURIComponent(token)}`;

    const ws = new WebSocket(url, { origin: ORIGIN });
    this.ws = ws;

    ws.on("open", () => this.cb.onState(true));
    ws.on("message", () => this.cb.onNudge());
    ws.on("close", () => {
      this.cb.onState(false);
      if (this.ws === ws) this.ws = null;
      this.scheduleReconnect();
    });
    ws.on("error", () => {
      // 'close' fires after 'error'; reconnect is scheduled there.
      this.cb.onState(false);
    });
  }
}
