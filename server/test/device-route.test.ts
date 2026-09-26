import { describe, it, expect } from "vitest";
import Fastify from "fastify";
import { deviceRoutes } from "../src/routes/device.js";
import { getDevice, updateDeviceConfig, upsertDeviceWifi, forgetDevice, countDevices } from "../src/db.js";

const TOKEN = "a".repeat(32);

async function app(opts: { maxDevices?: number } = {}) {
  const a = Fastify();
  await a.register(deviceRoutes, opts);
  return a;
}

function get(a: Awaited<ReturnType<typeof app>>, mac: string, headers: Record<string, string>) {
  return a.inject({ method: "GET", url: `/device/${mac}/config`, headers });
}

const hdrs = (rev: number, token = TOKEN) => ({
  "x-device-token": token, "x-config-rev": String(rev), "x-firmware-version": "2",
  "x-slug": "nan", "x-local-ip": "10.0.10.93",
});

// Same headers, but with no X-Config-Rev at all (as opposed to rev "0").
const hdrsNoRev = (token = TOKEN) => ({
  "x-device-token": token, "x-firmware-version": "2", "x-slug": "nan", "x-local-ip": "10.0.10.93",
});

describe("GET /device/:mac/config", () => {
  it("rejects malformed mac or token", async () => {
    const a = await app();
    expect((await get(a, "not-a-mac", hdrs(0))).statusCode).toBe(400);
    expect((await get(a, "bbbbbbbbbb01", hdrs(0, "short"))).statusCode).toBe(400);
  });

  it("self-registers on first contact and records the check-in", async () => {
    const a = await app();
    const r = await get(a, "bbbbbbbbbb02", hdrs(0));
    expect(r.statusCode).toBe(304);
    expect(getDevice("bbbbbbbbbb02")).toMatchObject({
      configRev: 0, fwVersion: "2", reportedSlug: "nan", localIp: "10.0.10.93",
    });
    expect(getDevice("bbbbbbbbbb02")?.tokenSha256).not.toContain(TOKEN); // stored hashed
  });

  it("403s a different token for a registered board", async () => {
    const a = await app();
    await get(a, "bbbbbbbbbb03", hdrs(0));
    expect((await get(a, "bbbbbbbbbb03", hdrs(0, "b".repeat(32)))).statusCode).toBe(403);
  });

  it("returns the payload when the board's rev is behind, 304 once caught up", async () => {
    const a = await app();
    await get(a, "bbbbbbbbbb04", hdrs(0));
    updateDeviceConfig("bbbbbbbbbb04", { label: "", slug: "shen", brightness: null, pollSeconds: null });
    upsertDeviceWifi({ mac: "bbbbbbbbbb04", ssid: "Cabin", psk: "pw", priority: 1, action: "upsert" });
    const r = await get(a, "bbbbbbbbbb04", hdrs(0));
    expect(r.statusCode).toBe(200);
    expect(r.json()).toEqual({
      rev: 2, slug: "shen", wifi: { upsert: [{ ssid: "Cabin", psk: "pw", priority: 1 }], remove: [] },
    });
    expect((await get(a, "bbbbbbbbbb04", hdrs(2))).statusCode).toBe(304);
  });

  it("after forget, the next token re-registers and gets the full config with no wifi upserts", async () => {
    const a = await app();
    await get(a, "bbbbbbbbbb05", hdrs(0));
    updateDeviceConfig("bbbbbbbbbb05", { label: "", slug: "shen", brightness: null, pollSeconds: null });
    upsertDeviceWifi({ mac: "bbbbbbbbbb05", ssid: "Cabin", psk: "pw", priority: 1, action: "upsert" });
    forgetDevice("bbbbbbbbbb05");
    const r = await get(a, "bbbbbbbbbb05", hdrs(0, "c".repeat(32)));
    expect(r.statusCode).toBe(200);
    expect(r.json()).toMatchObject({ slug: "shen", wifi: { upsert: [], remove: [] } });
    expect((await get(a, "bbbbbbbbbb05", hdrs(1, TOKEN))).statusCode).toBe(403);
  });

  it("returns Cache-Control: no-store on a 200", async () => {
    const a = await app();
    await get(a, "bbbbbbbbbb06", hdrs(0));
    updateDeviceConfig("bbbbbbbbbb06", { label: "", slug: "shen", brightness: null, pollSeconds: null });
    const r = await get(a, "bbbbbbbbbb06", hdrs(0));
    expect(r.statusCode).toBe(200);
    expect(r.headers["cache-control"]).toBe("no-store");
  });

  it("lowercases an uppercase mac in the URL and registers under the lowercase key", async () => {
    const a = await app();
    const r = await get(a, "BBBBBBBBBB07", hdrs(0));
    expect(r.statusCode).toBe(304);
    expect(getDevice("bbbbbbbbbb07")).toMatchObject({ configRev: 0, fwVersion: "2" });
  });

  it("sends the payload (no 304) when X-Config-Rev is missing entirely", async () => {
    const a = await app();
    const r = await get(a, "bbbbbbbbbb08", hdrsNoRev());
    expect(r.statusCode).toBe(200);
    expect(r.json()).toMatchObject({ rev: 0 });
  });

  it("truncates over-long check-in headers to 64 chars", async () => {
    const a = await app();
    const long = "x".repeat(100);
    await get(a, "bbbbbbbbbb09", { ...hdrs(0), "x-firmware-version": long, "x-slug": long, "x-local-ip": long });
    const d = getDevice("bbbbbbbbbb09");
    expect(d?.fwVersion).toBe(long.slice(0, 64));
    expect(d?.fwVersion.length).toBe(64);
    expect(d?.reportedSlug).toBe(long.slice(0, 64));
    expect(d?.localIp).toBe(long.slice(0, 64));
  });

  it("caps new registrations at maxDevices but not known boards or re-registration after forget", async () => {
    // Other tests in this file register boards against the same in-memory db,
    // so the cap has to be set relative to what's already there, not a bare 2.
    const cap = countDevices() + 2;
    const a = await app({ maxDevices: cap });
    expect((await get(a, "cccccccccc07", hdrs(0))).statusCode).toBe(304);
    expect((await get(a, "cccccccccc08", hdrs(0))).statusCode).toBe(304);
    const r = await get(a, "cccccccccc09", hdrs(0));
    expect(r.statusCode).toBe(429);
    expect(r.body).toBe("device limit reached\n");
    expect(getDevice("cccccccccc09")).toBeUndefined();

    // A board already known (not a new registration) still works at the cap.
    expect((await get(a, "cccccccccc07", hdrs(0))).statusCode).toBe(304);

    // A forgotten board re-registering is not a *new* registration either
    // (forget also bumps config_rev, so this now gets a fresh 200, not a 304).
    forgetDevice("cccccccccc07");
    expect((await get(a, "cccccccccc07", hdrs(0, "d".repeat(32)))).statusCode).toBe(200);
  });
});
