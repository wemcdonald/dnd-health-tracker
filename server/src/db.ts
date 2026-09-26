/**
 * SQLite persistence (better-sqlite3, synchronous).
 *
 * Tables:
 *   characters(slug, character_id, user_id, game_id, enabled)
 *   settings(key, value)   — currently just the Cobalt cookie
 *   devices(mac, token_sha256, label, slug, brightness, poll_seconds, config_rev,
 *           first_seen, last_seen, fw_version, reported_slug, local_ip, last_rejected)
 *   device_wifi(mac, ssid, psk, priority, action)   — per-board pushed WiFi list
 *
 * The DB file lives at $DB_PATH (default ./data/tracker.db) so it can sit on a
 * Docker volume. This module is the only place that touches the DB.
 */

import Database from "better-sqlite3";
import { mkdirSync } from "node:fs";
import { dirname } from "node:path";

export interface Character {
  slug: string;
  characterId: string;
  userId: string;
  gameId: string;
  enabled: boolean;
}

const DB_PATH = process.env["DB_PATH"] ?? "./data/tracker.db";

mkdirSync(dirname(DB_PATH), { recursive: true });

const db = new Database(DB_PATH);
db.pragma("journal_mode = WAL");

db.exec(`
  CREATE TABLE IF NOT EXISTS characters (
    slug         TEXT PRIMARY KEY,
    character_id TEXT NOT NULL,
    user_id      TEXT NOT NULL DEFAULT '',
    game_id      TEXT NOT NULL DEFAULT '',
    enabled      INTEGER NOT NULL DEFAULT 1
  );
  CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
  );
  CREATE TABLE IF NOT EXISTS devices (
    mac           TEXT PRIMARY KEY,
    token_sha256  TEXT,
    label         TEXT NOT NULL DEFAULT '',
    slug          TEXT,
    brightness    REAL,
    poll_seconds  REAL,
    config_rev    INTEGER NOT NULL DEFAULT 0,
    first_seen    INTEGER NOT NULL,
    last_seen     INTEGER NOT NULL,
    fw_version    TEXT NOT NULL DEFAULT '',
    reported_slug TEXT NOT NULL DEFAULT '',
    local_ip      TEXT NOT NULL DEFAULT '',
    last_rejected INTEGER
  );
  CREATE TABLE IF NOT EXISTS device_wifi (
    mac      TEXT NOT NULL,
    ssid     TEXT NOT NULL,
    psk      TEXT NOT NULL DEFAULT '',
    priority INTEGER NOT NULL DEFAULT 0,
    action   TEXT NOT NULL CHECK (action IN ('upsert', 'remove')),
    PRIMARY KEY (mac, ssid)
  );
`);

interface CharacterRow {
  slug: string;
  character_id: string;
  user_id: string;
  game_id: string;
  enabled: number;
}

function rowToCharacter(r: CharacterRow): Character {
  return {
    slug: r.slug,
    characterId: r.character_id,
    userId: r.user_id,
    gameId: r.game_id,
    enabled: r.enabled !== 0,
  };
}

const stmtAll = db.prepare<[], CharacterRow>("SELECT * FROM characters ORDER BY slug");
const stmtGet = db.prepare<[string], CharacterRow>("SELECT * FROM characters WHERE slug = ?");
const stmtUpsert = db.prepare<[string, string, string, string, number]>(`
  INSERT INTO characters (slug, character_id, user_id, game_id, enabled)
  VALUES (?, ?, ?, ?, ?)
  ON CONFLICT(slug) DO UPDATE SET
    character_id = excluded.character_id,
    user_id      = excluded.user_id,
    game_id      = excluded.game_id,
    enabled      = excluded.enabled
`);
const stmtDelete = db.prepare<[string]>("DELETE FROM characters WHERE slug = ?");

export function listCharacters(): Character[] {
  return stmtAll.all().map(rowToCharacter);
}

export function getCharacter(slug: string): Character | undefined {
  const r = stmtGet.get(slug);
  return r ? rowToCharacter(r) : undefined;
}

export function upsertCharacter(c: Character): void {
  stmtUpsert.run(c.slug, c.characterId, c.userId, c.gameId, c.enabled ? 1 : 0);
}

export function deleteCharacter(slug: string): void {
  stmtDelete.run(slug);
}

