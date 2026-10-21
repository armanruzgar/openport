import { studyPrefs, type Studies } from "./studies"
import type { Candle, CandleInterval } from "../api/types"
import type { Order, Position } from "../api/trading-types"
import { isNum } from "./format"
import { orderLabel } from "./journal"

export const candleIntervals: { value: CandleInterval; label: string }[] = [
  { value: "1m", label: "1m" },
  { value: "5m", label: "5m" },
  { value: "15m", label: "15m" },
  { value: "30m", label: "30m" },
  { value: "1h", label: "1h" },
  { value: "1d", label: "1D" },
]

export const intervalNames: Record<CandleInterval, string> = {
  "1m": "1-minute", "5m": "5-minute", "15m": "15-minute", "30m": "30-minute", "1h": "hourly", "1d": "daily",
}

/** Bars fetched per request; the chart shows as many as fit and pans through the rest. */
export const candleLimit = 600

export interface ChartLevel {
  price: number
  label: string
  kind: "long" | "short" | "trigger" | "alert" | "brief"
}

const shortDay = (expiry: string) =>
  new Date(`${expiry}T12:00:00Z`).toLocaleDateString("en-US", { month: "short", day: "numeric", timeZone: "UTC" })

const levelPrice = (x: number) => x.toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 })

/** Lines over the underlying: each strike held (net long or short), and each armed order waiting for the underlying to cross a level. */
export function chartLevels(underlying: string, positions: readonly Position[], orders: readonly Order[]): ChartLevel[] {
  const strikes = new Map<number, { parts: string[]; net: number }>()
  for (const p of positions) {
    if (p.underlying !== underlying || p.quantity === 0) continue
    const entry = strikes.get(p.strike) ?? { parts: [], net: 0 }
    entry.parts.push(`${p.quantity > 0 ? "+" : "−"}${Math.abs(p.quantity)}${p.type === "call" ? "C" : "P"} ${shortDay(p.expiry)}`)
    entry.net += p.quantity
    strikes.set(p.strike, entry)
  }
  const levels: ChartLevel[] = [...strikes].map(([strike, { parts, net }]) =>
    ({ price: strike, label: `${strike} ${parts.join(", ")}`, kind: net < 0 ? "short" : "long" }))
  for (const o of orders) {
    const trigger = o.trigger
    if (o.underlying !== underlying || o.status !== "armed" || trigger?.source !== "underlying" || (trigger.symbol && trigger.symbol !== underlying)) continue
    const level = Number(trigger.level)
    if (!Number.isFinite(level)) continue
    const action = o.role === "stop_loss" ? "Stop" : o.role === "take_profit" ? "Take profit" : o.side === "sell" ? "Sell" : "Buy"
    levels.push({ price: level, kind: "trigger",
      label: `${action} ${trigger.direction === "at_or_below" ? "≤" : "≥"} ${levelPrice(level)}: ${orderLabel(o)} ×${o.remaining_quantity}` })
  }
  return levels.sort((a, b) => b.price - a.price)
}

/** One standard deviation of the underlying's move by an expiry, S·σ·√T, from its at-the-money IV. */
export function expectedMove(spot: number | null | undefined, atmIv: number | null | undefined, days: number | null | undefined): number | null {
  if (!isNum(spot) || !isNum(atmIv) || !isNum(days) || spot <= 0 || atmIv <= 0 || days <= 0) return null
  return spot * atmIv * Math.sqrt(days / 365)
}

/** The bars in view: `span` of them ending `offset` bars before the latest. */
export function clampWindow(total: number, span: number, offset: number, minSpan = 10) {
  const shown = Math.min(total, Math.max(minSpan, Math.round(span)))
  const back = Math.max(0, Math.min(total - shown, Math.round(offset)))
  return { span: shown, offset: back, start: total - back - shown, end: total - back }
}

/** Low to high of the bars with a margin, so wicks never touch the frame. */
export function priceDomain(bars: readonly Candle[]): [number, number] | null {
  let lo = Infinity
  let hi = -Infinity
  for (const bar of bars) {
    if (bar.l < lo) lo = bar.l
    if (bar.h > hi) hi = bar.h
  }
  if (!(lo <= hi)) return null
  const pad = (hi - lo || hi * 0.001 || 1) * 0.08
  return [lo - pad, hi + pad]
}

