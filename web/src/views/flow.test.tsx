import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, describe, expect, it, vi } from "vitest"
import { flowPath } from "../api/client"
import type { AnalysisGrid, AnalysisPortfolio, AnalysisPortfolios, AnalysisProfile, EarningsHistory, Flow, FlowPrint } from "../api/types"
import { EarningsPanel, pricedText } from "../components/EarningsPanel"
import { emptyTapeText, flowTags, sideText, strikeBars } from "../lib/flow"
import { AnalysisView, profileSeries } from "./AnalysisView"
import { FlowView } from "./FlowView"

vi.mock("../api/live", () => ({ useLive: () => ({ version: () => 1, status: { trading: { write: "open" } } }) }))

// Hand-made payloads in the server's shapes; the prints are not market data.
const print: FlowPrint = { sequence: 9, time: "2026-10-21T14:30:01.000Z", symbol: "SPXW  261021C05800000", expiry: "2026-10-21PM",
  strike: 5800, type: "call", price: 4.3, size: 250, premium: 107500, bid: 4.1, ask: 4.3, quote_time: "2026-10-21T14:30:00.000Z",
  side: "ask", aggressor: "buy", venue: 27, sweep: true, block: true, group: null, simulated: false }
const totals = { call_net: 107500, put_net: -1500, net: 106000, bought: 107500, sold: 1500, unclassified: 0, contracts: 255, prints: 2 }
const flow: Flow = { symbol: "SPX", provider: "databento", trades: true, source: "feed", message: null, as_of: print.time,
  block_size: 100, capacity: 5000, retained: 2, total: 2, evicted: 0, matched: 2, totals,
  expiries: [{ ...totals, expiry: "2026-10-21PM" }],
  strikes: [{ ...totals, expiry: "2026-10-21PM", strike: 5800, put_net: 0, net: 107500 }, { ...totals, expiry: "2026-10-21PM", strike: 5700, call_net: 0, net: -1500 }],
  prints: [print, { ...print, sequence: 8, type: "put", strike: 5700, price: 2.9, size: 5, premium: 1450, side: "outside", aggressor: "sell", sweep: false, block: false, group: 3 }],
  model: "model" }

function render(node: ReactNode, data: [unknown[], unknown][]) {
  const client = new QueryClient({ defaultOptions: { queries: { staleTime: Infinity, gcTime: Infinity, retry: false } } })
  for (const [key, value] of data) client.setQueryData(key, value)
  const html = renderToStaticMarkup(<QueryClientProvider client={client}>{node}</QueryClientProvider>)
  client.clear()
  return html
}
afterEach(() => vi.restoreAllMocks())

describe("flow", () => {
  it("builds the query and labels prints", () => {
    expect(flowPath("SPX", { limit: 200 })).toBe("/api/underlyings/SPX/flow?limit=200")
    expect(flowPath("SPX", { type: "put", side: ["bid", "ask"], only: "sweep", min_premium: 10000 }))
      .toBe("/api/underlyings/SPX/flow?type=put&side=bid%2Cask&only=sweep&min_premium=10000")
    expect(flowTags(print)).toEqual(["sweep", "block"])
    expect(flowTags({ ...print, sweep: false, block: false, group: 4 })).toEqual(["multi-leg"])
    expect(sideText(print)).toBe("at ask")
    expect(sideText(flow.prints[1]!)).toBe("below bid")
    expect(sideText({ ...print, side: "outside", aggressor: "buy" })).toBe("above ask")
    expect(strikeBars(flow, null).map((b) => b.value)).toEqual([107500, -1500])
    expect(strikeBars(flow, "2026-10-21PM").map((b) => b.label)).toEqual(["5800", "5700"])
  })

  it("never shows an empty tape as a quiet market", () => {
    const none: Flow = { ...flow, provider: "cboe", trades: false, source: "none", retained: 0, matched: 0, prints: [], expiries: [], strikes: [],
      message: "cboe does not deliver option trades, so there is no time and sales or flow for SPX." }
    expect(emptyTapeText(none)).toBe(none.message)
    expect(emptyTapeText({ ...flow, message: null, retained: 0 })).toBe("No option trades for SPX yet.")
    expect(emptyTapeText({ ...flow, prints: [] })).toBe("No prints match these filters.")
    const html = render(<FlowView symbol="SPX" />, [[["flow", "SPX", { limit: 200 }, 1], none]])
    expect(html).toContain("cboe does not deliver option trades")
    expect(html).not.toContain("<table")
  })

  it("renders the tape, net premium and the demo's simulated label", () => {
    let html = render(<FlowView symbol="SPX" />, [[["flow", "SPX", { limit: 200 }, 1], flow]])
    for (const text of ["Net premium by expiry", "Time and sales", "at ask", "below bid", "sweep", "block", "multi-leg", "≥ 100 contracts"]) expect(html).toContain(text)
    expect(html).not.toContain("simulated")
    const simulated: Flow = { ...flow, source: "simulated", message: "Simulated prints: the demo market's volume placed at its bid, ask or midpoint. They are not market trades.",
      prints: flow.prints.map((p) => ({ ...p, simulated: true })) }
    html = render(<FlowView symbol="SPX" />, [[["flow", "SPX", { limit: 200 }, 1], simulated]])
    expect(html).toContain("They are not market trades.")
    expect(html).toContain(">simulated<")
  })
})

