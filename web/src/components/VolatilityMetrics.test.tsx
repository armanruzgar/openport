import { renderToStaticMarkup } from "react-dom/server"
import { expect, it } from "vitest"
import type { ConstantVol, Volatility } from "../api/types"
import { VolatilityDetails, VolatilitySummary } from "./VolatilityMetrics"

/** Each summary stat's value by its label. The footnote under the stats names both marks, so marks are read here. */
function stats(html: string): Record<string, string> {
  return Object.fromEntries([...html.matchAll(/<div[^>]*>([^<]*)<\/div><div[^>]*>([^<]*)<\/div><\/div>/g)].map((match) => [match[1]!, match[2]!]))
}
const constant = (days: number, vol: number, flags: Partial<ConstantVol> = {}): ConstantVol =>
  ({ days, vol, variance: (vol / 100) ** 2, near: null, next: null, truncated: false, proxy: false, reason: null, ...flags })

const estimate = { vol: null, fallback: false, reason: "insufficient_history" }
const point = { vol: null, strike: null, reason: "delta_outside_smile" }
const skew = { call: point, put: point, rr: null, bf: null }
const data: Volatility = {
  symbol: "SPX", as_of: "2026-09-25T16:00:00Z", version: 1, spot: 100, forward: 100, units: "vol_points",
  mfiv: { constant: [{ days: 30, vol: 20, variance: .04, near: "2026-10-16PM", next: "2026-10-30PM", truncated: true, proxy: false, reason: null }], expiries: [] },
  atm: { constant: [], expiries: [] },
  skew: { delta_convention: "Black-76 forward, premium-unadjusted, undiscounted", expiries: [{ id: "2026-10-16PM", source: "smile_interpolation", delta25: skew, delta10: skew, slope: null, curvature: null }], days: 30, delta25: skew, delta10: skew, proxy: true, reason: "unusable_bracketing_expiry" },
  term: { mfiv9_30: null, mfiv30_93: null, atm30_7: null, truncated: true, proxy: true },
  realized: { daily_as_of: "2026-09-24", windows: [{ sessions: 21, close_to_close: estimate, parkinson: { vol: 18, fallback: true, reason: "invalid_ohlc_used_close_to_close" }, garman_klass: estimate, yang_zhang: estimate }], cones: [], intraday: [], today: null },
  vrp: { spread: null, ratio: null, truncated: true, proxy: false, reason: "missing_implied_or_realized" },
  implied_moves: { intervals: [], sessions: [{ date: "2026-09-28", label: "User <event>", points: null, percent: null, forward: 100, calendar_arbitrage: true, shared: true, proxy: false, truncated: true, reason: "negative_forward_variance" }], today_points: null, today_percent: null, today_reason: "no_same_day_expiry", today_proxy: false, today_truncated: false, today_calendar_arbitrage: false },
  sources: { mfiv: "Quoted strip", atm: "Smile", skew: "Forward delta", term: "Total variance", realized: "Completed daily bars", vrp: "Ex ante", implied_moves: "Shared business sessions" },
}
it("renders point units, missing markers and visible proxy/truncation explanations", () => {
  const html = renderToStaticMarkup(<VolatilitySummary data={data} />)
  // Skew is a proxy and the VRP and model-free strips truncated: each mark sits on its own stat.
  expect(stats(html)).toEqual({
    "Model-free IV · 30d": "20.00 †", "ATM IV · 30d": "—", "25Δ RR / BF · 30d": "— / — ≈", "Close RV · 21 sessions": "—",
    "VRP · spread / ratio": "— / — †", "Model-free IV · 9d/30d": "— †", "Model-free IV · 30d/93d": "— †", "ATM IV · 30d − 7d": "—",
  })
  expect(html).toContain("Truncated strike strip")
  expect(html).not.toContain("2000.00")
  expect(html).not.toContain("NaN")
})
it("marks only the stats whose own inputs are truncated or proxies", () => {
  const clean: Volatility = { ...data,
    mfiv: { ...data.mfiv, constant: [constant(9, 18), constant(30, 20), constant(93, 22)] },
    atm: { ...data.atm, constant: [constant(7, 17), constant(30, 19)] },
    skew: { ...data.skew, proxy: false, reason: null, delta25: { ...skew, rr: -4, bf: 1 } },
    term: { mfiv9_30: .9, mfiv30_93: .909, atm30_7: 2, truncated: false, proxy: false },
    vrp: { spread: 3, ratio: 1.2, truncated: false, proxy: false, reason: null },
  }
  expect(stats(renderToStaticMarkup(<VolatilitySummary data={clean} />))).toEqual({
    "Model-free IV · 30d": "20.00", "ATM IV · 30d": "19.00", "25Δ RR / BF · 30d": "-4.00 / 1.00", "Close RV · 21 sessions": "—",
    "VRP · spread / ratio": "3.00 / 1.20", "Model-free IV · 9d/30d": "0.900", "Model-free IV · 30d/93d": "0.909", "ATM IV · 30d − 7d": "2.00",
  })
  // A term ratio carries the marks of either tenor it divides; the 30-day stats keep their own.
  const legs: Volatility = { ...clean,
    mfiv: { ...clean.mfiv, constant: [constant(9, 18, { proxy: true }), constant(30, 20), constant(93, 22, { truncated: true })] },
    atm: { ...clean.atm, constant: [constant(7, 17, { proxy: true }), constant(30, 19)] },
  }
  expect(stats(renderToStaticMarkup(<VolatilitySummary data={legs} />))).toEqual({
    "Model-free IV · 30d": "20.00", "ATM IV · 30d": "19.00", "25Δ RR / BF · 30d": "-4.00 / 1.00", "Close RV · 21 sessions": "—",
    "VRP · spread / ratio": "3.00 / 1.20", "Model-free IV · 9d/30d": "0.900 ≈", "Model-free IV · 30d/93d": "0.909 †", "ATM IV · 30d − 7d": "2.00 ≈",
  })
})
it("shows fallback reasons, arbitrage flags and escaped event labels in scrollable tables", () => {
  const html = renderToStaticMarkup(<VolatilityDetails data={data} />)
  expect(html).toContain("invalid_ohlc_used_close_to_close")
  expect(html).toContain("18.00 ≈")
  expect(html).toContain("2026-09-24")
  expect(html).toContain("Calendar arbitrage")
  expect(html).toContain("User &lt;event&gt;")
  expect(html).toContain("premium-unadjusted, undiscounted")
  expect(html).toContain("≈ smile")
  expect(html).toContain("overflow-auto")
  expect(html).not.toContain("NaN")
})

it("shows derived event premiums beside their labels and keeps unavailable moves missing", () => {
  const event = { date: "2026-09-25", session: "2026-09-28", label: "Earnings · AMC", timing: "after_close", time: null,
    near: "2026-09-25PM", next: "2026-09-28AM", baseline_variance: .0001, variance_premium: .0004,
    percent: 2, points: 2, forward: 100, proxy: false, truncated: false, calendar_arbitrage: false, reason: null }
  const html = renderToStaticMarkup(<VolatilityDetails data={{ ...data, event_volatility: [event, { ...event, label: "Missing", percent: null, points: null, reason: "event_not_bracketed" }] }} />)
  for (const text of ["Earnings · AMC", "2.00%", "move session 2026-09-28", "event not bracketed", "constant business-session baseline", "—"]) expect(html).toContain(text)
})
