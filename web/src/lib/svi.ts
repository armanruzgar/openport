import type { SabrFit, SurfaceExpiry, SviFit, SsviFit, SurfaceHistory } from "../api/types"
import type { Series } from "../charts/LineChart"
import { days, expiryLabel, isNum } from "./format"

interface DisplayRange { spot: number | null; window: number }

export type SmileMode = "market" | "svi" | "ssvi" | "all"
export const smileColor = (i: number) => `var(--chart-${i % 8 + 1})`

export function sviVol(p: SviFit, k: number, years: number): number | null {
  if (!(years > 0) || !Number.isFinite(years) || !(p.sigma > 0) || p.b < 0 || Math.abs(p.rho) >= 1) return null
  const x = k - p.m
  const w = p.a + p.b * (p.rho * x + Math.hypot(x, p.sigma))
  return w >= 0 && Number.isFinite(w) ? Math.sqrt(w / years) : null
}

export function ssviVol(p: SsviFit, k: number, theta: number, years: number): number | null {
  if (p.status !== "ok" || !isNum(p.rho) || !isNum(p.eta) || !isNum(p.gamma) ||
    Math.abs(p.rho) >= 1 || p.eta <= 0 || p.gamma <= 0 || p.gamma > .5 ||
    p.eta * (1 + Math.abs(p.rho)) > 2 + 1e-12 || !(theta > 0) || !(years > 0)) return null
  const phi = p.eta / (theta ** p.gamma * (1 + theta) ** (1 - p.gamma))
  const z = phi * k
  const w = theta / 2 * (1 + p.rho * z + Math.hypot(z + p.rho, Math.sqrt(1 - p.rho ** 2)))
  const iv = Math.sqrt(w / years)
  return Number.isFinite(iv) ? iv : null
}

/** SSVI is safe beyond quoted strikes; sample the whole requested window. */
export function sampleSsvi(e: SurfaceExpiry, p: SsviFit | undefined, axis: "strike" | "moneyness", display?: DisplayRange): Series["points"] {
  if (!p || p.status !== "ok" || !isNum(e.ssvi_theta) || !isNum(e.forward) || e.forward <= 0) return []
  const years = e.svi_years ?? (e.days ?? 0) / 365
  if (ssviVol(p, 0, e.ssvi_theta, years) == null) return []
  const ks = e.points.map((point) => point.k).filter(isNum)
  const hasWindow = isNum(display?.spot) && display.spot > 0 && display.window > 0 && display.window < 1
  const lo = hasWindow ? Math.log(display.spot! * (1 - display.window) / e.forward) : e.ssvi_min_k ?? Math.min(...ks)
  const hi = hasWindow ? Math.log(display.spot! * (1 + display.window) / e.forward) : e.ssvi_max_k ?? Math.max(...ks)
  if (!Number.isFinite(lo) || !Number.isFinite(hi) || !(hi > lo)) return []
  return Array.from({ length: 241 }, (_, i) => {
    const k = lo + (hi - lo) * i / 240
    return { x: axis === "strike" ? e.forward! * Math.exp(k) : k, y: ssviVol(p, k, e.ssvi_theta!, years) }
  })
}

/** Hagan et al. (2002) lognormal SABR IV at k = ln(K/F), as the server computes it. */
export function sabrVol(p: SabrFit, forward: number, k: number, years: number): number | null {
  if (!(p.alpha > 0) || !(p.beta >= 0 && p.beta <= 1) || !(Math.abs(p.rho) < 1) || !(p.nu >= 0) ||
    !(forward > 0) || !(years > 0) || !Number.isFinite(k)) return null
  const a = p.alpha * forward ** (p.beta - 1)
  const omb = 1 - p.beta
  const shift = Math.exp(k * omb / 2)
  const lead = a / (shift * (1 + omb ** 2 / 24 * k ** 2 + omb ** 4 / 1920 * k ** 4))
  const z = -p.nu / a * shift * k
  let ratio = 1
  if (Math.abs(z) < 1e-6) ratio = 1 - p.rho * z / 2 + (2 - 3 * p.rho ** 2) * z * z / 12
  else {
    const argument = (Math.sqrt(1 - 2 * p.rho * z + z * z) + z - p.rho) / (1 - p.rho)
    if (!(argument > 0)) return null
    ratio = z / Math.log(argument)
  }
  const correction = 1 + (omb ** 2 / 24 * a * a / (shift * shift) + p.rho * p.beta * p.nu * a / (4 * shift) +
    (2 - 3 * p.rho ** 2) / 24 * p.nu ** 2) * years
  const iv = lead * ratio * correction
  return iv > 0 && Number.isFinite(iv) ? iv : null
}

