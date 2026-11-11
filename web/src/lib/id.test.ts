import { afterEach, expect, it, vi } from "vitest"
import { randomId } from "./id"

const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/
afterEach(() => vi.unstubAllGlobals())

it("builds version 4 UUIDs where crypto.randomUUID is missing, as over plain HTTP on a LAN address", () => {
  const real = globalThis.crypto
  vi.stubGlobal("crypto", { getRandomValues: real.getRandomValues.bind(real) })
  const ids = new Set(Array.from({ length: 200 }, () => randomId()))
  expect(ids.size).toBe(200)
  for (const id of ids) expect(id).toMatch(uuid)
})

it("uses crypto.randomUUID where the browser offers it", () => {
  vi.stubGlobal("crypto", { randomUUID: () => "secure-context-id" })
  expect(randomId()).toBe("secure-context-id")
})