describe("earnings", () => {
  const history: EarningsHistory = { symbol: "SPX", error: null, model: "Implied: the front straddle", summary: { events: 2, over: 1, under: 1, imported: 1, over_rate: .5, mean_implied: .04, mean_realized: .03 },
    moves: [
      { symbol: "SPX", date: "2026-10-20", session: "after_close", label: null, source: "recorded", implied: .041, implied_at: "2026-10-20T19:59:00.000Z", expiry: "2026-10-23PM",
        strike: 5800, straddle: 237.8, spot: 5800, realized: null, before: "2026-10-20", after: "2026-10-21", close_before: null, close_after: null, priced: null },
      { symbol: "SPX", date: "2026-07-30", session: "after_close", label: "Q3", source: "imported", implied: .041, implied_at: null, expiry: null,
        strike: null, straddle: null, spot: null, realized: -.023, before: null, after: null, close_before: null, close_after: null, priced: "over" },
    ] }
  it("shows the hit rate, labels imported rows and waits for closes", () => {
    const html = render(<EarningsPanel symbol="SPX" />, [[["earnings", "SPX"], history]])
    for (const text of ["1 of 2</span> events", "50%", "imported", "recorded", "over-priced", "waiting for the close", "-2.3%", "4.1%", "5800 straddle 237.80"]) expect(html).toContain(text)
    expect(pricedText({ ...history.moves[0]!, implied: null })).toBe("waiting for the straddle")
  })
})

describe("analysis", () => {
  const portfolio: AnalysisPortfolio = { portfolio: "ira", id: "ira", name: "IRA", analysis_only: true, read_only: true,
    note: "Imported for analysis only: OpenPort never places orders for it, and it is not a paper account.", as_of: null, unpriced: ["AAPL"], complete: false,
    format: "schwab", imported: "2026-10-21T14:00:00.000Z", skipped: ["line 7 Cash & Cash Investments: no quantity (cash or a total)"],
    value: null, cost: 1000, unrealised: null, views: ["grid", "profile"],
    exposure: { aggregate: { dollar_delta: 1000, dollar_gamma_1pct: 10, vega: 50, theta: -5 }, underlyings: [], complete: false },
    holdings: [
      { symbol: "SPXW  261120C05800000", quantity: 2, source: "line 4", cost: 40, underlying: "SPX", type: "call", expiry: "2026-11-20", strike: 5800, multiplier: 100,
        mark: 42, value: 8400, unrealised: 400, delta: 110, gamma: 1, vega: 1500, theta: -60, status: "priced" },
      { symbol: "AAPL", quantity: 10, source: "line 5", cost: 180, underlying: "AAPL", type: "shares", expiry: null, strike: null, multiplier: 1,
        mark: null, value: null, unrealised: null, delta: null, gamma: null, vega: null, theta: null, status: "no_market_data" },
    ] }
  const profile: AnalysisProfile = { portfolio: "ira", name: "IRA", analysis_only: true, read_only: true, note: "", as_of: null, unpriced: [], complete: true,
    view: "profile", underlying: null, benchmark: "SPY", reference: "SPY", spot: null, vol_points: 0, percent: [-10, 0, 10], prices: [null, null, null],
    curves: [{ days: 0, time: "", pnl: [-100, 0, 120], clamped: false }, { days: 7, time: "", pnl: [-90, -5, 110], clamped: false }], betas: [], model: "Black-76" }
  const grid: AnalysisGrid = { portfolio: "ira", name: "IRA", analysis_only: true, read_only: true, note: "", as_of: null, unpriced: [], complete: true, view: "grid",
    spot_percent: [-5, 0, 5], vol_points: [0], pnl: [[-50], [0], [60]], clamped: [[false], [false], [false]], model: "grid model" }
  const list: AnalysisPortfolios = { portfolios: [{ id: "ira", name: "IRA", format: "schwab", imported: portfolio.imported, holdings: 2, analysis_only: true, read_only: true }],
    formats: ["generic", "tastytrade", "schwab", "ibkr"], note: "", error: null }

  it("labels the portfolio analysis only and offers no trading", () => {
    const html = render(<AnalysisView />, [[["analysis-portfolios"], list], [["analysis", "ira"], portfolio],
      [["analysis-profile", "ira", null], profile], [["analysis-grid", "ira"], grid]])
    for (const text of ["analysis only", "never places orders", "underlying not on this feed", "Not valued: AAPL", "1 lines not imported", "Scenario grid", "Risk profile", "Import for analysis"]) expect(html).toContain(text)
    for (const text of ["Buy", "Sell", "Close position", "Order"]) expect(html).not.toContain(`>${text}<`)
    expect(profileSeries(profile).map((s) => s.label)).toEqual(["today", "+7d"])
    expect(profileSeries(profile)[1]!.points.map((p) => p.y)).toEqual([-90, -5, 110])
  })
})