const stmtGetSetting = db.prepare<[string], { value: string }>("SELECT value FROM settings WHERE key = ?");
const stmtSetSetting = db.prepare<[string, string]>(
  "INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value",
);

export function getSetting(key: string): string | undefined {
  return stmtGetSetting.get(key)?.value;
}

export function setSetting(key: string, value: string): void {
  stmtSetSetting.run(key, value);
}

export const COBALT_COOKIE_KEY = "cobalt_cookie";

// ----- boards (server-pushed per-device config) ---------------------------

/** A registered board. Nullable config fields mean "not managed from the server". */
export interface Device {
  mac: string;               // 12 lowercase hex, no separators
  tokenSha256: string | null; // null after "forget": next check-in re-registers
  label: string;
  slug: string | null;
  brightness: number | null;
  pollSeconds: number | null;
  configRev: number;
  firstSeen: number;
  lastSeen: number;
  fwVersion: string;
  reportedSlug: string;
  localIp: string;
  lastRejected: number | null; // last token-mismatch (403) check-in; probably a re-flashed board
}

export interface DeviceWifi {
  mac: string;
  ssid: string;
  psk: string;
  priority: number;
  action: "upsert" | "remove";
}

export interface DeviceConfigEdit {
  label: string;
  slug: string | null;
  brightness: number | null;
  pollSeconds: number | null;
}

export interface DeviceCheckin {
  now: number;
  fwVersion: string;
  reportedSlug: string;
  localIp: string;
}

interface DeviceRow {
  mac: string;
  token_sha256: string | null;
  label: string;
  slug: string | null;
  brightness: number | null;
  poll_seconds: number | null;
  config_rev: number;
  first_seen: number;
  last_seen: number;
  fw_version: string;
  reported_slug: string;
  local_ip: string;
  last_rejected: number | null;
}

interface DeviceWifiRow {
  mac: string;
  ssid: string;
  psk: string;
  priority: number;
  action: string;
}

function rowToDevice(r: DeviceRow): Device {
  return {
    mac: r.mac,
    tokenSha256: r.token_sha256,
    label: r.label,
    slug: r.slug,
    brightness: r.brightness,
    pollSeconds: r.poll_seconds,
    configRev: r.config_rev,
    firstSeen: r.first_seen,
    lastSeen: r.last_seen,
    fwVersion: r.fw_version,
    reportedSlug: r.reported_slug,
    localIp: r.local_ip,
    lastRejected: r.last_rejected,
  };
}

function rowToWifi(r: DeviceWifiRow): DeviceWifi {
  if (r.action !== "upsert" && r.action !== "remove") {
    throw new Error(`unexpected device_wifi.action ${JSON.stringify(r.action)} for ${r.mac}/${r.ssid}`);
  }
  return { mac: r.mac, ssid: r.ssid, psk: r.psk, priority: r.priority, action: r.action };
}

const stmtDevAll = db.prepare<[], DeviceRow>("SELECT * FROM devices ORDER BY label, mac");
const stmtDevGet = db.prepare<[string], DeviceRow>("SELECT * FROM devices WHERE mac = ?");
const stmtDevInsert = db.prepare<[string, string, number, number]>(
  "INSERT INTO devices (mac, token_sha256, first_seen, last_seen) VALUES (?, ?, ?, ?)",
);
const stmtDevToken = db.prepare<[string | null, string]>("UPDATE devices SET token_sha256 = ? WHERE mac = ?");
const stmtDevCheckin = db.prepare<[number, string, string, string, string]>(
  "UPDATE devices SET last_seen = ?, fw_version = ?, reported_slug = ?, local_ip = ? WHERE mac = ?",
);
const stmtDevRejected = db.prepare<[number, string]>("UPDATE devices SET last_rejected = ? WHERE mac = ?");
const stmtDevConfig = db.prepare<[string, string | null, number | null, number | null, string]>(
  "UPDATE devices SET label = ?, slug = ?, brightness = ?, poll_seconds = ?, config_rev = config_rev + 1 WHERE mac = ?",
);
const stmtDevBump = db.prepare<[string]>("UPDATE devices SET config_rev = config_rev + 1 WHERE mac = ?");
const stmtDevDelete = db.prepare<[string]>("DELETE FROM devices WHERE mac = ?");
const stmtDevCount = db.prepare<[], { n: number }>("SELECT COUNT(*) AS n FROM devices");
const stmtWifiList = db.prepare<[string], DeviceWifiRow>(
  "SELECT mac, ssid, psk, priority, action FROM device_wifi WHERE mac = ? ORDER BY priority DESC, ssid",
);
const stmtWifiUpsert = db.prepare<{
  mac: string;
  ssid: string;
  psk: string;
  priority: number;
  action: string;
  open: number;
}>(`
  INSERT INTO device_wifi (mac, ssid, psk, priority, action) VALUES (@mac, @ssid, @psk, @priority, @action)
  ON CONFLICT(mac, ssid) DO UPDATE SET
    psk = CASE
      WHEN excluded.action = 'remove' THEN ''
      WHEN excluded.psk = '' AND @open = 0 THEN device_wifi.psk
      ELSE excluded.psk
    END,
    priority = excluded.priority,
    action = excluded.action
`);
const stmtWifiDelete = db.prepare<[string, string]>("DELETE FROM device_wifi WHERE mac = ? AND ssid = ?");
const stmtWifiDeleteAll = db.prepare<[string]>("DELETE FROM device_wifi WHERE mac = ?");

