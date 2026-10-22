// @vitest-environment jsdom
import { describe, expect, it } from "vitest"
import { optimizerPath, riskProfilePath } from "../api/client"
import type { RiskProfile } from "../api/trading-types"
import type { Chain, ChainRow, OptionQuote, StrategyCandidate } from "../api/types"
import { hedgeOrder, hedgeText } from "../components/HedgePanel"
import { searchQuery } from "../components/StrategyOptimizer"
import { candidateCurve, dateLines } from "../views/AnalyzeView"
import { addAnalyzeCandidate, candidateLegs, entryCost, maxAnalyzeCandidates, moveLeg, nearestRow, profileLegs, sliceRows, stepStrike,
  updateAnalyze, valueAt, zeroCrossings, type AnalyzeCandidate } from "./analyze"
import type { StrategyLeg } from "./strategy"
import { chain as fixtureChain, quote } from "../test/trading-fixtures"

const option = (strike: number, type: "call" | "put", bid: number, ask: number, tradable = true): OptionQuote =>
  ({ ...quote, symbol: `SPXW  261016${type === "call" ? "C" : "P"}0${strike}000`, bid, ask, mid: (bid + ask) / 2, tradable })
const rows: ChainRow[] = [6900, 6950, 7000, 7050, 7100].map((strike) => ({ strike, iv: .2, gex: 0, vex: 0,
  call: option(strike, "call", 100 - (strike - 6900) / 5, 101 - (strike - 6900) / 5, strike !== 6950), put: option(strike, "put", 20, 21) }))
const leg = (strike: number, side: "buy" | "sell", type: "call" | "put" = "call", ratio = 1): StrategyLeg => {
  const row = rows.find((r) => r.strike === strike)!
  return { symbol: row[type]!.symbol!, underlying: "SPX", side, ratio, type, strike, expiry: "2026-10-16PM", quote: row[type] }
}

describe("analyze legs", () => {
  it("sends legs as signed contracts and prices the spread a natural entry pays", () => {
    const spread = [leg(7000, "buy"), leg(7100, "sell"), leg(7000, "buy")]
    expect(profileLegs(spread, 3)).toEqual({ "SPXW  261016C07000000": 6, "SPXW  261016C07100000": -3 })
    // Each leg crosses half its dollar spread: two buys and a sell, a dollar wide, three units.
    expect(entryCost(spread, 3, "natural")).toBeCloseTo(3 * 0.5 * 100 * 3)
    expect(entryCost(spread, 3, "mid")).toBe(0)
    expect(entryCost([{ ...leg(7000, "buy"), quote: null }], 1, "natural")).toBeNull()
  })
  it("steps, snaps and moves strikes among tradable contracts only", () => {
    const call = leg(6900, "buy")
    expect(stepStrike(rows, call, 1)!.strike).toBe(7000)  // 6950's call is untradable
    expect(stepStrike(rows, call, -1)).toBeNull()
    expect(stepStrike(rows, leg(7100, "sell"), 1)).toBeNull()
    expect(nearestRow(rows, "call", 6960)!.strike).toBe(7000)
    expect(nearestRow(rows, "put", 6960)!.strike).toBe(6950)
    const moved = moveLeg(call, rows[4]!)
    expect(moved).toMatchObject({ strike: 7100, symbol: "SPXW  261016C07100000", side: "buy", quote: rows[4]!.call })
  })
  it("reads curves between their points and finds where they cross zero", () => {
    const prices = [90, 100, 110, 120]
    expect(valueAt(prices, [-10, 0, 10, 20], 105)).toBe(5)
    expect(valueAt(prices, [-10, 0, 10, 20], 90)).toBe(-10)
    expect(valueAt(prices, [-10, 0, null, 20], 105)).toBeNull()
    expect(valueAt(prices, [-10, 0, 10, 20], 130)).toBeNull()
    expect(zeroCrossings(prices, [-10, 0, 10, 20])).toEqual([100])
    expect(zeroCrossings(prices, [-10, 10, 5, -5])).toEqual([95, 115])
  })
  it("orders price slices by price across moves, prices, sigmas and breakevens", () => {
    const rows = sliceRows(100, { percents: [5, -5], prices: [101], sigma: 0.1, breakevens: [{ label: "BE A", price: 103 }] })
    expect(rows.map((r) => r.label)).toEqual(["−2σ", "−1σ", "-5%", "$101", "BE A", "+5%", "+1σ", "+2σ"])
    expect(rows[0]!.price).toBeCloseTo(100 * Math.exp(-0.2))
    expect(sliceRows(100, { sigma: null, prices: [-1] })).toEqual([])
  })
})

