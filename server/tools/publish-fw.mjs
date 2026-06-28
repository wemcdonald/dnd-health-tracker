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
const sha256 = createHash("sha256").update(bytes).digest("hex");
copyFileSync(imagePath, join(outDir, "image.bin"));
writeFileSync(manifestPath, `${nextVersion} ${bytes.length}\n${sha256}\n/firmware/image.bin\n`);
console.log(`published firmware v${nextVersion} (${bytes.length} bytes, sha256 ${sha256})`);