export function listDevices(): Device[] {
  return stmtDevAll.all().map(rowToDevice);
}

export function getDevice(mac: string): Device | undefined {
  const r = stmtDevGet.get(mac);
  return r ? rowToDevice(r) : undefined;
}

/** First contact from a board: store its token hash (self-registration). */
export function registerDevice(mac: string, tokenSha256: string, now: number): Device {
  stmtDevInsert.run(mac, tokenSha256, now, now);
  const r = stmtDevGet.get(mac);
  if (!r) throw new Error(`registerDevice: insert of ${mac} did not persist`);
  return rowToDevice(r);
}

export function setDeviceToken(mac: string, tokenSha256: string): void {
  stmtDevToken.run(tokenSha256, mac);
}

/**
 * "Forget": the next check-in from this MAC re-registers with its new token.
 * Also drops the board's managed WiFi rows (SSIDs + plaintext PSKs) so a
 * forgotten board's secrets aren't handed to whoever re-registers next —
 * label/slug/brightness/pollSeconds aren't secret, so those are kept.
 */
export function forgetDevice(mac: string): void {
  db.transaction(() => {
    stmtDevToken.run(null, mac);
    stmtWifiDeleteAll.run(mac);
    stmtDevBump.run(mac);
  })();
}

/** Total registered boards, used to cap public self-registration (see routes/device.ts). */
export function countDevices(): number {
  const row = stmtDevCount.get();
  if (!row) throw new Error("countDevices: COUNT query returned no rows");
  return row.n;
}

export function recordCheckin(mac: string, c: DeviceCheckin): void {
  stmtDevCheckin.run(c.now, c.fwVersion, c.reportedSlug, c.localIp, mac);
}

/** Record a token-mismatch (403) check-in, so the admin page can flag a probably re-flashed board. */
export function recordRejectedCheckin(mac: string, now: number): void {
  stmtDevRejected.run(now, mac);
}

export function updateDeviceConfig(mac: string, e: DeviceConfigEdit): void {
  stmtDevConfig.run(e.label, e.slug, e.brightness, e.pollSeconds, mac);
}

export function listDeviceWifi(mac: string): DeviceWifi[] {
  return stmtWifiList.all(mac).map(rowToWifi);
}

/**
 * Upsert a managed WiFi row. A blank `psk` on an existing row is treated as
 * "leave the saved password alone" (so re-saving priority doesn't wipe it) —
 * pass `{ open: true }` to explicitly store an empty password (open network).
 * Marking a row for removal always drops the stored password.
 */
export function upsertDeviceWifi(w: DeviceWifi, opts: { open?: boolean } = {}): void {
  const open = opts.open ?? false;
  db.transaction(() => {
    stmtWifiUpsert.run({ mac: w.mac, ssid: w.ssid, psk: w.psk, priority: w.priority, action: w.action, open: open ? 1 : 0 });
    stmtDevBump.run(w.mac);
  })();
}

export function deleteDeviceWifi(mac: string, ssid: string): void {
  db.transaction(() => {
    stmtWifiDelete.run(mac, ssid);
    stmtDevBump.run(mac);
  })();
}

export function deleteDevice(mac: string): void {
  db.transaction(() => {
    stmtWifiDeleteAll.run(mac);
    stmtDevDelete.run(mac);
  })();
}
