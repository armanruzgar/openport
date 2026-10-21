import { renderToStaticMarkup } from "react-dom/server"
import { describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import type { CircuitBreaker, Tick } from "../api/types"
import { status } from "../test/trading-fixtures"
import { CircuitBreakerBanner } from "./CircuitBreakerBanner"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))

const breaker: CircuitBreaker = {
  symbol: "SPX", day: "2026-09-22", previous_close: { date: "2026-09-21", price: 5400 }, level: 1,
  market_time: "2026-09-22T14:05:00Z", active: true, error: null,
  halts: [{ level: 1, start: "2026-09-22T14:00:00Z", end: "2026-09-22T14:15:00Z", reference: 5400, price: 5000, active: true }],
}
function render(state: CircuitBreaker | null | undefined, tick: Tick | null = null) {
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, circuit_breaker: state }, tick, "open"))
  return renderToStaticMarkup(<CircuitBreakerBanner />).replaceAll("&amp;", "&")
}

describe("the circuit breaker banner", () => {
  it("describes the recorded fall and resume time from REST, independently of wall time", () => {
    const html = render(breaker)
    expect(html).toContain('role="status"')
    expect(html).toContain("Trading is halted market-wide: the S&P 500 fell 7.4% from its previous close of 5,400.00 (a level 1 circuit breaker).")
    expect(html).toContain("Trading resumes at 10:15 ET.")
    // A revised reference cannot rewrite the print that caused the halt.
    expect(render({ ...breaker, previous_close: { date: "2026-09-21", price: 5500 } })).toContain("5,400.00")
  })

  it("uses Eastern standard time in winter and identifies the SPY proxy", () => {
    const html = render({ ...breaker, symbol: "SPY", halts: [{ ...breaker.halts[0]!, reference: 540, price: 500,
      start: "2026-12-01T15:00:00Z", end: "2026-12-01T15:15:00Z" }] })
    expect(html).toContain("SPY, standing in for the S&P 500, fell 7.4%")
    expect(html).toContain("previous close of 540.00")
    expect(html).toContain("Trading resumes at 10:15 ET.")
  })

  it("uses the highest active level and describes level 3 as the rest of the day", () => {
    const second = { ...breaker.halts[0]!, level: 2, price: 4600, end: "2026-09-22T14:20:00Z" }
    expect(render({ ...breaker, level: 2, halts: [...breaker.halts, second] })).toContain("a level 2 circuit breaker")
    const html = render({ ...breaker, level: 3, halts: [...breaker.halts, { ...second, level: 3, price: 4200 }] })
    expect(html).toContain("a level 3 circuit breaker")
    expect(html).toContain("for the rest of the day.")
    expect(html).not.toContain("resumes at")
  })

  it("identifies authored halts without a fictitious fall and explains missing references", () => {
    const html = render({ ...breaker, level: 0, halts: [{ ...breaker.halts[0]!, level: 0, reference: 0, price: 0 }] })
    expect(html).toContain("halted market-wide by the scenario")
    expect(html).toContain("Trading resumes at 10:15 ET.")
    expect(html).not.toContain("NaN")
    expect(render({ ...breaker, halts: [...breaker.halts,
      { ...breaker.halts[0]!, level: 0, reference: 0, price: 0, end: "2026-09-22T14:30:00Z" }] }))
      .toContain("Trading resumes at 10:30 ET.")
    expect(render({ ...breaker, active: false, previous_close: null, inactive_reason: "MISSING_PREVIOUS_CLOSE" }))
      .toContain("Circuit breaker inactive: waiting for SPX previous close.")
  })

  it("disappears when ticks end a halt and stays hidden for missing or inactive state", () => {
    const tick: Tick = { type: "tick", feed: status.feed, underlyings: status.underlyings, engine: status.engine,
      circuit_breaker: { ...breaker, active: false, halts: [{ ...breaker.halts[0]!, active: false }] } }
    expect(render(breaker, tick)).toBe("")
    expect(render(null)).toBe("")
    expect(render(undefined)).toBe("")
    expect(render({ ...breaker, active: false })).toBe("")
    expect(render({ ...breaker, halts: [] })).toBe("")
    tick.circuit_breaker = breaker
    expect(render(undefined, tick)).toContain("Trading is halted market-wide")
  })

  it("uses the server's rest-of-day decision for authored halts on early-close days", () => {
    const state: CircuitBreaker = { ...breaker, level: 0, day: "2026-11-27", halts: [{ ...breaker.halts[0]!,
      level: 0, start: "2026-11-27T15:00:00Z", end: "2026-11-27T18:00:00Z", reference: 0, price: 0 }] }
    const tick: Tick = { type: "tick", feed: status.feed, engine: status.engine, circuit_breaker: state,
      underlyings: status.underlyings.map((u) => ({ ...u, paper: { accepting: false, reason: "MARKET_HALTED",
        message: "Trading is halted market-wide by the scenario for the rest of the day", session: "regular" } })) }
    const html = render(state, tick)
    expect(html).toContain("halted market-wide by the scenario")
    expect(html).toContain("for the rest of the day.")
    expect(html).not.toContain("resumes at")
    expect(render({ ...state, halts: [{ ...state.halts[0]!, end: "2026-11-27T15:30:00Z" }] }))
      .toContain("Trading resumes at 10:30 ET.")
  })
})

it("names scoped halts without claiming a market-wide closure", () => {
  const html = render({ ...breaker, level: 0, halts: [{ ...breaker.halts[0]!, level: 0, underlying: "SPY", simulated: true }] })
  expect(html).toContain("Simulated single-name halt: SPY")
  expect(html).toContain("Other underlyings are not halted")
  expect(html).not.toContain("halted market-wide")
})
