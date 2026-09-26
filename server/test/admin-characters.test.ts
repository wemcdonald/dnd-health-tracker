import { describe, it, expect } from "vitest";
import Fastify from "fastify";
import formbody from "@fastify/formbody";
import { adminRoutes } from "../src/routes/admin.js";
import { getCharacter } from "../src/db.js";

async function app() {
  const a = Fastify();
  await a.register(formbody);
  await a.register(adminRoutes);
  return a;
}
const form = (a: Awaited<ReturnType<typeof app>>, url: string, body: string) =>
  a.inject({ method: "POST", url, payload: body, headers: { "content-type": "application/x-www-form-urlencoded" } });

describe("admin characters", () => {
  it("saves a character with a valid slug", async () => {
    const r = await form(await app(), "/admin/characters", "slug=thorin&characterRef=12345678");
    expect(r.statusCode).toBe(302);
    expect(getCharacter("thorin")?.characterId).toBe("12345678");
  });

  it("accepts a slug at exactly the 64-char cap (same cap the firmware enforces)", async () => {
    const slug = "a".repeat(64);
    const r = await form(await app(), "/admin/characters", `slug=${slug}&characterRef=12345678`);
    expect(r.statusCode).toBe(302);
    expect(getCharacter(slug)?.characterId).toBe("12345678");
  });

  it("rejects a slug over 64 chars", async () => {
    const slug = "a".repeat(65);
    const r = await form(await app(), "/admin/characters", `slug=${slug}&characterRef=12345678`);
    expect(r.statusCode).toBe(400);
    expect(getCharacter(slug)).toBeUndefined();
  });

  it("rejects invalid characters in the slug", async () => {
    const r = await form(await app(), "/admin/characters", "slug=Bad Slug&characterRef=12345678");
    expect(r.statusCode).toBe(400);
  });
});