/** Hagan's expansion is not safe far from its quotes: clip to the window and calibration range. */
export function sampleSabr(e: SurfaceExpiry, axis: "strike" | "moneyness", display?: DisplayRange): Series["points"] {
  const fit = e.sabr
  if (!fit || fit.status !== "ok" || !isNum(e.forward) || e.forward <= 0) return []
  const ks = e.points.map((p) => p.k).filter(isNum)
  const hasWindow = isNum(display?.spot) && display.spot > 0 && display.window > 0 && display.window < 1
  if (!hasWindow && ks.length < 2) return []
  const lo = Math.max(hasWindow ? Math.log(display.spot! * (1 - display.window) / e.forward) : Math.min(...ks), fit.min_k ?? -Infinity)
  const hi = Math.min(hasWindow ? Math.log(display.spot! * (1 + display.window) / e.forward) : Math.max(...ks), fit.max_k ?? Infinity)
  const years = e.svi_years ?? (e.days ?? 0) / 365
  if (!(hi > lo) || !(years > 0)) return []
  return Array.from({ length: 241 }, (_, i) => {
    const k = lo + (hi - lo) * i / 240
    return { x: axis === "strike" ? e.forward! * Math.exp(k) : k, y: sabrVol(fit, e.forward!, k, years) }
  })
}

/** Uniform in k, clipped to the selected window and calibration range.
 * Sampling is independent of quoted strike spacing, including on a strike axis. */
export function sampleSvi(e: SurfaceExpiry, axis: "strike" | "moneyness", display?: DisplayRange): Series["points"] {
  if (!e.svi || e.svi.status !== "ok" || !isNum(e.forward) || e.forward <= 0) return []
  const ks = e.points.map((p) => p.k).filter(isNum)
  const hasWindow = isNum(display?.spot) && display.spot > 0 && display.window > 0 && display.window < 1
  if (!hasWindow && ks.length < 2 && (!isNum(e.svi_min_k) || !isNum(e.svi_max_k))) return []
  const windowLo = hasWindow ? Math.log(display.spot! * (1 - display.window) / e.forward) : ks.length >= 2 ? Math.min(...ks) : e.svi_min_k!
  const windowHi = hasWindow ? Math.log(display.spot! * (1 + display.window) / e.forward) : ks.length >= 2 ? Math.max(...ks) : e.svi_max_k!
  const lo = Math.max(windowLo, e.svi_min_k ?? -Infinity)
  const hi = Math.min(windowHi, e.svi_max_k ?? Infinity)
  if (!(hi > lo)) return []
  const years = e.svi_years ?? (e.days ?? 0) / 365
  if (!(years > 0) || !Number.isFinite(years)) return []
  return Array.from({ length: 241 }, (_, i) => {
    const k = lo + (hi - lo) * i / 240
    return { x: axis === "strike" ? e.forward! * Math.exp(k) : k, y: sviVol(e.svi!, k, years) }
  })
}

export const comparisonLabels = { prior_close: "Prior close", week: "1 week ago", month: "1 month ago" }
/** Same listed expiry, evaluated at each snapshot's own tenor and forward. */
export function comparisonSeries(current: SurfaceExpiry | undefined, history: SurfaceHistory | undefined,
  axis: "strike" | "moneyness", display?: DisplayRange): Series[] {
  if (!current) return []
  const curves: Series[] = [{ id: "comparison-today", label: "Current · SVI", color: smileColor(0), points: sampleSvi(current, axis, display) }]
  for (const [i, comparison] of (history?.comparisons ?? []).entries()) {
    const expiry = comparison.snapshot?.expiries.find(e => e.id === current.id)
    if (!expiry) continue
    const points = sampleSvi(expiry, axis, display)
    if (points.length) curves.push({ id: `comparison-${comparison.label}`, label: `${comparisonLabels[comparison.label]} · ${comparison.date}${!expiry.calendar_ok || !expiry.svi?.butterfly_ok ? " · grid violation" : ""}`, color: smileColor(i + 1), dashed: true, points })
  }
  return curves
}

/** SABR is an overlay on any mode: dotted, drawn only for expiries that carry a fit. */
export function smileSeries(expiries: SurfaceExpiry[], axis: "strike" | "moneyness", mode: SmileMode, display?: DisplayRange, ssvi?: SsviFit, sabr = false): Series[] {
  return expiries.flatMap((e, i) => {
    const base = { label: `${expiryLabel(e.id)} · ${days(e.days)}`, color: smileColor(i) }
    const series: Series[] = []
    if (mode === "market" || mode === "all") series.push({ ...base, id: `${e.id}-market`, label: `${base.label} market`, dots: true, band: i === 0,
      points: e.points.filter((p) => axis === "strike" || isNum(p.k)).map((p) => ({
        x: axis === "strike" ? p.strike : p.k!, y: p.iv, lo: p.bid_iv, hi: p.ask_iv,
      })) })
    if (mode === "svi" || mode === "all") {
      const points = sampleSvi(e, axis, display)
      if (points.length) series.push({ ...base, id: `${e.id}-svi`, label: `${base.label} SVI`, points })
    }
    if (mode === "ssvi" || mode === "all") {
      const points = sampleSsvi(e, ssvi, axis, display)
      if (points.length) series.push({ ...base, id: `${e.id}-ssvi`, label: `${base.label} SSVI`, dashed: true, points })
    }
    if (sabr) {
      const points = sampleSabr(e, axis, display)
      if (points.length) series.push({ ...base, id: `${e.id}-sabr`, label: `${base.label} SABR`, dotted: true, points })
    }
    return series
  })
}
