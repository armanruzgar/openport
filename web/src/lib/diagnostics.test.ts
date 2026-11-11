import { describe, expect, it } from "vitest"
import { status } from "../test/trading-fixtures"
import { diagnostics, providerCategory } from "./diagnostics"
import { providerNames } from "./provider"

describe("diagnostics provider", () => {
  it.each(providerNames)("names %s, which openportd can run", (name) => {
    expect(providerCategory(name)).toBe(name)
    expect(diagnostics({ ...status, provider: { ...status.provider, name } }, status.feed, "open")).toContain(`Provider: ${name}`)
  })
  it.each([["schwab+cboe", "schwab+cboe"], ["ibkr+tradier", "ibkr+tradier"], ["replay (alpaca)", "replay"], ["Schwab", "schwab"],
    ["custom", "other"], ["schwab+custom", "schwab+other"], ["schwabby", "other"]])("reports %s as %s", (name, category) => {
    expect(providerCategory(name)).toBe(category)
  })
})
