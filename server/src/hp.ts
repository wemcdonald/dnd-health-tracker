/**
 * D&D Beyond HP computation.
 *
 * A faithful port of the firmware's `hp.py` (`compute_hp` + the CON / level
 * resolution in `_Scanner.finalize`), which itself ports `internal/ddb/hp.go`.
 * The device version had to stream-scan a 50-100 KB document on a 520 KB MCU; on
 * the server we have RAM to spare, so we `JSON.parse` the whole body and walk the
 * object. The *formula* is identical.
 *
 *   max     = overrideHitPoints, if set and > 0; otherwise
 *             baseHitPoints + bonusHitPoints + conModifier * totalLevel  (min 1)
 *   current = clamp(max - removedHitPoints, 0, max)
 *   temp    = max(0, temporaryHitPoints)
 *
 * conModifier = floor((conScore - 10) / 2), where conScore resolves the base
 * stat, an override (replaces), a bonus (adds) and `constitution-score` bonus
 * modifiers (add).
 */

export const CON_STAT_ID = 3; // D&D Beyond ability-score id for Constitution

export interface Hp {
  cur: number;
  max: number;
  temp: number;
}

/** Floor-toward-negative-infinity division, matching Python's `//`. */
function floorDiv(a: number, b: number): number {
  return Math.floor(a / b);
}

/** Pure HP formula, mirroring `compute_hp` in hp.py. */
export function computeHp(
  base: number,
  bonus: number,
  override: number | null,
  removed: number,
  temp: number,
  conScore: number,
  level: number,
): Hp {
  const conMod = floorDiv(conScore - 10, 2);
  let max = override !== null && override > 0 ? override : base + bonus + conMod * level;
  if (max < 1) max = 1; // a living character has >= 1 max HP
  let cur = max - removed;
  if (cur < 0) cur = 0;
  else if (cur > max) cur = max;
  if (temp < 0) temp = 0;
  return { cur, max, temp };
}

interface StatEntry {
  id?: number;
  value?: number | null;
}
interface ModifierEntry {
  type?: string;
  subType?: string;
  value?: number | null;
}

/** Return the `value` of the Constitution entry in a DDB stat array, or null. */
function conFromStats(stats: unknown): number | null {
  if (!Array.isArray(stats)) return null;
  for (const s of stats as StatEntry[]) {
    if (s && s.id === CON_STAT_ID && typeof s.value === "number") return s.value;
  }
  return null;
}

function num(v: unknown): number {
  return typeof v === "number" && Number.isFinite(v) ? v : 0;
}

/**
 * Compute HP from a parsed DDB v5 character document (the object that lives
 * under the top-level `data` key of the character-service response).
 * Throws if the document has no HP fields (private sheet / unexpected shape).
 */
export function hpFromCharacterData(data: Record<string, unknown>): Hp {
  if (data == null || typeof data !== "object") {
    throw new Error("ddb: character data is null (private sheet or bad response?)");
  }

  const hasAnyHp = [
    "baseHitPoints",
    "bonusHitPoints",
    "overrideHitPoints",
    "removedHitPoints",
    "temporaryHitPoints",
  ].some((k) => k in data);
  if (!hasAnyHp) {
    throw new Error("ddb: no HP fields found (private sheet or bad response?)");
  }

  // Total level = sum of class levels.
  let level = 0;
  const classes = data["classes"];
  if (Array.isArray(classes)) {
    for (const c of classes) {
      if (c && typeof c === "object") level += num((c as Record<string, unknown>)["level"]);
    }
  }

  // CON resolution: base stat, then override replaces, bonus adds, then
  // `constitution-score` bonus modifiers add.
  let con = 10;
  const statCon = conFromStats(data["stats"]);
  if (statCon !== null) con = statCon;
  const overrideCon = conFromStats(data["overrideStats"]);
  if (overrideCon !== null) con = overrideCon;
  const bonusCon = conFromStats(data["bonusStats"]);
  if (bonusCon !== null) con += bonusCon;

  const modifiers = data["modifiers"];
  if (modifiers && typeof modifiers === "object") {
    for (const category of Object.values(modifiers as Record<string, unknown>)) {
      if (!Array.isArray(category)) continue;
      for (const m of category as ModifierEntry[]) {
        if (m && m.type === "bonus" && m.subType === "constitution-score" && typeof m.value === "number") {
          con += m.value;
        }
      }
    }
  }

  const override = typeof data["overrideHitPoints"] === "number" ? (data["overrideHitPoints"] as number) : null;
  return computeHp(
    num(data["baseHitPoints"]),
    num(data["bonusHitPoints"]),
    override,
    num(data["removedHitPoints"]),
    num(data["temporaryHitPoints"]),
    con,
    level,
  );
}

export const NUM_LEDS = 16;

/**
 * Map HP to the number of LEDs to light (0..NUM_LEDS). The device does no HP
 * math — it just lights `lit` LEDs — so this is where the bar resolution lives.
 * A living character (cur > 0) always shows at least 1 LED so "barely alive"
 * never reads as "dead/0 LEDs". cur == 0 -> 0 LEDs.
 */
export function litFromHp(cur: number, max: number, numLeds = NUM_LEDS): number {
  if (max <= 0 || cur <= 0) return 0;
  let lit = Math.round((numLeds * cur) / max);
  if (lit < 1) lit = 1;
  if (lit > numLeds) lit = numLeds;
  return lit;
}
