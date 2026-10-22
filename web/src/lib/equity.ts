import type { EquitySample } from "../api/trading-types"
import type { Marker, Series } from "../charts/LineChart"

/** No interpolation across missing minutes, attempt changes or restarts. */
export function intradayEquity(samples: readonly EquitySample[], day: string, attempt?: number): { series: Series[]; markers: Marker[] } {
  const rows = samples.filter((s) => s.day === day && (attempt == null || s.attempt === attempt) && Number.isFinite(Date.parse(s.time))).sort((a, b) => Date.parse(a.time) - Date.parse(b.time))
  const definitions = [
    ["equity", "Equity", "var(--chart-1)"], ["floor", "Drawdown floor", "var(--bearish)"],
    ["target", "Profit target", "var(--bullish)"], ["tomorrow_floor", "Tomorrow's floor if closed now", "var(--warn)"],
  ] as const
  const series: Series[] = definitions.map(([key, label, color]) => {
    const points: Series["points"] = []
    rows.forEach((row, i) => {
      const x = Date.parse(row.time), previous = rows[i - 1]
      const gap = previous && (x - Date.parse(previous.time) > 90_000 || previous.attempt !== row.attempt)
      if (gap) points.push({ x: x - 1, y: null })
      // A floor or target changes at the recorded instant, not gradually over
      // the preceding minute; the old level ends just before it, so the readout
      // at that instant shows the new one. Keep equity and the projected EOD floor continuous.
      if (!gap && previous && (key === "floor" || key === "target") && previous[key] != null && previous[key] !== row[key]) {
        const prior = Number(previous[key])
        if (Number.isFinite(prior)) points.push({ x: x - 1, y: prior })
      }
      const value = row[key] == null ? null : Number(row[key])
      points.push({ x, y: value != null && Number.isFinite(value) ? value : null })
    })
    // Equity marks each sample; the plan's levels are dashed lines.
    return { id: key, label, color, points, dashed: key !== "equity", dots: key === "equity" }
  }).filter((s) => s.points.some((p) => p.y != null))
  const markers = rows.flatMap((row, i) => {
    const prior = rows[i - 1]
    return prior?.floor != null && row.floor != null && row.attempt === prior.attempt && Number(row.floor) > Number(prior.floor)
      ? [{ x: Date.parse(row.time), label: "Floor raised", color: "var(--bearish)" }] : []
  })
  return { series, markers }
}

/** Observed Greeks only; unavailable valuations and gaps break the line. */
export function riskHistory(samples: readonly EquitySample[], metric: keyof import("../api/trading-types").EquityRisk, day?: string): Series[] {
  const rows = samples.filter((s) => (!day || s.day === day) && Number.isFinite(Date.parse(s.time)))
    .sort((a, b) => Date.parse(a.time) - Date.parse(b.time))
  const points: Series["points"] = []
  rows.forEach((row, i) => {
    const x = Date.parse(row.time), previous = rows[i - 1]
    if (previous && (x - Date.parse(previous.time) > 90_000 || previous.attempt !== row.attempt)) points.push({ x: x - 1, y: null })
    const value = row.risk?.[metric]
    points.push({ x, y: value != null && Number.isFinite(value) ? value : null })
  })
  return points.some((p) => p.y != null) ? [{ id: metric, label: metric.replaceAll("_", " "), color: "var(--chart-1)", points, dots: true }] : []
}