const zone = "America/New_York"
const dayFormat = new Intl.DateTimeFormat("en-CA", { timeZone: zone, year: "numeric", month: "2-digit", day: "2-digit" })
const timeFormat = new Intl.DateTimeFormat("en-US", { timeZone: zone, hour: "2-digit", minute: "2-digit", hourCycle: "h23" })
const monthDayFormat = new Intl.DateTimeFormat("en-US", { timeZone: zone, month: "short", day: "numeric" })
const monthFormat = new Intl.DateTimeFormat("en-US", { timeZone: zone, month: "short" })
const longDayFormat = new Intl.DateTimeFormat("en-US", { timeZone: zone, weekday: "short", month: "short", day: "numeric", year: "numeric" })

/** "2026-09-22", the bar's New York date. */
export const barDay = (t: number) => dayFormat.format(t * 1000)
/** "09:30" New York time. */
export const barClock = (t: number) => timeFormat.format(t * 1000)

/** A bar's time for the crosshair: "Sep 22 10:35" intraday, "Tue, Sep 22, 2026" daily. */
export function barTime(t: number, interval: CandleInterval): string {
  return interval === "1d" ? longDayFormat.format(t * 1000) : `${monthDayFormat.format(t * 1000)} ${barClock(t)}`
}

const roundSteps = [5, 15, 30, 60, 120, 240]
const intervalMinutes: Record<CandleInterval, number> = { "1m": 1, "5m": 5, "15m": 15, "30m": 30, "1h": 60, "1d": 1440 }

/** Intraday bars that start a new New York day, or follow a gap of more than a quarter hour beyond the interval (the close to the overnight session, a halted feed). */
export function sessionBreak(previous: Candle, next: Candle, interval: CandleInterval): boolean {
  if (interval === "1d") return false
  return barDay(previous.t) !== barDay(next.t) || next.t - previous.t > (intervalMinutes[interval] + 15) * 60
}

export interface TimeTick { index: number; label: string; major: boolean }

/**
 * Axis labels at least `spacing` bars apart: the date where a New York day starts
 * (daily bars: the month, or the year in January), the time where a session
 * resumes after a gap, and otherwise times on round steps such as :00 and :30.
 */
export function timeTicks(bars: readonly Candle[], interval: CandleInterval, spacing: number): TimeTick[] {
  const ticks: TimeTick[] = []
  const gap = Math.max(1, spacing)
  const minutes = intervalMinutes[interval]
  const step = interval === "1d" ? 0 : roundSteps.find((s) => s >= gap * minutes) ?? 0
  let last = -Infinity
  let previous = ""
  for (let i = 0; i < bars.length; i++) {
    const t = bars[i]!.t
    const day = barDay(t)
    const period = interval === "1d" ? day.slice(0, 7) : day
    const newPeriod = i > 0 && period !== previous
    const resumes = i > 0 && !newPeriod && sessionBreak(bars[i - 1]!, bars[i]!, interval)
    previous = period
    if ((newPeriod || resumes) && i - last >= gap * 0.6) {
      ticks.push({ index: i, major: true, label: interval === "1d"
        ? (day.slice(5, 7) === "01" ? day.slice(0, 4) : monthFormat.format(t * 1000))
        : newPeriod ? monthDayFormat.format(t * 1000) : barClock(t) })
      last = i
      continue
    }
    if (!step || i - last < gap) continue
    const clock = barClock(t)
    const minute = Number(clock.slice(0, 2)) * 60 + Number(clock.slice(3, 5))
    if (minute % step === 0) {
      ticks.push({ index: i, major: false, label: clock })
      last = i
    }
  }
  return ticks
}

const chartStorageKey = "openport.chart"
export interface ChartPrefs { interval: CandleInterval; hidden: boolean; studies?: Studies }
const defaultPrefs: ChartPrefs = { interval: "5m", hidden: false }

/** The viewer's last interval and whether they hid the chart; defaults where storage is unavailable. */
export function loadChartPrefs(): ChartPrefs {
  try {
    const saved = JSON.parse(localStorage.getItem(chartStorageKey) ?? "null") as Partial<ChartPrefs> | null
    return {
      interval: candleIntervals.some((i) => i.value === saved?.interval) ? saved!.interval! : defaultPrefs.interval,
      hidden: saved?.hidden === true,
      ...(saved?.studies ? { studies: studyPrefs(saved.studies) } : {}),
    }
  } catch {
    return defaultPrefs
  }
}

export function saveChartPrefs(prefs: ChartPrefs) {
  try { localStorage.setItem(chartStorageKey, JSON.stringify(prefs)) } catch { /* Keep the in-memory choice. */ }
}
