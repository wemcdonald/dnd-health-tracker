import { describe, expect, it } from "vitest";
import { normalizeCobaltCookie } from "../src/auth.js";

describe("normalizeCobaltCookie", () => {
  it("prefixes a bare CobaltSession value (the common devtools paste)", () => {
    expect(normalizeCobaltCookie("eyJhbGci.abc.def")).toBe("CobaltSession=eyJhbGci.abc.def");
  });

  it("trims surrounding whitespace before prefixing", () => {
    expect(normalizeCobaltCookie("  eyJabc  ")).toBe("CobaltSession=eyJabc");
  });

  it("leaves an already-named cookie untouched", () => {
    expect(normalizeCobaltCookie("CobaltSession=eyJabc")).toBe("CobaltSession=eyJabc");
  });

  it("leaves a full multi-cookie header untouched", () => {
    const header = "foo=1; CobaltSession=eyJabc; bar=2";
    expect(normalizeCobaltCookie(header)).toBe(header);
  });

  it("returns empty string unchanged", () => {
    expect(normalizeCobaltCookie("")).toBe("");
  });
});
