/**
 * SQLite persistence (better-sqlite3, synchronous).
 *
 * Two tables:
 *   characters(slug, character_id, user_id, game_id, enabled)
 *   settings(key, value)   — currently just the Cobalt cookie
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
