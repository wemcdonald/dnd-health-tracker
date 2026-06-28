import { afterEach, describe, expect, it, vi } from "vitest";
import { fetchHp } from "../src/ddb.js";

// A minimal v5 character document with just the fields hpFromCharacterData reads.
const doc = {
  data: {
    baseHitPoints: 26,
    removedHitPoints: 4,
    temporaryHitPoints: 0,
    bonusHitPoints: null,
    overrideHitPoints: null,
    classes: [],
    modifiers: { race: [], class: [], background: [], item: [], feat: [], condition: [] },
    stats: [],
    bonusStats: [],
    overrideStats: [],
  },
};

function mockFetchOk() {
  const spy = vi.fn(async () => new Response(JSON.stringify(doc), { status: 200 }));
  vi.stubGlobal("fetch", spy);
  return spy;
}

afterEach(() => vi.unstubAllGlobals());

describe("fetchHp auth header", () => {
  it("sends no Authorization header for a public fetch", async () => {
    const spy = mockFetchOk();
    await fetchHp("123");
    const headers = (spy.mock.calls[0]![1] as RequestInit).headers as Record<string, string>;
    expect(headers["Authorization"]).toBeUndefined();
  });

  it("sends `Bearer <token>` when a token is supplied (private sheet)", async () => {
    const spy = mockFetchOk();
    await fetchHp("123", { token: "abc.def" });
    const headers = (spy.mock.calls[0]![1] as RequestInit).headers as Record<string, string>;
    expect(headers["Authorization"]).toBe("Bearer abc.def");
  });
});
