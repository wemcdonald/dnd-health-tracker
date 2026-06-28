/**
 * Server bootstrap: wires the admin UI + device endpoint, then starts a manager
 * per enabled character (polling + optional WSS) so files are live immediately.
 */

import Fastify from "fastify";
import formbody from "@fastify/formbody";
import { dndRoutes } from "./routes/dnd.js";
import { adminRoutes } from "./routes/admin.js";
import { firmwareRoutes } from "./routes/firmware.js";
import { syncManagers, stopAllManagers } from "./manager.js";

const PORT = Number(process.env["PORT"] ?? 8080);
const HOST = process.env["HOST"] ?? "0.0.0.0";

async function main(): Promise<void> {
  const app = Fastify({ logger: true });
  await app.register(formbody); // parse application/x-www-form-urlencoded posts
  await app.register(dndRoutes);
  await app.register(adminRoutes);
  await app.register(firmwareRoutes, { firmwareDir: process.env.FIRMWARE_DIR ?? "firmware" });

  // Start the per-character managers (reads the DB).
  syncManagers();

  const shutdown = async (signal: string): Promise<void> => {
    app.log.info(`received ${signal}, shutting down`);
    stopAllManagers();
    await app.close();
    process.exit(0);
  };
  process.on("SIGINT", () => void shutdown("SIGINT"));
  process.on("SIGTERM", () => void shutdown("SIGTERM"));

  await app.listen({ port: PORT, host: HOST });
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
