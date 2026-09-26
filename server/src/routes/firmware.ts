import type { FastifyInstance, FastifyPluginOptions } from "fastify";
import { createReadStream, statSync } from "node:fs";
import { join } from "node:path";
import { readFirmwareManifest, firmwareManifestText } from "../firmware.js";

type Opts = FastifyPluginOptions & { firmwareDir: string };

/**
 * Firmware images are namespaced **per board** so an image for one architecture
 * can never be served to (and brick) another. A device fetches its own board's
 * feed:  /firmware/<board>/latest  +  /firmware/<board>/image.bin
 *
 * `board` is allow-listed (never used to build a path until it matches), which
 * also blocks path traversal. Add new boards here as they ship.
 */
const ALLOWED_BOARDS = new Set(["pico", "esp32"]);

export async function firmwareRoutes(app: FastifyInstance, opts: Opts) {
  const root = opts.firmwareDir;

  // Resolve the per-board dir, or null if the board isn't recognised.
  const boardDir = (board: string): string | null =>
    ALLOWED_BOARDS.has(board) ? join(root, board) : null;

  app.get<{ Params: { board: string } }>("/firmware/:board/latest", async (req, reply) => {
    const dir = boardDir(req.params.board);
    if (!dir) return reply.code(404).type("text/plain; charset=utf-8").send("unknown board\n");
    const m = readFirmwareManifest(dir);
    if (!m) return reply.code(404).type("text/plain; charset=utf-8").send("no firmware\n");
    return reply
      .type("text/plain; charset=utf-8")
      .header("Cache-Control", "no-store")
      .send(firmwareManifestText(m));
  });

  app.get<{ Params: { board: string } }>("/firmware/:board/image.bin", async (req, reply) => {
    const dir = boardDir(req.params.board);
    if (!dir) return reply.code(404).type("text/plain; charset=utf-8").send("unknown board\n");
    const path = join(dir, "image.bin");
    let total: number;
    try {
      total = statSync(path).size;
    } catch {
      return reply.code(404).type("text/plain; charset=utf-8").send("no image\n");
    }
    // no-store: Cloudflare otherwise caches .bin for hours, so a republished
    // image would be served stale and fail the device's SHA-256 check.
    reply
      .header("Accept-Ranges", "bytes")
      .header("Cache-Control", "no-store")
      .type("application/octet-stream");

    const range = req.headers.range;
    if (range) {
      // Handles bytes=START-END and bytes=START- forms; suffix ranges (bytes=-N)
      // and multi-range sets are not implemented — an unrecognised Range is ignored
      // and the full file is returned (RFC 9110-compliant).
      const m = /^bytes=(\d+)-(\d*)$/.exec(range);
      if (m) {
        const start = Number(m[1]);
        const end = m[2] ? Number(m[2]) : total - 1;
        if (start <= end && end < total) {
          reply
            .code(206)
            .header("Content-Range", `bytes ${start}-${end}/${total}`)
            .header("Content-Length", String(end - start + 1));
          return reply.send(createReadStream(path, { start, end }));
        }
        return reply.code(416).header("Content-Range", `bytes */${total}`).send();
      }
    }
    reply.header("Content-Length", String(total));
    return reply.send(createReadStream(path));
  });
}
