#!/usr/bin/env node
import { readFileSync, writeFileSync, copyFileSync, existsSync, mkdirSync } from "node:fs";
import { createHash } from "node:crypto";
import { join } from "node:path";

const [, , imagePath] = process.argv;
if (!imagePath) {
  console.error("usage: publish-fw.mjs <path-to-image.bin>");
  process.exit(1);
}
const outDir = process.env.FIRMWARE_DIR ?? join(process.cwd(), "firmware");
mkdirSync(outDir, { recursive: true });

const manifestPath = join(outDir, "manifest.txt");
let nextVersion = 1;
if (existsSync(manifestPath)) {
  const first = readFileSync(manifestPath, "utf8").split("\n")[0];
  const cur = Number(first.trim().split(/\s+/)[0]);
  if (Number.isInteger(cur)) nextVersion = cur + 1;
}

const bytes = readFileSync(imagePath);
// Must match firmware OTA_MAX_IMAGE_BYTES (A/B slot capacity = 1992 KiB).
// Refuse oversized images at publish time so the failure is loud here rather
// than a silent no-update on the device (the firmware parser rejects them).
const OTA_MAX_IMAGE_BYTES = 1992 * 1024;
if (bytes.length > OTA_MAX_IMAGE_BYTES) {
  console.error(`image is ${bytes.length} bytes, exceeds OTA_MAX_IMAGE_BYTES (${OTA_MAX_IMAGE_BYTES}); refusing to publish`);
  process.exit(1);
}
const sha256 = createHash("sha256").update(bytes).digest("hex");
copyFileSync(imagePath, join(outDir, "image.bin"));
writeFileSync(manifestPath, `${nextVersion} ${bytes.length}\n${sha256}\n/firmware/image.bin\n`);
console.log(`published firmware v${nextVersion} (${bytes.length} bytes, sha256 ${sha256})`);
