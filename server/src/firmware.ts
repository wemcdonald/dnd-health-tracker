import { readFileSync } from "node:fs";
import { join } from "node:path";

export type FirmwareManifest = {
  version: number;
  size: number;
  sha256: string;
  imagePath: string;
};

const SHA256_RE = /^[0-9a-f]{64}$/;

export function readFirmwareManifest(dir: string): FirmwareManifest | null {
  let raw: string;
  try {
    raw = readFileSync(join(dir, "manifest.txt"), "utf8");
  } catch {
    return null;
  }
  const lines = raw.split("\n");
  if (lines.length < 3) return null;
  const parts = lines[0]!.trim().split(/\s+/);
  if (parts.length < 2) return null;
  const version = Number(parts[0]);
  const size = Number(parts[1]);
  const sha256 = lines[1]!.trim();
  const imagePath = lines[2]!.trim();
  if (!Number.isInteger(version) || version < 0) return null;
  if (!Number.isInteger(size) || size <= 0) return null;
  if (!SHA256_RE.test(sha256)) return null;
  if (!imagePath.startsWith("/")) return null;
  return { version, size, sha256, imagePath };
}

export function firmwareManifestText(m: FirmwareManifest): string {
  return `${m.version} ${m.size}\n${m.sha256}\n${m.imagePath}\n`;
}
