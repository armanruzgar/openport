import { renderToStaticMarkup } from "react-dom/server"
import { describe, expect, it } from "vitest"
import { riskProfilePath } from "../api/client"
import type { Probability } from "../api/types"
import type { ProfileCurve, RiskProfile } from "../api/trading-types"
import { parseNumbers } from "../lib/probability"
import { curveLabel, levelText, parseBetas, profileChart } from "../lib/profile"
import { ProbabilityTable } from "./ProbabilityPanel"
import { ProfileResult } from "./RiskProfilePanel"
import { DynamicsNote } from "./ScenarioGrid"

const curve = (days: number, pnl: number[], extra: Partial<ProfileCurve> = {}): ProfileCurve => ({
  days, label: null, time: "2026-09-22T14:00:00Z", pnl, clamped: false,
  horizon: { until: "2026-09-22T20:00:00Z", sigma: 0.01, one_sd: { low: 594, high: 606 }, two_sd: { low: 588.1, high: 612.1 } },
  down: null, up: null, soft_down: null, soft_up: null, ...extra,
})
const profile: RiskProfile = {
  account_version: "7", time: "2026-09-22T14:00:00Z", underlying: null, benchmark: "SPY", reference: "SPY", spot: 600,
  vol_points: 0, equity: "10000.00", room: "1000.00", soft_room: null, percent: [-5, 0, 5], prices: [570, 600, 630],
  curves: [curve(0, [-1200, 0, 900], { down: { percent: -4.2, points: -25.2, price: 574.8, touch_probability: 0.12 } }),
    curve(30.25, [-1500, -80, 700], { label: "expiry", clamped: true })],
  betas: [
    { underlying: "SPX", beta: 1, source: "index", observations: 0, correlation: null, dollar_delta: 2000, weighted_dollar_delta: 2000 },
    { underlying: "QQQ", beta: 1.18, source: "daily", observations: 251, correlation: 0.9, dollar_delta: 1000, weighted_dollar_delta: 1180 },
  ],
  weighted_dollar_delta: 3180, weighted_delta: 5.3, complete: true, model: "Black-76",
}

describe("risk profile", () => {
  it("draws each date against the reference's price with the floor as a loss", () => {
    const chart = profileChart(profile)
    expect(chart.byPrice).toBe(true)
    expect(chart.series.map((s) => s.label)).toEqual(["Today", "Expiry (T+30.3)"])
    expect(chart.series[0]!.points).toEqual([{ x: 570, y: -1200 }, { x: 600, y: 0 }, { x: 630, y: 900 }])
    expect(chart.series[1]!.dashed).toBe(true)
    expect(chart.references).toEqual([{ y: -1000, label: "Plan floor", color: "var(--bearish)" }])
    expect(chart.markers.map((m) => m.x)).toEqual([600, 594, 606])
    // Without a spot the curves run against the percent move, and an incomplete profile draws nothing.
    const bare = profileChart({ ...profile, spot: null, prices: [null, null, null], complete: false })
    expect(bare.byPrice).toBe(false)
    expect(bare.series[0]!.points[0]).toEqual({ x: -5, y: null })
    expect(bare.markers).toEqual([])
  })
  it("tabulates each date's floor levels and the book's beta weighting", () => {
    const html = renderToStaticMarkup(<ProfileResult profile={profile} />)
    expect(html).toContain("574.80 (−4.20%) · 12% touch")
    expect(html).toContain("Expiry (T+30.3) *")
    expect(html).toContain("594.00 – 606.00")
    expect(html).toContain("Daily closes · 251 returns")
    expect(html).toContain("Same index")
    expect(html).toContain("5.3 SPY deltas")
    expect(html).toContain("+3180")
    expect(renderToStaticMarkup(<ProfileResult profile={{ ...profile, complete: false }} />)).toContain("Profile incomplete")
  })
  it("labels curves, levels and parses beta overrides", () => {
    expect(curveLabel({ days: 1, label: null })).toBe("T+1")
    expect(levelText(null)).toBe("—")
    expect(levelText({ percent: 3, points: null, price: null, touch_probability: null })).toBe("+3.00%")
    expect(parseBetas("qqq:1.2, IWM : -0.5")).toEqual({ QQQ: 1.2, IWM: -0.5 })
    expect(parseBetas("")).toEqual({})
    expect(parseBetas("QQQ")).toBeNull()
    expect(parseBetas("QQQ:20")).toBeNull()
  })
  it("asks the server only for what differs from its defaults", () => {
    expect(riskProfilePath({})).toBe("/api/risk/profile")
    expect(riskProfilePath({ benchmark: "SPX", days: [0, 1, "expiry"], iv: 2, range: 5, betas: { QQQ: 1.2 } }))
      .toBe("/api/risk/profile?benchmark=SPX&days=0%2C1%2Cexpiry&iv=2&range=5&betas=QQQ%3A1.2")
    expect(riskProfilePath({ underlying: "SPX", benchmark: "SPY", iv: 0 })).toBe("/api/risk/profile?underlying=SPX")
    // Sticky strike is the server's default, so only other dynamics are sent.
    expect(riskProfilePath({ underlying: "SPX", dynamics: "sticky_strike" })).toBe("/api/risk/profile?underlying=SPX")
    expect(riskProfilePath({ underlying: "SPX", dynamics: "sticky_delta" })).toBe("/api/risk/profile?underlying=SPX&dynamics=sticky_delta")
  })
})

