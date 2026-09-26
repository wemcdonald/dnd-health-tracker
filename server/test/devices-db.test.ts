import { describe, it, expect } from "vitest";
import {
  getDevice, registerDevice, recordCheckin, updateDeviceConfig, setDeviceToken,
  forgetDevice, deleteDevice, listDevices, listDeviceWifi, upsertDeviceWifi, deleteDeviceWifi,
  countDevices,
} from "../src/db.js";

describe("devices db", () => {
  it("registers a board with rev 0 and nothing managed", () => {
    const d = registerDevice("aaaaaaaaaa01", "f".repeat(64), 1000);
    expect(d).toMatchObject({
      mac: "aaaaaaaaaa01", tokenSha256: "f".repeat(64), label: "", slug: null,
      brightness: null, pollSeconds: null, configRev: 0, firstSeen: 1000, lastSeen: 1000,
    });
    expect(getDevice("aaaaaaaaaa01")).toEqual(d);
    expect(listDevices().map((x) => x.mac)).toContain("aaaaaaaaaa01");
  });

  it("records check-ins without bumping rev", () => {
    registerDevice("aaaaaaaaaa02", "f".repeat(64), 1000);
    recordCheckin("aaaaaaaaaa02", { now: 2000, fwVersion: "3", reportedSlug: "nan", localIp: "10.0.10.93" });
    expect(getDevice("aaaaaaaaaa02")).toMatchObject({
      lastSeen: 2000, fwVersion: "3", reportedSlug: "nan", localIp: "10.0.10.93", configRev: 0,
    });
  });

  it("config and wifi edits bump rev", () => {
    registerDevice("aaaaaaaaaa03", "f".repeat(64), 1000);
    updateDeviceConfig("aaaaaaaaaa03", { label: "den", slug: "shen", brightness: 0.4, pollSeconds: null });
    expect(getDevice("aaaaaaaaaa03")).toMatchObject({ label: "den", slug: "shen", brightness: 0.4, configRev: 1 });
    upsertDeviceWifi({ mac: "aaaaaaaaaa03", ssid: "Cabin", psk: "pw", priority: 2, action: "upsert" });
    upsertDeviceWifi({ mac: "aaaaaaaaaa03", ssid: "Old", psk: "", priority: 0, action: "remove" });
    expect(listDeviceWifi("aaaaaaaaaa03")).toEqual([
      { mac: "aaaaaaaaaa03", ssid: "Cabin", psk: "pw", priority: 2, action: "upsert" },
      { mac: "aaaaaaaaaa03", ssid: "Old", psk: "", priority: 0, action: "remove" },
    ]);
    deleteDeviceWifi("aaaaaaaaaa03", "Old");
    expect(listDeviceWifi("aaaaaaaaaa03").map((w) => w.ssid)).toEqual(["Cabin"]);
    expect(getDevice("aaaaaaaaaa03")?.configRev).toBe(4);
  });

  it("forget clears the token; setDeviceToken re-registers", () => {
    registerDevice("aaaaaaaaaa04", "f".repeat(64), 1000);
    forgetDevice("aaaaaaaaaa04");
    expect(getDevice("aaaaaaaaaa04")?.tokenSha256).toBeNull();
    setDeviceToken("aaaaaaaaaa04", "e".repeat(64));
    expect(getDevice("aaaaaaaaaa04")?.tokenSha256).toBe("e".repeat(64));
  });

  it("forget drops managed wifi (secrets) but keeps the non-secret config and bumps rev", () => {
    registerDevice("aaaaaaaaaa06", "f".repeat(64), 1000);
    updateDeviceConfig("aaaaaaaaaa06", { label: "den", slug: "shen", brightness: 0.4, pollSeconds: null });
    upsertDeviceWifi({ mac: "aaaaaaaaaa06", ssid: "Cabin", psk: "pw", priority: 1, action: "upsert" });
    const revBefore = getDevice("aaaaaaaaaa06")?.configRev ?? 0;
    forgetDevice("aaaaaaaaaa06");
    expect(getDevice("aaaaaaaaaa06")?.tokenSha256).toBeNull();
    expect(listDeviceWifi("aaaaaaaaaa06")).toEqual([]);
    expect(getDevice("aaaaaaaaaa06")).toMatchObject({ label: "den", slug: "shen", brightness: 0.4 });
    expect(getDevice("aaaaaaaaaa06")?.configRev).toBe(revBefore + 1);
  });

  it("counts registered devices", () => {
    const before = countDevices();
    registerDevice("aaaaaaaaaa07", "f".repeat(64), 1000);
    expect(countDevices()).toBe(before + 1);
  });

  it("delete removes the board and its wifi rows", () => {
    registerDevice("aaaaaaaaaa05", "f".repeat(64), 1000);
    upsertDeviceWifi({ mac: "aaaaaaaaaa05", ssid: "X", psk: "p", priority: 0, action: "upsert" });
    deleteDevice("aaaaaaaaaa05");
    expect(getDevice("aaaaaaaaaa05")).toBeUndefined();
    expect(listDeviceWifi("aaaaaaaaaa05")).toEqual([]);
  });
});
