import { describe, expect, it } from "vitest"
import type { SabrFit, SurfaceExpiry, SviFit, SsviFit } from "../api/types"
import { sabrVol, sampleSabr, sampleSvi, sampleSsvi, smileSeries, sviVol, ssviVol } from "./svi"

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

const sabr: SabrFit = { alpha: .6, beta: .5, rho: -.3, nu: .8, beta_calibrated: false, rmse_vol_points: .01, points: 40,
  status: "ok", reason: null, fit_ms: 1, butterfly_min_g: .2, butterfly_k: 0, butterfly_ok: true, min_k: -.15, max_k: .1 }

describe("SABR overlay", () => {
  it("matches the server's Hagan expansion, including the ATM series branch", () => {
    // Values from analytics::sabr_iv for the same parameters, F = 100, T = 1.5.
    expect(sabrVol(sabr, 100, -.2, 1.5)).toBeCloseTo(0.11366372017678149, 13)
    expect(sabrVol(sabr, 100, 0, 1.5)).toBeCloseTo(0.063993375000000005, 13)
    expect(sabrVol(sabr, 100, 1e-8, 1.5)).toBeCloseTo(0.063993373560537345, 13)
    expect(sabrVol(sabr, 100, .15, 1.5)).toBeCloseTo(0.075730969610391954, 13)
    expect(sabrVol({ ...sabr, beta: 1, nu: 0, alpha: .2 }, 100, -.3, 1)).toBeCloseTo(.2, 14)
    expect(sabrVol({ ...sabr, alpha: -1 }, 100, 0, 1)).toBeNull()
    expect(sabrVol({ ...sabr, rho: 1 }, 100, 0, 1)).toBeNull()
    expect(sabrVol(sabr, 100, 0, 0)).toBeNull()
  })
  it("draws only inside the quoted range and the window, and only when asked", () => {
    const withSabr = { ...expiry, sabr }
    const points = sampleSabr(withSabr, "moneyness", { spot: 5000, window: .2 })
    expect(points).toHaveLength(241)
    expect(points[0]!.x).toBe(-.15)
    expect(points[240]!.x).toBe(.1)
    expect(sampleSabr({ ...withSabr, sabr: null }, "moneyness")).toEqual([])
    expect(sampleSabr({ ...withSabr, forward: null }, "moneyness")).toEqual([])
    const off = smileSeries([withSabr], "moneyness", "market")
    expect(off.some((s) => s.id.endsWith("-sabr"))).toBe(false)
    const on = smileSeries([withSabr], "moneyness", "market", undefined, undefined, true)
    const curve = on.find((s) => s.id.endsWith("-sabr"))!
    expect(curve.dotted).toBe(true)
    expect(curve.label).toContain("SABR")
  })
})

