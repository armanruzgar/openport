// @vitest-environment jsdom
import { afterEach, expect, it } from "vitest"
import { updateAnalyze, type AnalyzeState } from "./analyze"
import { updateWhatIf, type WhatIfState } from "./what-if"

afterEach(() => localStorage.clear())

it("keeps only what-if candidates whose every field checks out", () => {
  // A candidate without a name used to throw on every render of Positions.
  const good = { id: "a2", name: "Close all", orders: [{ client_order_id: "", symbol: "SPXW  261016C07000000", side: "sell", type: "market", time_in_force: "ioc", quantity: 1 }] }
  const spread = { id: "a4", name: "Spread", orders: [{ client_order_id: "", type: "limit", limit_price: "1.20", time_in_force: "day", quantity: 2,
    legs: [{ symbol: "SPXW  261016P06900000", side: "sell", ratio: 1 }, { symbol: "SPXW  261016P06890000", side: "buy", ratio: 1 }] }] }
  localStorage.setItem("openport.what-if.live:damaged", JSON.stringify({ candidates: [
    { id: "a1", orders: [{ symbol: "X" }] }, good,
    { id: "a3", name: "Bad legs", orders: [{ legs: [{ symbol: 1 }], quantity: 1, type: "market" }] },
    { id: "a5", name: "No price", orders: [{ symbol: "X", side: "buy", type: "limit", quantity: 1 }] },
    spread, null, "text"], target: "a1" }))
  let loaded: WhatIfState | undefined
  updateWhatIf("live:damaged", (state) => { loaded = state; return state })
  expect(loaded).toEqual({ candidates: [good, spread], target: null })
  localStorage.setItem("openport.what-if.live:other", "[1, 2]")
  updateWhatIf("live:other", (state) => { loaded = state; return state })
  expect(loaded).toEqual({ candidates: [], target: null })
})

it("keeps only Analyze candidates whose every field checks out", () => {
  const leg = { symbol: "SPXW  261016C07000000", underlying: "SPX", side: "buy", ratio: 1, type: "call", strike: 7000, expiry: "2026-10-16PM", quote: { bid: 1 } }
  localStorage.setItem("openport.analyze.DAMAGED", JSON.stringify({ candidates: [
    { id: "x", name: "Old", legs: [{ ...leg, strike: "7000" }], units: 1, shares: 0, visible: true },
    { id: "y", name: "Good", legs: [leg], units: 2, shares: 0, visible: true },
    { id: "z", legs: [leg], units: 1, shares: 0, visible: true }], selected: "x" }))
  let loaded: AnalyzeState | undefined
  updateAnalyze("DAMAGED", (state) => { loaded = state; return state })
  expect(loaded).toEqual({ candidates: [{ id: "y", name: "Good", legs: [{ ...leg, quote: null }], units: 2, shares: 0, visible: true }], selected: "y" })
})
