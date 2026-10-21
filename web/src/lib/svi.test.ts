import { describe, expect, it } from "vitest"
import type { SurfaceExpiry, SviFit, SsviFit } from "../api/types"
import { comparisonSeries, sampleSvi, sampleSsvi, smileSeries, sviVol, ssviVol } from "./svi"

const fit: SviFit = { a: .02, b: .1, rho: -.4, m: .02, sigma: .15, rmse_vol_points: .01,
  points: 50, status: "ok", reason: null, fit_ms: 1, butterfly_min_g: .3, butterfly_k: .2, butterfly_ok: true }
const expiry: SurfaceExpiry = { id: "2027-01-15PM", expiry: "2027-01-15", days: 182.5, forward: 5000,
  atm_iv: .2, svi: fit, svi_years: .5, svi_min_k: -.3, svi_max_k: .3,
  points: [-.2, -.05, .2].map((k) => ({ k, strike: 5000 * Math.exp(k), iv: .2, bid_iv: .19, ask_iv: .21 })) }

describe("SVI curves", () => {
  it("evaluates total variance in the correct units and fails closed", () => {
    expect(sviVol(fit, fit.m, .5)).toBeCloseTo(Math.sqrt((fit.a + fit.b * fit.sigma) / .5), 14)
    expect(sviVol(fit, 0, 0)).toBeNull()
    expect(sviVol({ ...fit, a: -1 }, 0, 1)).toBeNull()
    expect(sviVol({ ...fit, sigma: 0 }, 0, 1)).toBeNull()
  })
  it("samples densely and uniformly in k, using the full precision tenor", () => {
    const points = sampleSvi({ ...expiry, days: 180 }, "moneyness")
    expect(points).toHaveLength(241)
    expect(points[0]!.x).toBeCloseTo(-.2)
    expect(points[240]!.x).toBeCloseTo(.2)
    expect(points[120]!.x).toBeCloseTo(0)
    expect(points[120]!.y).toBe(sviVol(fit, 0, .5))
    const strikes = sampleSvi(expiry, "strike")
    expect(strikes[120]!.x).toBeCloseTo(5000)
    expect(strikes[120]!.x - strikes[119]!.x).not.toBeCloseTo(strikes[1]!.x - strikes[0]!.x)
  })
  it("does not extrapolate beyond calibration or invent curves for unavailable fits", () => {
    expect(sampleSvi({ ...expiry, svi: null }, "strike")).toEqual([])
    expect(sampleSvi({ ...expiry, forward: null }, "strike")).toEqual([])
    const limited = sampleSvi({ ...expiry, svi_min_k: -.1, svi_max_k: .1 }, "moneyness")
    expect(limited[0]!.x).toBe(-.1)
    expect(limited[240]!.x).toBe(.1)
  })
  it("draws the fitted curve in the requested window even without displayable market quotes", () => {
    const points = sampleSvi({ ...expiry, points: [] }, "moneyness", { spot: 5000, window: .1 })
    expect(points).toHaveLength(241)
    expect(points[0]!.x).toBeCloseTo(Math.log(.9))
    expect(points[240]!.x).toBeCloseTo(Math.log(1.1))
  })
  it("selects market dots, SVI lines, or both with stable expiry colors", () => {
    const both = smileSeries([expiry], "moneyness", "all")
    expect(both).toHaveLength(2)
    expect(both[0]!.dots).toBe(true)
    expect(both[0]!.band).toBe(true)
    expect(both[1]!.dots).toBeUndefined()
    expect(both[1]!.points.length).toBe(241)
    expect(both[0]!.color).toBe(both[1]!.color)
    expect(smileSeries([expiry], "strike", "market").map((s) => s.id)).toEqual([`${expiry.id}-market`])
    expect(smileSeries([expiry], "strike", "svi").map((s) => s.id)).toEqual([`${expiry.id}-svi`])
  })
})

const ssvi: SsviFit = { rho: -.6, eta: .8, gamma: .3, status: "ok", reason: null, monotone_adjusted: false, rmse_vol_points: .2, fit_ms: 8 }
const ssviExpiry = { ...expiry, ssvi_theta: .02, ssvi_rmse_vol_points: .15, ssvi_min_k: -.3, ssvi_max_k: .3 }
describe("SSVI surface curves", () => {
  it("recovers ATM total variance and samples densely even outside quotes", () => {
    expect(ssviVol(ssvi, 0, .02, .5)).toBeCloseTo(.2, 14)
    const display = { spot: 5000, window: .5 }
    const points = sampleSsvi(ssviExpiry, ssvi, "moneyness", display)
    expect(points).toHaveLength(241)
    expect(points[0]!.x).toBeCloseTo(Math.log(.5))
    expect(points[240]!.x).toBeCloseTo(Math.log(1.5))
    expect(points[120]!.x).toBeCloseTo((Math.log(.5) + Math.log(1.5)) / 2)
    const strikes = sampleSsvi({ ...ssviExpiry, points: [] }, ssvi, "strike", display)
    expect(strikes).toHaveLength(241)
    expect(strikes[0]!.x).toBeCloseTo(2500)
    expect(strikes[240]!.x).toBeCloseTo(7500)
    expect(points.every((p) => p.y != null && Number.isFinite(p.y))).toBe(true)
  })
  it("does not invent estimates for failed surfaces or omitted expiries", () => {
    expect(sampleSsvi(expiry, ssvi, "strike")).toEqual([])
    expect(sampleSsvi(ssviExpiry, { ...ssvi, status: "failed" }, "strike")).toEqual([])
    expect(ssviVol({ ...ssvi, gamma: .6 }, 0, .02, .5)).toBeNull()
    expect(ssviVol(ssvi, 0, 0, .5)).toBeNull()
  })
  it("selects all four modes and distinguishes curves with dashes and stable colors", () => {
    const all = smileSeries([ssviExpiry], "moneyness", "all", undefined, ssvi)
    expect(all.map((s) => s.id)).toEqual([`${expiry.id}-market`, `${expiry.id}-svi`, `${expiry.id}-ssvi`])
    expect(all[2]!.dashed).toBe(true)
    expect(all[2]!.color).toBe(all[1]!.color)
    for (const mode of ["market", "svi", "ssvi"] as const) {
      expect(smileSeries([ssviExpiry], "strike", mode, undefined, ssvi).map((s) => s.id)).toEqual([`${expiry.id}-${mode}`])
    }
  })
})

it("samples stored curves at their original tenor/forward and matches exact expiry ids", () => {
  const stored = { ...expiry, forward: 5100, svi_years: 1, points: [], calendar_ok: false }
  const history = { symbol: "SPX", enabled: true, snapshots: [], comparisons: [
    { label: "prior_close" as const, date: "2026-09-21", snapshot: { as_of: "2026-09-21T19:59:00Z", date: "2026-09-21", slot: "close", spot: 5100, model: "svi" as const, expiries: [stored] } },
    { label: "week" as const, date: "2026-09-15", snapshot: null },
  ] }
  const curves = comparisonSeries(expiry, history, "strike")
  expect(curves).toHaveLength(2)
  expect(curves[1]!.points[120]!.x).toBeCloseTo(5100)
  expect(curves[1]!.points[120]!.y).toBe(sviVol(fit, 0, 1))
  expect(curves[1]!.label).toContain("grid violation")
  expect(comparisonSeries({ ...expiry, id: "different" }, history, "strike")).toHaveLength(1)
})
