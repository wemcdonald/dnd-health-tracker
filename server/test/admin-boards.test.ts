import { describe, it, expect } from "vitest";
import Fastify from "fastify";
import formbody from "@fastify/formbody";
import { adminRoutes } from "../src/routes/admin.js";
import { registerDevice, getDevice, listDeviceWifi, upsertDeviceWifi } from "../src/db.js";

async function app() {
  const a = Fastify();
  await a.register(formbody);
  await a.register(adminRoutes);
  return a;
}
const form = (a: Awaited<ReturnType<typeof app>>, url: string, body: string) =>
  a.inject({ method: "POST", url, payload: body, headers: { "content-type": "application/x-www-form-urlencoded" } });

describe("admin boards", () => {
  it("lists boards with a formatted MAC", async () => {
    registerDevice("cccccccccc01", "f".repeat(64), Date.now());
    const r = await (await app()).inject({ method: "GET", url: "/" });
    expect(r.body).toContain("cc:cc:cc:cc:cc:01");
    expect(r.body).toContain("Boards");
  });

  it("saves managed fields (blank = unmanaged) and bumps rev", async () => {
    registerDevice("cccccccccc02", "f".repeat(64), Date.now());
    const r = await form(await app(), "/admin/devices/cccccccccc02", "label=den&slug=shen&brightness=0.4&pollSeconds=");
    expect(r.statusCode).toBe(302);
    expect(getDevice("cccccccccc02")).toMatchObject({
      label: "den", slug: "shen", brightness: 0.4, pollSeconds: null, configRev: 1,
    });
  });

  it("rejects out-of-range values", async () => {
    registerDevice("cccccccccc03", "f".repeat(64), Date.now());
    const a = await app();
    expect((await form(a, "/admin/devices/cccccccccc03", "slug=Bad Slug")).statusCode).toBe(400);
    expect((await form(a, "/admin/devices/cccccccccc03", "brightness=2")).statusCode).toBe(400);
    expect((await form(a, "/admin/devices/cccccccccc03", "pollSeconds=1")).statusCode).toBe(400);
    expect(getDevice("cccccccccc03")?.configRev).toBe(0);
  });

  it("adds and deletes wifi rows", async () => {
    registerDevice("cccccccccc04", "f".repeat(64), Date.now());
    const a = await app();
    await form(a, "/admin/devices/cccccccccc04/wifi", "ssid=Cabin&psk=pw&priority=2&action=upsert");
    await form(a, "/admin/devices/cccccccccc04/wifi", "ssid=Old&action=remove");
    expect(listDeviceWifi("cccccccccc04").map((w) => [w.ssid, w.action])).toEqual([
      ["Cabin", "upsert"], ["Old", "remove"],
    ]);
    await form(a, "/admin/devices/cccccccccc04/wifi/delete", "ssid=Old");
    expect(listDeviceWifi("cccccccccc04").map((w) => w.ssid)).toEqual(["Cabin"]);
  });

  it("never renders a psk back", async () => {
    registerDevice("cccccccccc05", "f".repeat(64), Date.now());
    upsertDeviceWifi({ mac: "cccccccccc05", ssid: "Secret", psk: "hunter2-psk", priority: 0, action: "upsert" });
    const r = await (await app()).inject({ method: "GET", url: "/" });
    expect(r.body).toContain("Secret");
    expect(r.body).not.toContain("hunter2-psk");
  });

  it("forget clears the token; delete removes the board", async () => {
    registerDevice("cccccccccc06", "f".repeat(64), Date.now());
    const a = await app();
    await form(a, "/admin/devices/cccccccccc06/forget", "x=1");
    expect(getDevice("cccccccccc06")?.tokenSha256).toBeNull();
    await form(a, "/admin/devices/cccccccccc06/delete", "x=1");
    expect(getDevice("cccccccccc06")).toBeUndefined();
  });

  it("404s on unknown mac for the wifi route", async () => {
    const a = await app();
    const r = await form(a, "/admin/devices/dddddddddd01/wifi", "ssid=Cabin&action=upsert");
    expect(r.statusCode).toBe(404);
  });

  it("404s on unknown mac for the config route", async () => {
    const a = await app();
    const r = await form(a, "/admin/devices/dddddddddd02", "label=nope");
    expect(r.statusCode).toBe(404);
  });
});
