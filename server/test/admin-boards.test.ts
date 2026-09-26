import { describe, it, expect } from "vitest";
import Fastify from "fastify";
import formbody from "@fastify/formbody";
import { adminRoutes } from "../src/routes/admin.js";
import { registerDevice, getDevice, listDeviceWifi, upsertDeviceWifi, recordCheckin, recordRejectedCheckin } from "../src/db.js";

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

  it("400s a malformed mac on the admin routes", async () => {
    const a = await app();
    expect((await form(a, "/admin/devices/not-a-mac", "label=x")).statusCode).toBe(400);
  });

  it("404s on unknown mac for the forget and delete routes", async () => {
    const a = await app();
    expect((await form(a, "/admin/devices/dddddddddd03/forget", "x=1")).statusCode).toBe(404);
    expect((await form(a, "/admin/devices/dddddddddd04/delete", "x=1")).statusCode).toBe(404);
  });

  it("keeps the saved psk when a wifi edit's psk is blank, but updates priority", async () => {
    registerDevice("cccccccccc08", "f".repeat(64), Date.now());
    const a = await app();
    await form(a, "/admin/devices/cccccccccc08/wifi", "ssid=Cabin&psk=pw&priority=1&action=upsert");
    await form(a, "/admin/devices/cccccccccc08/wifi", "ssid=Cabin&psk=&priority=5&action=upsert");
    expect(listDeviceWifi("cccccccccc08")).toEqual([
      { mac: "cccccccccc08", ssid: "Cabin", psk: "pw", priority: 5, action: "upsert" },
    ]);
  });

  it("clears the saved psk when open=on even though psk is blank", async () => {
    registerDevice("cccccccccc09", "f".repeat(64), Date.now());
    const a = await app();
    await form(a, "/admin/devices/cccccccccc09/wifi", "ssid=Cabin&psk=pw&priority=1&action=upsert");
    await form(a, "/admin/devices/cccccccccc09/wifi", "ssid=Cabin&psk=&priority=1&action=upsert&open=on");
    expect(listDeviceWifi("cccccccccc09")).toEqual([
      { mac: "cccccccccc09", ssid: "Cabin", psk: "", priority: 1, action: "upsert" },
    ]);
  });

  it("a brand-new row with a blank psk and no open flag just stores an empty psk", async () => {
    registerDevice("cccccccccc10", "f".repeat(64), Date.now());
    const a = await app();
    await form(a, "/admin/devices/cccccccccc10/wifi", "ssid=Cabin&psk=&priority=0&action=upsert");
    expect(listDeviceWifi("cccccccccc10")).toEqual([
      { mac: "cccccccccc10", ssid: "Cabin", psk: "", priority: 0, action: "upsert" },
    ]);
  });

  it("drops the saved psk once a network is marked for removal", async () => {
    registerDevice("cccccccccc11", "f".repeat(64), Date.now());
    const a = await app();
    await form(a, "/admin/devices/cccccccccc11/wifi", "ssid=Cabin&psk=pw&priority=1&action=upsert");
    await form(a, "/admin/devices/cccccccccc11/wifi", "ssid=Cabin&action=remove");
    expect(listDeviceWifi("cccccccccc11")).toEqual([
      { mac: "cccccccccc11", ssid: "Cabin", psk: "", priority: 0, action: "remove" },
    ]);
  });

  it("rejects an ssid over 32 bytes, counting multibyte characters as multiple bytes", async () => {
    registerDevice("cccccccccc12", "f".repeat(64), Date.now());
    const a = await app();
    const ssid = encodeURIComponent("é".repeat(17)); // 17 chars, 2 bytes each = 34 bytes > 32
    const r = await form(a, "/admin/devices/cccccccccc12/wifi", `ssid=${ssid}&action=upsert`);
    expect(r.statusCode).toBe(400);
  });

  it("rejects a priority outside -100..100", async () => {
    registerDevice("cccccccccc13", "f".repeat(64), Date.now());
    const a = await app();
    expect(
      (await form(a, "/admin/devices/cccccccccc13/wifi", "ssid=Cabin&priority=101&action=upsert")).statusCode,
    ).toBe(400);
    expect(
      (await form(a, "/admin/devices/cccccccccc13/wifi", "ssid=Cabin&priority=-101&action=upsert")).statusCode,
    ).toBe(400);
  });

  it("escapes a hostile reported slug", async () => {
    registerDevice("cccccccccc14", "f".repeat(64), Date.now());
    recordCheckin("cccccccccc14", {
      now: Date.now(), fwVersion: "1", reportedSlug: "<script>alert(1)</script>", localIp: "1.2.3.4",
    });
    const r = await (await app()).inject({ method: "GET", url: "/" });
    expect(r.body).not.toContain("<script>alert(1)</script>");
    expect(r.body).toContain("&lt;script&gt;");
  });

  it("shows a rejected-check-in warning when it's newer than last seen", async () => {
    registerDevice("cccccccccc15", "f".repeat(64), 1000);
    recordRejectedCheckin("cccccccccc15", 2000);
    const r = await (await app()).inject({ method: "GET", url: "/" });
    expect(r.body).toContain("rejected check-in");
  });
});
