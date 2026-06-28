import type { FastifyInstance, FastifyPluginOptions } from "fastify";
import { createReadStream, statSync } from "node:fs";
import { join } from "node:path";
import { readFirmwareManifest, firmwareManifestText } from "../firmware.js";

type Opts = FastifyPluginOptions & { firmwareDir: string };

export async function firmwareRoutes(app: FastifyInstance, opts: Opts) {
  const dir = opts.firmwareDir;

  app.get("/firmware/latest", async (_req, reply) => {
    const m = readFirmwareManifest(dir);
    if (!m) return reply.code(404).type("text/plain; charset=utf-8").send("no firmware\n");
    return reply
      .type("text/plain; charset=utf-8")
      .header("Cache-Control", "no-store")
      .send(firmwareManifestText(m));
  });

  app.get("/firmware/image.bin", async (req, reply) => {
    const path = join(dir, "image.bin");
    let total: number;
    try {
      total = statSync(path).size;
    } catch {
      return reply.code(404).type("text/plain; charset=utf-8").send("no image\n");
    }
    reply.header("Accept-Ranges", "bytes").type("application/octet-stream");

    const range = req.headers.range;
    if (range) {
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
