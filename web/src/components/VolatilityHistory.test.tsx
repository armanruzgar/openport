import { renderToStaticMarkup } from "react-dom/server"
import { describe, expect, it } from "vitest"
import type { Volatility, VolatilitySeries } from "../api/types"
import { historyLines, IvHistoryBadge, VolatilityHistory } from "./VolatilityHistory"

const history: VolatilitySeries = { symbol: "SPX", interval: "1d", fields: ["mfiv30", "atm30", "proxy_iv30", "rv21", "rr25"], rows: [
  { t: 1, mfiv30: null, atm30: null, proxy_iv30: 20, rv21: 18, rr25: null, sources: { proxy_iv30: "VIX" } },
  { t: 2, mfiv30: 22, atm30: 21, proxy_iv30: null, rv21: null, rr25: -3, sources: { mfiv30: "own" } },
  { t: 3, mfiv30: null, atm30: null, proxy_iv30: null, rv21: 0, rr25: 0, sources: {} },
] }
const data = {
  iv_rank: .25, iv_percentile: .4, history_sessions: 100,
  history_basis: { current: "own_atm", own_sessions: 10, proxy_sessions: 90, window: 252 },
  proxy: { name: "VIX", used: true, start: "2026-01-02" },
  skew: { delta_convention: "Forward delta" },
  ex_post_vrp: { proxy: true, implied_source: "SPX model-free IV or VIX proxy", realized_source: "SPX daily closes",
    convention: "Overlapping 21-session months", points: [], summaries: [
      { years: 1, observations: 0, proxy_observations: 0, from: "", to: "", mean_variance: null, median_variance: null, mean_vol_points: null, median_vol_points: null, positive_share: null },
      { years: 3, observations: 5, proxy_observations: 5, from: "2025-01-02", to: "2026-01-02", mean_variance: .01, median_variance: .012, mean_vol_points: 2, median_vol_points: 3, positive_share: .6 },
    ] },
} as unknown as Volatility

describe("volatility history", () => {
  it("shows rank basis, sample size and named proxy start without rescaling vol", () => {
    const html = renderToStaticMarkup(<IvHistoryBadge data={data} />)
    for (const text of ["IV rank 25.0%", "Percentile 40.0%", "100", "of 252 sessions", "ATM 30d fallback", "90 VIX proxy sessions", "2026-01-02"]) expect(html).toContain(text)
  })
  it("keeps proxy and own data in separate lines and missing RV apart from zero", () => {
    const lines = historyLines(history)
    expect(lines[0]?.points.map(p => p.y)).toEqual([null, 22, null])
    expect(lines[1]?.points.map(p => p.y)).toEqual([null, 21, null])
    expect(lines[2]?.points.map(p => p.y)).toEqual([18, null, 0])
    expect(lines[3]?.points.map(p => p.y)).toEqual([20, null, null])
    expect(lines[3]?.dashed).toBe(true)
  })
  it("labels SPY's SPX proxy and overlapping months and preserves missing summaries", () => {
    const html = renderToStaticMarkup(<VolatilityHistory data={data} history={history} />)
    for (const text of ["SPX proxy for SPY", "Overlapping 21-session months", "SPX daily closes", "Mean variance", "Mean IV − RV (vp)", "60.0%", "0.01000", "—"]) expect(html).toContain(text)
    expect(html).not.toContain("NaN")
  })
  it("handles unavailable history and older server responses", () => {
    const older = { skew: data.skew } as unknown as Volatility
    const badge = renderToStaticMarkup(<IvHistoryBadge data={older} />)
    expect(badge).toContain("IV rank —")
    expect(badge).toContain("Current IV unavailable")
    const html = renderToStaticMarkup(<VolatilityHistory data={older} error="History unavailable" />)
    expect(html).toContain("History unavailable")
    expect(html).not.toContain("Ex-post variance risk premium")
  })
  it("shows signed skew ranks and a measured-only tracking line", () => {
    const ranked = { ...data, symbol: "SPX", history_ranks: { rr25: { current: -3, rank: -.5, percentile: 0, z_score: -2, sessions: 5, window: 252 } } }
    const measured = { ...history, rows: [{ ...history.rows[1]!, vix: 24, vix_minus_mfiv: 2 }] }
    const html = renderToStaticMarkup(<VolatilityHistory data={ranked} history={measured} />)
    for (const text of ["Skew and term history ranks", "-50.0%", "-2.00", "z-score", "VIX − SPX", "quotes need not be simultaneous"]) expect(html).toContain(text)
    expect(renderToStaticMarkup(<VolatilityHistory data={data} history={history} />)).not.toContain("VIX − SPX")
  })
})