describe("analyze candidates", () => {
  it("keeps up to four per underlying in this browser, without stale quotes", () => {
    updateAnalyze("TEST", () => ({ candidates: [], selected: null }))
    for (let i = 0; i < maxAnalyzeCandidates; ++i) expect(addAnalyzeCandidate("TEST", { name: `C${i}`, legs: [leg(7000, "buy")], units: 1, shares: 0 })).toBe(true)
    expect(addAnalyzeCandidate("TEST", { name: "extra", legs: [], units: 1, shares: 0 })).toBe(false)
    const saved = JSON.parse(window.localStorage.getItem("openport.analyze.TEST")!)
    expect(saved.candidates).toHaveLength(4)
    expect(saved.candidates[0].legs[0].quote).toBeNull()
    expect(saved.selected).toBe(saved.candidates[3].id)
  })
  it("turns an optimizer candidate into legs", () => {
    const candidate = { legs: [{ symbol: "SPXW  261016C07000000", side: "buy", ratio: 1, type: "call", strike: 7000, expiry: "2026-10-16PM",
      bid: 1, ask: 2, mid: 1.5, iv: 0.2 }] } as StrategyCandidate
    expect(candidateLegs(candidate, "SPX")).toEqual([{ symbol: "SPXW  261016C07000000", underlying: "SPX", side: "buy", ratio: 1, type: "call",
      strike: 7000, expiry: "2026-10-16PM", quote: null }])
  })
  it("draws today, a custom date and just past the first expiry", () => {
    const candidate: AnalyzeCandidate = { id: "a", name: "A", legs: [leg(7000, "buy")], units: 1, shares: 0, visible: true }
    const chains = new Map<string, Chain>([["2026-10-16PM", fixtureChain]])
    const now = Date.parse("2026-10-06T20:00:00Z")
    const lines = dateLines(candidate, chains, now, 3)
    expect(lines.map((l) => l.key)).toEqual(["today", "custom", "expiry"])
    expect(lines[2]!.days).toBe(10.01)
    expect(dateLines(candidate, new Map(), now, null).map((l) => l.key)).toEqual(["today"])
  })
  it("adds a covered call's shares and takes off the spread paid", () => {
    const profile = { spot: 100, prices: [90, 100, 110], curves: [{ pnl: [-5, 0, 5] }] } as unknown as RiskProfile
    const candidate: AnalyzeCandidate = { id: "c", name: "C", legs: [leg(7000, "sell")], units: 2, shares: 100, visible: true }
    // Shares: 200 × the move; the short call's half spread, 0.5 × 100 × 2 units.
    expect(candidateCurve(profile, 0, candidate, "natural")).toEqual([-5 - 2000 - 100, 0 - 100, 5 + 2000 - 100])
    expect(candidateCurve(profile, 0, candidate, "mid")).toEqual([-2005, 0, 2005])
  })
})

describe("analyze requests", () => {
  it("adds legs and the held flag to the profile query", () => {
    expect(riskProfilePath({ underlying: "SPX", days: [0, 10], legs: { "SPXW  261016C07000000": 2, "SPXW  261016C07100000": -2, gone: 0 }, held: false }))
      .toBe("/api/risk/profile?underlying=SPX&days=0%2C10&legs=SPXW%20%20261016C07000000%3A2%2CSPXW%20%20261016C07100000%3A-2&held=false")
    expect(riskProfilePath({ underlying: "SPX", held: true })).toBe("/api/risk/profile?underlying=SPX")
  })
  it("builds the optimizer query from the form, leaving defaults to the server", () => {
    const query = searchQuery({ target: "7050", budget: "1500", date: "2026-10-16", expiries: ["2026-10-16PM"], structures: ["vertical", "condor"],
      rank: "pop", vol: "-2", price: "mid" })
    expect(query).toEqual({ target: 7050, budget: 1500, date: "2026-10-16", expiries: ["2026-10-16PM"], structures: ["vertical", "condor"],
      rank: "pop", vol_points: -2, price: "mid", limit: 25 })
    expect(optimizerPath("SPX", query as never)).toBe("/api/underlyings/SPX/optimizer?target=7050&budget=1500&date=2026-10-16&expiry_ids=2026-10-16PM" +
      "&structures=vertical%2Ccondor&rank=pop&vol_points=-2&price=mid&limit=25")
    expect(searchQuery({ target: "", budget: "1", date: "", expiries: [], structures: ["single"], rank: "expected", vol: "0", price: "natural" }))
      .toEqual({ reason: "Enter the price you expect." })
    expect(searchQuery({ target: "1", budget: "1", date: "", expiries: [], structures: [], rank: "expected", vol: "0", price: "natural" }))
      .toEqual({ reason: "Pick at least one structure." })
  })
  it("describes hedges and turns option hedges into what-if orders", () => {
    const base = { name: "SPX ATM call", kind: "option", underlying: "SPX", instrument: "SPXW  261016C07000000", beta_hedge: false,
      expiry: "2026-10-16PM", strike: 7000, type: "call", feasible: true, reason: null, side: "sell", quantity: 3, price: 4.5, premium: -1350,
      spread_cost: 15, residual: null, order: { symbol: "SPXW  261016C07000000", side: "sell", type: "limit", time_in_force: "day", quantity: 3, limit_price: "4.50" },
      stock_trade: null, exceeds_order_limit: false, sizing: "dollar_delta", instrument_beta: 1 } as const
    expect(hedgeText(base)).toBe("Sell 3 SPX 7000 call (2026-10-16PM)")
    expect(hedgeOrder(base)).toEqual({ client_order_id: "", symbol: "SPXW  261016C07000000", side: "sell", type: "limit", time_in_force: "day", quantity: 3, limit_price: "4.50" })
    expect(hedgeText({ ...base, kind: "shares", quantity: 1500, side: "buy", underlying: "SPY", order: null })).toBe("Buy 1,500 SPY shares")
    expect(hedgeText({ ...base, quantity: 0, side: null, reason: "Already within the target" })).toBe("Already within the target")
    expect(hedgeText({ ...base, feasible: false, quantity: null, reason: "No quote" })).toBe("No quote")
  })
})
