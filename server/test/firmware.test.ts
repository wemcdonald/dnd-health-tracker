import { describe, it, expect } from "vitest";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { readFirmwareManifest, firmwareManifestText } from "../src/firmware.js";

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
});

describe("firmwareManifestText", () => {
  it("round-trips the wire format", () => {
    const m = { version: 42, size: 418234, sha256: "b".repeat(64), imagePath: "/firmware/image.bin" };
    expect(firmwareManifestText(m)).toBe("42 418234\n" + "b".repeat(64) + "\n/firmware/image.bin\n");
  });
});
