import { describe, it, expect } from "vitest";
import Fastify from "fastify";
import { deviceRoutes } from "../src/routes/device.js";
import { getDevice, updateDeviceConfig, upsertDeviceWifi, forgetDevice } from "../src/db.js";

const TOKEN = "a".repeat(32);

async function app() {
  const a = Fastify();
  await a.register(deviceRoutes);
  return a;
}

function get(a: Awaited<ReturnType<typeof app>>, mac: string, headers: Record<string, string>) {
  return a.inject({ method: "GET", url: `/device/${mac}/config`, headers });
}

const hdrs = (rev: number, token = TOKEN) => ({
  "x-device-token": token, "x-config-rev": String(rev), "x-firmware-version": "2",
  "x-slug": "nan", "x-local-ip": "10.0.10.93",
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

  it("after forget, the next token re-registers and gets the full config", async () => {
    const a = await app();
    await get(a, "bbbbbbbbbb05", hdrs(0));
    updateDeviceConfig("bbbbbbbbbb05", { label: "", slug: "shen", brightness: null, pollSeconds: null });
    forgetDevice("bbbbbbbbbb05");
    const r = await get(a, "bbbbbbbbbb05", hdrs(0, "c".repeat(32)));
    expect(r.statusCode).toBe(200);
    expect((await get(a, "bbbbbbbbbb05", hdrs(1, TOKEN))).statusCode).toBe(403);
  });
});
