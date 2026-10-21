// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { HedgeSuggestions, RiskProfile, RiskProfileQuery } from "../api/trading-types"
import type { Chain, ChainRow, StrategySearch } from "../api/types"
import { LineChart } from "../charts/LineChart"
import { HedgePanel } from "../components/HedgePanel"
import { addAnalyzeCandidate, updateAnalyze } from "../lib/analyze"
import { updateWhatIf, useWhatIf } from "../lib/what-if"
import { renderTimeout, waitForRender } from "../test/render"
import { account, chain, expiry, portfolio, quote, risk, status, summary, trading } from "../test/trading-fixtures"
import { AnalyzeView } from "./AnalyzeView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../api/trading", async (original) => ({ ...await original<typeof import("../api/trading")>(), useRefreshTrading: () => vi.fn() }))

const row = (strike: number): ChainRow => ({ strike, iv: .2, gex: 0, vex: 0,
  call: { ...quote, symbol: `SPXW  261016C0${strike}000`, bid: 60 - (strike - 7000) / 5, ask: 61 - (strike - 7000) / 5, mid: 60.5 - (strike - 7000) / 5 },
  put: { ...quote, symbol: `SPXW  261016P0${strike}000`, bid: 50, ask: 51, mid: 50.5 } })
const wide: Chain = { ...chain, strikes: [6900, 6950, 7000, 7050, 7100, 7150, 7200].map(row) }
const legsOf = (query: RiskProfileQuery) => query.legs ?? {}
/** A profile with one curve per requested date that crosses zero at 7000. */
function profileFor(query: RiskProfileQuery): RiskProfile {
  const prices = [6300, 7000, 7700]
  return { account_version: "17", time: "2026-09-23T15:30:00Z", underlying: "SPX", benchmark: null, reference: "SPX", spot: 7000, vol_points: query.iv ?? 0,
    equity: "99988.70", room: "2500.00", soft_room: null, percent: [-10, 0, 10], prices, betas: [], weighted_dollar_delta: null, weighted_delta: null,
    complete: true, model: "Black-76 fixture", held: query.held !== false, legs: Object.entries(legsOf(query)).map(([symbol, quantity]) => ({ symbol, quantity })),
    curves: (query.days ?? [0]).map((days) => ({ days: Number(days), label: null, time: "2026-09-23T15:30:00Z", pnl: [-400, 0, 600], clamped: false,
      horizon: { until: "2026-09-23T20:00:00Z", sigma: 0.05, one_sd: { low: 6650, high: 7350 }, two_sd: null }, down: null, up: null, soft_down: null, soft_up: null })) }
}
const search: StrategySearch = {
  symbol: "SPX", as_of: "2026-09-23T15:30:00Z", spot: 7000, target_price: 7000, target_date: "2026-10-16", target_time: "2026-10-16T20:00:00Z",
  budget: 1000, vol_points: 0, rank: "expected", price: "natural", structures: ["vertical"], expiries: [expiry.id], skipped: [],
  considered: 12, evaluated: 12, unbounded: 0, over_budget: 3, within_budget: 1, truncated: false, max_candidates: 5000, variance_fallback: false, label: "Derived estimates", model: "Fixture model",
  candidates: [{ name: "Bull call 7050/7100 Oct 16", structure: "vertical", shares: 0, net: 10.5, max_loss: 1050, max_profit: 3950, breakevens: [7060.5],
    pop: { risk_neutral: 0.41, user: 0.47 }, expected: { risk_neutral: -50, user: 25 }, return_on_risk: 0.024, quantity: 1, valued_at: "2026-10-16T20:00:00Z",
    sigma: { risk_neutral: 0.05, user: 0.05 },
    legs: [{ symbol: "SPXW  261016C07050000", side: "buy", ratio: 1, type: "call", strike: 7050, expiry: expiry.id, bid: 50, ask: 51, mid: 50.5, iv: .2 },
      { symbol: "SPXW  261016C07100000", side: "sell", ratio: 1, type: "call", strike: 7100, expiry: expiry.id, bid: 40, ask: 41, mid: 40.5, iv: .2 }] }],
}

let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.stubGlobal("ResizeObserver", class {
    constructor(private readonly callback: (entries: { contentRect: { width: number; height: number } }[]) => void) {}
    observe() { this.callback([{ contentRect: { width: 640, height: 320 } }]) }
    disconnect() {}
  })
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  vi.spyOn(api, "summary").mockResolvedValue(summary)
  vi.spyOn(api, "chain").mockResolvedValue(wide)
  vi.spyOn(api, "portfolio").mockResolvedValue(portfolio)
  vi.spyOn(api, "account").mockResolvedValue(account)
  vi.spyOn(api, "orders").mockResolvedValue({ account_version: "17", orders: [] })
  vi.spyOn(api, "previewOrder").mockRejectedValue(new Error("fixture preview"))
  vi.spyOn(api, "riskProfile").mockImplementation(async (query) => profileFor(query))
  updateAnalyze("SPX", () => ({ candidates: [], selected: null }))
  updateWhatIf("live:main", () => ({ candidates: [], target: null }))
  client = new QueryClient({ defaultOptions: { queries: { retry: false, gcTime: Infinity } } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); client.clear(); vi.restoreAllMocks(); vi.unstubAllGlobals() })
