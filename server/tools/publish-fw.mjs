#!/usr/bin/env node
// Publish an OTA image for one board into the server's per-board firmware feed.
//
//   publish-fw.mjs <board> <path-to-image.bin> [version]
//
// Images are namespaced per board so a Pico image can never be served to an
// ESP32 (which would brick it). This writes FIRMWARE_DIR/<board>/{image.bin,
// manifest.txt} with an imagePath of /firmware/<board>/image.bin — the exact
// path the device fetches. Keeping this the single publish path is what makes
// serving updates for multiple architectures foolproof.
import { readFileSync, writeFileSync, copyFileSync, existsSync, mkdirSync } from "node:fs";
import { createHash } from "node:crypto";
import { join } from "node:path";

// Pico: A/B slot capacity 1992 KiB. ESP32: HEALTHBAR_C3 ota_0/ota_1 = 0x1D0000
// (firmware-esp32/board/HEALTHBAR_C3/partitions.csv); override with FW_MAX_BYTES.
const BOARD_MAX_BYTES = {
  pico: 1992 * 1024,
  esp32: Number(process.env.FW_MAX_BYTES) || 0x1d0000,
};

const [, , board, imagePath, versionArg] = process.argv;
if (!board || !imagePath || !(board in BOARD_MAX_BYTES)) {
  console.error(`usage: publish-fw.mjs <${Object.keys(BOARD_MAX_BYTES).join("|")}> <path-to-image.bin> [version]`);
  process.exit(1);
}

const root = process.env.FIRMWARE_DIR ?? join(process.cwd(), "firmware");
const outDir = join(root, board);
mkdirSync(outDir, { recursive: true });

const manifestPath = join(outDir, "manifest.txt");
// The published manifest version MUST match the version baked into the image
// (the device gates updates on manifest.version > its baked FIRMWARE_VERSION).
// An explicit version keeps the two consistent; otherwise auto-increment this
// board's prior manifest.
let nextVersion;
if (versionArg !== undefined && Number.isInteger(Number(versionArg))) {
  nextVersion = Number(versionArg);
} else {
  nextVersion = 1;
  if (existsSync(manifestPath)) {
    const first = readFileSync(manifestPath, "utf8").split("\n")[0];
    const cur = Number(first.trim().split(/\s+/)[0]);
    if (Number.isInteger(cur)) nextVersion = cur + 1;
  }
}

const bytes = readFileSync(imagePath);
// Refuse oversized images at publish time so the failure is loud here rather
// than a silent no-update on the device (the firmware parser rejects them too).
const maxBytes = BOARD_MAX_BYTES[board];
if (bytes.length > maxBytes) {
  console.error(`image is ${bytes.length} bytes, exceeds ${board} max (${maxBytes}); refusing to publish`);
  process.exit(1);
}
const sha256 = createHash("sha256").update(bytes).digest("hex");
copyFileSync(imagePath, join(outDir, "image.bin"));
writeFileSync(manifestPath, `${nextVersion} ${bytes.length}\n${sha256}\n/firmware/${board}/image.bin\n`);
console.log(`published ${board} firmware v${nextVersion} (${bytes.length} bytes, sha256 ${sha256}) -> ${outDir}`);