describe("probability cones", () => {
  it("shows each horizon's cone and the odds of ending beyond or touching each price", () => {
    const data: Probability = { symbol: "SPX", as_of: "2026-09-22T14:00:00Z", spot: 6000, model: "Lognormal", horizons: [
      { days: 0, until: "2026-09-22T20:00:00Z", sigma: 0.006, one_sd: { low: 5964, high: 6036 }, two_sd: { low: 5928, high: 6072 },
        prices: [{ price: 6100, above: 0.004, below: 0.996, touch: 0.008 }] },
      { days: 7, until: "2026-09-29T14:00:00Z", sigma: 0.025, one_sd: { low: 5852, high: 6152 }, two_sd: { low: 5707, high: 6307 },
        prices: [{ price: 6100, above: 0.25, below: 0.75, touch: 0.5 }] },
    ] }
    const html = renderToStaticMarkup(<ProbabilityTable data={data} />)
    expect(html).toContain("Today&#x27;s close")
    expect(html).toContain("7 days")
    expect(html).toContain("6100.00 (above)")
    expect(html).toContain("25.0% end")
    expect(html).toContain("50.0% touch")
    expect(html).toContain("6152.00")
  })
  it("reads comma-separated horizons and prices", () => {
    expect(parseNumbers("0, 1, 7, 7", 8, 0, 366)).toEqual([0, 1, 7])
    expect(parseNumbers("", 8, 0, 366)).toEqual([])
    expect(parseNumbers("400", 8, 0, 366)).toBeNull()
    expect(parseNumbers("-1", 8, 0, 366)).toBeNull()
    expect(parseNumbers("1,2,3", 2, 0, 366)).toBeNull()
  })
})

describe("smile dynamics", () => {
  it("names the dynamics and any underlying left sticky strike", () => {
    expect(renderToStaticMarkup(<ProfileResult profile={profile} />)).not.toContain("SSVI surface")
    const moved = renderToStaticMarkup(<ProfileResult profile={{ ...profile, dynamics: "sticky_moneyness", surface_fallback: ["QQQ"] }} />)
    expect(moved).toContain("Sticky moneyness: each smile moves with the forward along its fitted SSVI surface")
    expect(moved).toContain("No surface for QQQ: sticky strike there.")
    const delta = renderToStaticMarkup(<DynamicsNote dynamics="sticky_delta" fallback={[]} />)
    expect(delta).toContain("vol changes apply in delta space")
    expect(delta).not.toContain("No surface")
    expect(renderToStaticMarkup(<DynamicsNote />)).toContain("as the account&#x27;s rules value it")
  })
})

