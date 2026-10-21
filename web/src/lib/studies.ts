import type { Candle } from "../api/types"
import { barDay } from "./candles"

export interface Studies { sma: boolean; ema: boolean; bollinger: boolean; vwap: boolean; period: number }
export const defaultStudies: Studies = { sma: false, ema: false, bollinger: false, vwap: false, period: 20 }
export function studyPrefs(value: Partial<Studies> | null | undefined): Studies {
  return { sma: value?.sma === true, ema: value?.ema === true, bollinger: value?.bollinger === true, vwap: value?.vwap === true,
    period: Number.isInteger(value?.period) && value!.period! >= 2 && value!.period! <= 200 ? value!.period! : 20 }
}
export interface StudyPoint { sma: number | null; ema: number | null; upper: number | null; lower: number | null; vwap: number | null }
/** SMA-seeded EMA; population standard deviation bands at ±2σ. VWAP resets by
 * New York date and is unavailable for a session with missing bar volume. */
export function indicators(bars: readonly (Candle & { v?: number | null })[], requestedPeriod = 20): StudyPoint[] {
  const period = studyPrefs({ period: requestedPeriod }).period
  let ema: number | null = null
  let volume = 0, weighted = 0, session = "", complete = true
  return bars.map((bar, index) => {
    const day = barDay(bar.t)
    if (day !== session) { session = day; volume = 0; weighted = 0; complete = true }
    if (bar.v == null || !Number.isFinite(bar.v) || bar.v < 0) complete = false
    else { volume += bar.v; weighted += (bar.h + bar.l + bar.c) / 3 * bar.v }
    const window = bars.slice(Math.max(0, index - period + 1), index + 1)
    const ready = window.length === period && window.every((b) => Number.isFinite(b.c))
    const sma = ready ? window.reduce((sum, b) => sum + b.c, 0) / period : null
    const deviation = sma == null ? null : Math.sqrt(window.reduce((sum, b) => sum + (b.c - sma) ** 2, 0) / period)
    ema = sma == null ? null : ema == null ? sma : ema + 2 / (period + 1) * (bar.c - ema)
    return { sma, ema, upper: sma != null && deviation != null ? sma + 2 * deviation : null,
      lower: sma != null && deviation != null ? sma - 2 * deviation : null, vwap: complete && volume > 0 ? weighted / volume : null }
  })
}