const button = (name: string) => {
  const found = [...host.querySelectorAll("button")].find((b) => b.textContent?.trim() === name || b.getAttribute("aria-label") === name)
  if (!found) throw new Error(`Missing button: ${name}`)
  return found
}
const lastProfile = () => vi.mocked(api.riskProfile).mock.calls.map(([query]) => query).filter((q) => q.legs).at(-1)!

describe("Analyze tab", { timeout: renderTimeout }, () => {
  it("draws candidates through the risk profile and moves strikes from the leg controls", async () => {
    addAnalyzeCandidate("SPX", { name: "Bull call", units: 2, shares: 0, legs: [
      { symbol: "SPXW  261016C07000000", underlying: "SPX", side: "buy", ratio: 1, type: "call", strike: 7000, expiry: expiry.id, quote: null },
      { symbol: "SPXW  261016C07100000", underlying: "SPX", side: "sell", ratio: 1, type: "call", strike: 7100, expiry: expiry.id, quote: null }] })
    await act(async () => root.render(<QueryClientProvider client={client}><AnalyzeView symbol="SPX" /></QueryClientProvider>))
    await waitForRender(() => expect(host.querySelector('[aria-label="Bull call · Expiry"]')).not.toBeNull())
    const query = lastProfile()
    expect(query).toMatchObject({ underlying: "SPX", legs: { "SPXW  261016C07000000": 2, "SPXW  261016C07100000": -2 }, held: true, iv: 0, range: 10 })
    expect(query.days![0]).toBe(0)
    // The held book alone is drawn beside it, from a query without legs.
    expect(vi.mocked(api.riskProfile).mock.calls.some(([q]) => !q.legs && q.underlying === "SPX")).toBe(true)
    expect(host.querySelector('[aria-label="Drag +7000C"]')).not.toBeNull()
    expect(host.textContent).toContain("Price slices")
    expect(host.textContent).toContain("BE Bull call")
    // Stepping the short strike up asks for the new contract.
    await act(async () => button("Raise strike of 7100 call").click())
    await waitForRender(() => expect(lastProfile().legs).toEqual({ "SPXW  261016C07000000": 2, "SPXW  261016C07150000": -2 }))
    // Excluding the held book values the legs alone.
    await act(async () => host.querySelector<HTMLInputElement>('input[type="checkbox"]')!.click())
    await waitForRender(() => expect(lastProfile().held).toBe(false))
    // Sending to the ticket opens the strategy ticket with the legs.
    await act(async () => button("Send to ticket").click())
    await waitForRender(() => expect(host.textContent).toContain("Strategy order"))
  })
  it("searches the optimizer and analyzes a result", async () => {
    const optimizer = vi.spyOn(api, "optimizer").mockResolvedValue(search)
    await act(async () => root.render(<QueryClientProvider client={client}><AnalyzeView symbol="SPX" /></QueryClientProvider>))
    await waitForRender(() => expect(host.textContent).toContain("Strategy optimizer"))
    await act(async () => button("Search").click())
    await waitForRender(() => expect(host.textContent).toContain("Bull call 7050/7100 Oct 16"))
    expect(optimizer.mock.calls[0]![0]).toBe("SPX")
    expect(optimizer.mock.calls[0]![1]).toMatchObject({ target: 7000, budget: 1000, rank: "expected", price: "natural" })
    expect(host.textContent).toContain("41% / 47%")
    await act(async () => button("Analyze Bull call 7050/7100 Oct 16").click())
    await waitForRender(() => expect(lastProfile()?.legs).toEqual({ "SPXW  261016C07050000": 1, "SPXW  261016C07100000": -1 }))
  })
})

