import { describe, it, expect } from "vitest";
import { mkdtempSync, mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { readFirmwareManifest, firmwareManifestText } from "../src/firmware.js";
import Fastify from "fastify";
import { firmwareRoutes } from "../src/routes/firmware.js";

function dirWith(manifest: string | null): string {
  const d = mkdtempSync(join(tmpdir(), "fw-"));
  if (manifest !== null) writeFileSync(join(d, "manifest.txt"), manifest);
  return d;
}

describe("readFirmwareManifest", () => {
  it("parses a well-formed manifest", () => {
    const d = dirWith("42 418234\n" + "a".repeat(64) + "\n/firmware/image.bin\n");
    expect(readFirmwareManifest(d)).toEqual({
      version: 42, size: 418234, sha256: "a".repeat(64), imagePath: "/firmware/image.bin",
    });
  });
  it("returns null when manifest is absent", () => {
    expect(readFirmwareManifest(dirWith(null))).toBeNull();
  });
  it("returns null on a malformed sha256 (wrong length)", () => {
    const d = dirWith("42 418234\nabc\n/firmware/image.bin\n");
    expect(readFirmwareManifest(d)).toBeNull();
  });
  it("accepts version 0", () => {
    const d = dirWith("0 418234\n" + "a".repeat(64) + "\n/firmware/image.bin\n");
    expect(readFirmwareManifest(d)).toEqual({
      version: 0, size: 418234, sha256: "a".repeat(64), imagePath: "/firmware/image.bin",
    });
  });
  it("returns null on size 0", () => {
    const d = dirWith("42 0\n" + "a".repeat(64) + "\n/firmware/image.bin\n");
    expect(readFirmwareManifest(d)).toBeNull();
  });
  it("returns null on a negative size", () => {
    const d = dirWith("42 -1\n" + "a".repeat(64) + "\n/firmware/image.bin\n");
    expect(readFirmwareManifest(d)).toBeNull();
  });
  it("returns null on a negative version", () => {
    const d = dirWith("-1 418234\n" + "a".repeat(64) + "\n/firmware/image.bin\n");
    expect(readFirmwareManifest(d)).toBeNull();
  });
  it("returns null on a non-integer version", () => {
    const d = dirWith("1.5 418234\n" + "a".repeat(64) + "\n/firmware/image.bin\n");
    expect(readFirmwareManifest(d)).toBeNull();
  });
  it("returns null on a non-integer size", () => {
    const d = dirWith("42 4182.34\n" + "a".repeat(64) + "\n/firmware/image.bin\n");
    expect(readFirmwareManifest(d)).toBeNull();
  });
  it("returns null when imagePath lacks a leading slash", () => {
    const d = dirWith("42 418234\n" + "a".repeat(64) + "\nfirmware/image.bin\n");
    expect(readFirmwareManifest(d)).toBeNull();
  });
  it("returns null on a sha256 with invalid hex chars (correct length)", () => {
    const d = dirWith("42 418234\n" + "g".repeat(64) + "\n/firmware/image.bin\n");
    expect(readFirmwareManifest(d)).toBeNull();
  });
  it("returns null when line 0 has only one token", () => {
    const d = dirWith("42\n" + "a".repeat(64) + "\n/firmware/image.bin\n");
    expect(readFirmwareManifest(d)).toBeNull();
  });
});

describe("firmwareManifestText", () => {
  it("round-trips the wire format", () => {
    const m = { version: 42, size: 418234, sha256: "b".repeat(64), imagePath: "/firmware/image.bin" };
    expect(firmwareManifestText(m)).toBe("42 418234\n" + "b".repeat(64) + "\n/firmware/image.bin\n");
  });
});

function appWithImage(board = "esp32"): { app: ReturnType<typeof Fastify>; dir: string } {
  const dir = mkdtempSync(join(tmpdir(), "fwsrv-"));
  const bdir = join(dir, board);
  mkdirSync(bdir);
  const body = Buffer.from("HELLO-FIRMWARE-IMAGE-BYTES");
  writeFileSync(join(bdir, "image.bin"), body);
  writeFileSync(
    join(bdir, "manifest.txt"),
    `7 ${body.length}\n` + "c".repeat(64) + `\n/firmware/${board}/image.bin\n`,
  );
  const app = Fastify();
  app.register(firmwareRoutes, { firmwareDir: dir });
  return { app, dir };
}

describe("GET /firmware/:board/latest", () => {
  it("serves the manifest as text", async () => {
    const { app } = appWithImage("esp32");
    const res = await app.inject({ method: "GET", url: "/firmware/esp32/latest" });
    expect(res.statusCode).toBe(200);
    expect(res.headers["content-type"]).toMatch(/text\/plain/);
    expect(res.body).toBe("7 26\n" + "c".repeat(64) + "\n/firmware/esp32/image.bin\n");
  });
  it("serves each board from its own namespace", async () => {
    const { app } = appWithImage("pico");
    expect((await app.inject({ method: "GET", url: "/firmware/pico/latest" })).statusCode).toBe(200);
    // A board with nothing published 404s rather than leaking another board's image.
    expect((await app.inject({ method: "GET", url: "/firmware/esp32/latest" })).statusCode).toBe(404);
  });
  it("404s for an unknown / unlisted board", async () => {
    const { app } = appWithImage("esp32");
    expect((await app.inject({ method: "GET", url: "/firmware/bogus/latest" })).statusCode).toBe(404);
  });
  it("404s when nothing is published", async () => {
    const app = Fastify();
    app.register(firmwareRoutes, { firmwareDir: mkdtempSync(join(tmpdir(), "empty-")) });
    const res = await app.inject({ method: "GET", url: "/firmware/esp32/latest" });
    expect(res.statusCode).toBe(404);
  });
});

describe("GET /firmware/:board/image.bin", () => {
  it("serves the whole image", async () => {
    const { app } = appWithImage("esp32");
    const res = await app.inject({ method: "GET", url: "/firmware/esp32/image.bin" });
    expect(res.statusCode).toBe(200);
    expect(res.headers["accept-ranges"]).toBe("bytes");
    expect(res.body).toBe("HELLO-FIRMWARE-IMAGE-BYTES");
  });
  it("serves a byte range as 206", async () => {
    const { app } = appWithImage("esp32");
    const res = await app.inject({
      method: "GET", url: "/firmware/esp32/image.bin", headers: { range: "bytes=0-4" },
    });
    expect(res.statusCode).toBe(206);
    expect(res.headers["content-range"]).toBe("bytes 0-4/26");
    expect(res.body).toBe("HELLO");
  });
  it("returns 416 for an out-of-range request", async () => {
    const { app } = appWithImage("esp32");
    const res = await app.inject({
      method: "GET", url: "/firmware/esp32/image.bin", headers: { range: "bytes=9999-9999" },
    });
    expect(res.statusCode).toBe(416);
    expect(res.headers["content-range"]).toBe("bytes */26");
  });
  it("404s for an unknown board", async () => {
    const { app } = appWithImage("esp32");
    expect((await app.inject({ method: "GET", url: "/firmware/bogus/image.bin" })).statusCode).toBe(404);
  });
  it("404s when no image is published", async () => {
    const app = Fastify();
    app.register(firmwareRoutes, { firmwareDir: mkdtempSync(join(tmpdir(), "empty-img-")) });
    const res = await app.inject({ method: "GET", url: "/firmware/esp32/image.bin" });
    expect(res.statusCode).toBe(404);
  });
});
