import { describe, it, expect } from "vitest";
import { formatDeviceFile } from "../src/routes/dnd.js";
import type { LiveState } from "../src/manager.js";

const base: LiveState = {
  slug: "shen",
  lit: 0,
  cur: 25,
  max: 40,
  temp: 3,
  pct: 63,
  online: true,
  wsConnected: true,
  lastUpstreamSuccessMs: 1000,
  lastError: null,
};

describe("formatDeviceFile (device wire format)", () => {
  it("line 1 is `<cur> <max> <temp> <age>`", () => {
    const body = formatDeviceFile(base, 1000 + 7000); // 7s after last success
    const [line1] = body.split("\n");
    expect(line1).toBe("25 40 3 7");
  });

  it("line 2 stays the human string", () => {
    const body = formatDeviceFile(base, 1000);
    const [, line2] = body.split("\n");
    expect(line2).toBe("HP 25/40 (+3 temp) · 63%");
  });

  it("never-refreshed state emits the sentinel line", () => {
    const body = formatDeviceFile({ ...base, lastUpstreamSuccessMs: 0 }, 999999);
    expect(body.split("\n")[0]).toBe("0 0 0 99999");
  });

  it("undefined state emits the sentinel line", () => {
    const body = formatDeviceFile(undefined, 999999);
    expect(body.split("\n")[0]).toBe("0 0 0 99999");
  });
});