describe("draggable chart markers", () => {
  it("call back with the price under the pointer and step from the keyboard", async () => {
    const onDrag = vi.fn(), onStep = vi.fn()
    await act(async () => root.render(<LineChart series={[{ id: "a", label: "A", color: "red", points: [{ x: 0, y: 0 }, { x: 100, y: 10 }] }]}
      markers={[{ x: 50, label: "50C", color: "blue", onDrag, onStep }]} formatX={String} formatY={String} />))
    const handle = host.querySelector<SVGGElement>('[aria-label="Drag 50C"]')!
    const svg = host.querySelector("svg")!
    vi.spyOn(svg, "getBoundingClientRect").mockReturnValue({ left: 0, top: 0, width: 640, height: 320 } as DOMRect)
    await act(async () => { handle.dispatchEvent(new MouseEvent("pointerdown", { bubbles: true, clientX: 300 })) })
    await act(async () => { svg.dispatchEvent(new MouseEvent("pointermove", { bubbles: true, clientX: 52 + (640 - 52 - 16) * 0.8 })) })
    await act(async () => { svg.dispatchEvent(new MouseEvent("pointerup", { bubbles: true })) })
    // Once released, moving the pointer no longer drags.
    await act(async () => { svg.dispatchEvent(new MouseEvent("pointermove", { bubbles: true, clientX: 100 })) })
    expect(onDrag).toHaveBeenCalledOnce()
    expect(onDrag.mock.calls[0]![0]).toBeCloseTo(80, 6)
    await act(async () => { handle.dispatchEvent(new KeyboardEvent("keydown", { bubbles: true, key: "ArrowLeft" })) })
    expect(onStep).toHaveBeenCalledWith(-1)
  })
})

describe("hedge suggestions", { timeout: renderTimeout }, () => {
  const hedges: HedgeSuggestions = { account_version: "17", time: "2026-09-23T15:30:00Z", scope: "aggregate", target: "delta", utilisation: 1, simulated: true,
    model: "Fixture hedge model",
    current: { dollar_delta_low: 700000, dollar_delta_high: 1750000, vega_low: 2450, vega_high: 6125, worst_cell: -900, beta_weighted_dollar_delta: 700000,
      delta_limit: 5000000, vega_limit: 100000, grid_floor: null },
    hedges: [
      { name: "Beta hedge: SPY shares", kind: "shares", underlying: "SPY", instrument: "SPY", beta_hedge: true, expiry: null, strike: null, type: null, feasible: true,
        reason: null, side: "sell", quantity: 1200, price: 600, premium: -720000, spread_cost: 0, residual: null, order: null,
        stock_trade: { symbol: "SPY", side: "sell", shares: 1200 }, exceeds_order_limit: false },
      { name: "Beta hedge: SPX ATM call", kind: "option", underlying: "SPX", instrument: quote.symbol!, beta_hedge: true, expiry: expiry.id, strike: 7000, type: "call",
        feasible: true, reason: null, side: "sell", quantity: 2, price: 4.5, premium: -900, spread_cost: 10,
        residual: { dollar_delta_low: 0, dollar_delta_high: 1050000, vega_low: -10, vega_high: 3675, worst_cell: -300, delta_utilisation: 0.21, vega_utilisation: 0.04,
          grid_utilisation: null, beta_weighted_dollar_delta: 0 },
        order: { symbol: quote.symbol!, side: "sell", type: "limit", time_in_force: "day", quantity: 2, limit_price: "4.50" }, stock_trade: null, exceeds_order_limit: false },
      { name: "Beta hedge: SPX ATM put", kind: "option", underlying: "SPX", instrument: null, beta_hedge: true, expiry: null, strike: null, type: null, feasible: false,
        reason: "No tradable two-sided at-the-money quote", side: null, quantity: null, price: null, premium: null, spread_cost: null, residual: null, order: null,
        stock_trade: null, exceeds_order_limit: false },
    ] }
  function Store() { return <span data-testid="what-if">{useWhatIf("live:main").candidates.map((c) => c.name).join("|")}</span> }
  it("ranks hedges, adds an option hedge to the what-if and opens its ticket", async () => {
    const request = vi.spyOn(api, "hedges").mockResolvedValue(hedges)
    vi.spyOn(api, "chain").mockResolvedValue(chain)
    await act(async () => root.render(<QueryClientProvider client={client}><HedgePanel risk={risk} trading={trading} /><Store /></QueryClientProvider>))
    await waitForRender(() => expect(host.textContent).toContain("Sell 2 SPX 7000 call (2026-10-16PM)"))
    expect(request).toHaveBeenCalledWith("delta", "aggregate", 1, expect.anything())
    expect(host.textContent).toContain("Sell 1,200 SPY shares")
    expect(host.textContent).toContain("No tradable two-sided at-the-money quote")
    const add = [...host.querySelectorAll("button")].filter((b) => b.textContent === "Add to what-if")
    expect(add).toHaveLength(1)
    await act(async () => add[0]!.click())
    expect(host.querySelector('[data-testid="what-if"]')!.textContent).toBe("Hedge: Beta hedge: SPX ATM call")
    expect(host.textContent).toContain("compare it there, then open its ticket")
    const tickets = [...host.querySelectorAll("button")].filter((b) => b.textContent === "Ticket")
    await act(async () => tickets[1]!.click())
    await waitForRender(() => expect(host.querySelector('input[type="number"][min="1"]')).not.toBeNull())
    await act(async () => tickets[0]!.click())
    await waitForRender(() => expect(host.textContent).toContain("Trade shares"))
  })
})
