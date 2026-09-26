import { describe, it, expect } from "vitest";
import { hashToken, tokenMatches, formatMac, buildConfigPayload, MAC_RE, TOKEN_RE, SLUG_RE } from "../src/devices.js";
import type { Device, DeviceWifi } from "../src/db.js";

const dev: Device = {
  mac: "ac276e7d1774", tokenSha256: null, label: "", slug: null, brightness: null,
  pollSeconds: null, configRev: 7, firstSeen: 0, lastSeen: 0, fwVersion: "", reportedSlug: "", localIp: "",
};

describe("devices helpers", () => {
  it("validates mac and token shapes", () => {
    expect(MAC_RE.test("ac276e7d1774")).toBe(true);
    expect(MAC_RE.test("ac:27:6e:7d:17:74")).toBe(false);
    expect(TOKEN_RE.test("a".repeat(32))).toBe(true);
    expect(TOKEN_RE.test("A".repeat(32))).toBe(false);
  });

  it("caps slugs at 64 chars (matches the firmware's limit)", () => {
    expect(SLUG_RE.test("thorin")).toBe(true);
    expect(SLUG_RE.test("a".repeat(64))).toBe(true);
    expect(SLUG_RE.test("a".repeat(65))).toBe(false);
    expect(SLUG_RE.test("")).toBe(false);
    expect(SLUG_RE.test("Bad Slug")).toBe(false);
  });

  it("hashes tokens and compares against the stored hash", () => {
    const h = hashToken("a".repeat(32));
    expect(h).toMatch(/^[0-9a-f]{64}$/);
    expect(tokenMatches("a".repeat(32), h)).toBe(true);
    expect(tokenMatches("b".repeat(32), h)).toBe(false);
  });

  it("formats a mac for display", () => {
    expect(formatMac("ac276e7d1774")).toBe("ac:27:6e:7d:17:74");
  });

  it("omits unmanaged fields from the payload", () => {
    expect(buildConfigPayload(dev, [])).toEqual({ rev: 7, wifi: { upsert: [], remove: [] } });
  });

  it("includes managed fields and splits wifi by action", () => {
    const wifi: DeviceWifi[] = [
      { mac: dev.mac, ssid: "Cabin", psk: "pw", priority: 2, action: "upsert" },
      { mac: dev.mac, ssid: "Old", psk: "", priority: 0, action: "remove" },
    ];
    expect(buildConfigPayload({ ...dev, slug: "shen", brightness: 0.4, pollSeconds: 5 }, wifi)).toEqual({
      rev: 7, slug: "shen", brightness: 0.4, poll_seconds: 5,
      wifi: { upsert: [{ ssid: "Cabin", psk: "pw", priority: 2 }], remove: ["Old"] },
    });
  });
});
