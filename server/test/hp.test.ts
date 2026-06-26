import { describe, it, expect } from "vitest";
import { computeHp, hpFromCharacterData, litFromHp } from "../src/hp.js";

describe("computeHp", () => {
  it("base + conMod*level", () => {
    // CON 14 -> +2; level 5 -> +10; base 30 -> max 40
    expect(computeHp(30, 0, null, 0, 0, 14, 5)).toEqual({ cur: 40, max: 40, temp: 0 });
  });

  it("applies removed (clamped at 0) and temp", () => {
    expect(computeHp(30, 0, null, 100, 7, 14, 5)).toEqual({ cur: 0, max: 40, temp: 7 });
    expect(computeHp(30, 0, null, 15, 0, 14, 5)).toEqual({ cur: 25, max: 40, temp: 0 });
  });

  it("override replaces max when > 0", () => {
    expect(computeHp(30, 0, 99, 9, 0, 14, 5)).toEqual({ cur: 90, max: 99, temp: 0 });
    // override 0 is ignored (falls back to computed)
    expect(computeHp(30, 0, 0, 0, 0, 14, 5)).toEqual({ cur: 40, max: 40, temp: 0 });
  });

  it("negative CON modifier floors toward -inf, max never below 1", () => {
    // CON 1 -> floor((1-10)/2) = floor(-4.5) = -5
    expect(computeHp(2, 0, null, 0, 0, 1, 1)).toEqual({ cur: 1, max: 1, temp: 0 });
  });

  it("negative temp clamps to 0", () => {
    expect(computeHp(10, 0, null, 0, -3, 10, 1).temp).toBe(0);
  });
});

describe("hpFromCharacterData", () => {
  const base = {
    baseHitPoints: 30,
    bonusHitPoints: 0,
    overrideHitPoints: null,
    removedHitPoints: 8,
    temporaryHitPoints: 5,
    classes: [{ level: 3 }, { level: 2 }], // total level 5
    stats: [
      { id: 1, value: 16 },
      { id: 3, value: 14 }, // CON 14 -> +2
    ],
    bonusStats: [{ id: 3, value: null }],
    overrideStats: [{ id: 3, value: null }],
    modifiers: { race: [], class: [], feat: [] },
  };

  it("computes from a realistic-shaped document", () => {
    // max = 30 + 0 + 2*5 = 40; cur = 40 - 8 = 32; temp 5
    expect(hpFromCharacterData(base)).toEqual({ cur: 32, max: 40, temp: 5 });
  });

  it("overrideStats replaces CON", () => {
    const d = { ...base, overrideStats: [{ id: 3, value: 20 }] }; // CON 20 -> +5
    // max = 30 + 5*5 = 55
    expect(hpFromCharacterData(d).max).toBe(55);
  });

  it("bonusStats adds to CON", () => {
    const d = { ...base, bonusStats: [{ id: 3, value: 2 }] }; // 14+2=16 -> +3
    expect(hpFromCharacterData(d).max).toBe(30 + 3 * 5);
  });

  it("constitution-score bonus modifiers add", () => {
    const d = {
      ...base,
      modifiers: {
        feat: [{ type: "bonus", subType: "constitution-score", value: 2 }], // 14+2=16 -> +3
        item: [{ type: "bonus", subType: "armor-class", value: 99 }], // ignored
      },
    };
    expect(hpFromCharacterData(d).max).toBe(30 + 3 * 5);
  });

  it("throws on a private/empty document", () => {
    expect(() => hpFromCharacterData({})).toThrow();
  });
});

describe("litFromHp", () => {
  it("maps full/empty/partial onto 16 LEDs", () => {
    expect(litFromHp(40, 40)).toBe(16);
    expect(litFromHp(0, 40)).toBe(0);
    expect(litFromHp(20, 40)).toBe(8);
  });

  it("a living character shows at least 1 LED", () => {
    expect(litFromHp(1, 45)).toBe(1); // 16*1/45 = 0.35 -> would round to 0
  });

  it("clamps and handles zero max", () => {
    expect(litFromHp(100, 40)).toBe(16);
    expect(litFromHp(5, 0)).toBe(0);
  });
});
