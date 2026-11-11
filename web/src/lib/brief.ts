import type { Account, Money, Order, Position, Risk } from "../api/trading-types"
import type { Candle, ExposureSummary, Summary, Volatility, VolatilitySeriesRow } from "../api/types"
import { barClock, barDay, type ChartLevel } from "./candles"
import { parseOsi } from "./journal"
import { isNum } from "./format"
import { compareMoney, subtractMoney } from "./trading"

export const marketDate = (time: number) => Number.isFinite(time) ? barDay(time / 1000) : null
const shiftDate = (date: string, days: number) => new Date(Date.parse(`${date}T12:00:00Z`) + days * 86400000).toISOString().slice(0, 10)

/** The engine's trading date includes the coming session during overnight trading. */
export function briefDay(now: number, engineDay?: string | null, volatility?: Volatility): string | null {
  const local = marketDate(now)
  if (!local) return null
  if (engineDay && engineDay >= local) return engineDay
  const clock = barClock(now / 1000)
  if (clock >= "17:00" || [0, 6].includes(new Date(`${local}T12:00:00Z`).getUTCDay()))
    return volatility?.implied_moves.sessions.find(session => session.date > local)?.date ?? null
  return local
}

/** Session rows include today until its scheduled close, even without a covering expiry. */
export function notePhase(day: string, now: number, volatility?: Volatility): "plan" | "review" {
  const local = marketDate(now)
  if (!local || day > local || barClock(now / 1000) < "09:30") return "plan"
  if (day < local || barClock(now / 1000) >= "16:00") return "review"
  if (volatility && marketDate(Date.parse(volatility.as_of)) === local &&
      !volatility.implied_moves.sessions.some(session => session.date === day)) return "review"
  return "plan"
}

export function priorSession(bars: readonly Candle[], day: string | null, now: number, expectedDay?: string | null) {
  if (!day || !Number.isFinite(now)) return undefined
  return bars.filter(bar => bar.t * 1000 <= now && barDay(bar.t) < day && (!expectedDay || barDay(bar.t) === expectedDay))
    .sort((a, b) => b.t - a.t)[0]
}

export function overnightMove(spot: number | null | undefined, close: number | null | undefined) {
  return isNum(spot) && spot > 0 && isNum(close) && close > 0
    ? { points: spot - close, percent: (spot / close - 1) * 100 } : null
}

/** Only observed evening/pre-market bars, not regular-session highs or future bars. */
export function overnightRange(bars: readonly Candle[], day: string | null, now: number) {
  if (!day || !Number.isFinite(now)) return null
  const evening = shiftDate(day, -1)
  const observed = bars.filter(bar => {
    if (bar.t * 1000 > now || !isNum(bar.h) || !isNum(bar.l) || bar.h <= 0 || bar.l <= 0) return false
    const date = barDay(bar.t), clock = barClock(bar.t)
    return (date === evening && clock >= "20:15") || (date === day && clock < "09:30")
  })
  return observed.length ? { high: Math.max(...observed.map(bar => bar.h)), low: Math.min(...observed.map(bar => bar.l)), bars: observed.length } : null
}

export interface BriefMove {
  points: number | null
  percent: number | null
  reason: string | null
  proxy: boolean
  truncated: boolean
  shared: boolean
  calendar_arbitrage: boolean
}
const missingMove = (reason: string): BriefMove => ({ points: null, percent: null, reason, proxy: false, truncated: false, shared: false, calendar_arbitrage: false })

export function todayMove(volatility: Volatility | undefined, day: string | null): BriefMove {
  if (!volatility || !day) return missingMove("no implied-move data")
  const moves = volatility.implied_moves
  if (marketDate(Date.parse(volatility.as_of)) === day && moves.today_reason !== "no_same_day_expiry") {
    return { points: moves.today_calendar_arbitrage ? null : moves.today_points, percent: moves.today_calendar_arbitrage ? null : moves.today_percent,
      reason: moves.today_calendar_arbitrage ? "calendar arbitrage" : moves.today_reason,
      proxy: moves.today_proxy, truncated: moves.today_truncated, shared: false, calendar_arbitrage: moves.today_calendar_arbitrage }
  }
  const session = moves.sessions.find(row => row.date === day)
  return session ? { ...session, points: session.calendar_arbitrage ? null : session.points, percent: session.calendar_arbitrage ? null : session.percent }
    : missingMove("no remaining session move")
}

/** Combine percentage variances, then express the week's remaining 1σ at current spot. */
export function weekMove(volatility: Volatility | undefined, day: string | null, spot: number | null | undefined): BriefMove {
  if (!volatility || !day) return missingMove("no implied-move data")
  const weekday = new Date(`${day}T12:00:00Z`).getUTCDay()
  const friday = shiftDate(day, 5 - weekday)
  const sessions = volatility.implied_moves.sessions.filter(row => row.date >= day && row.date <= friday)
  if (!sessions.length) return missingMove("no remaining sessions this week")
  const flags = { proxy: sessions.some(row => row.proxy), truncated: sessions.some(row => row.truncated),
    shared: sessions.some(row => row.shared), calendar_arbitrage: sessions.some(row => row.calendar_arbitrage) }
  if (sessions.some(row => !isNum(row.percent) || row.percent < 0 || row.reason || row.calendar_arbitrage))
    return { ...missingMove("incomplete week: missing session variance"), ...flags }
  const percent = Math.sqrt(sessions.reduce((sum, row) => sum + row.percent! ** 2, 0))
  return { percent, points: isNum(spot) && spot > 0 ? spot * percent / 100 : null, reason: isNum(spot) && spot > 0 ? null : "current spot unavailable", ...flags }
}

export function priorSeries(rows: readonly VolatilitySeriesRow[], priorDay: string | null, now: number) {
  return priorDay ? rows.find(row => row.t * 1000 <= now && barDay(row.t) === priorDay) : undefined
}
export const volChange = (current: number | null | undefined, previous: number | null | undefined) =>
  isNum(current) && isNum(previous) ? current - previous : null
export const invertedTerm = (value: number | null | undefined) => isNum(value) ? value > 1 : null

export function briefLevels(prior: Candle | undefined, overnight: ReturnType<typeof overnightRange>, exposure: ExposureSummary | undefined,
  spot: number | null | undefined, move: BriefMove): ChartLevel[] {
  const entries: [string, number | null | undefined][] = [
    ["Prior high", prior?.h], ["Prior low", prior?.l], ["Prior close", prior?.c],
    ["Overnight high", overnight?.high], ["Overnight low", overnight?.low],
    ["Call wall", exposure?.call_wall], ["Put wall", exposure?.put_wall], ["Gamma flip", exposure?.gamma_flip],
  ]
  if (isNum(spot) && spot > 0 && isNum(move.points) && move.points >= 0 && !move.calendar_arbitrage)
    entries.push(["Today +1σ", spot + move.points], ["Today −1σ", spot - move.points])
  // Coincident levels get one line and tag with all names retained.
  const grouped = new Map<number, string[]>()
  for (const [label, value] of entries) if (isNum(value) && value > 0) grouped.set(value, [...grouped.get(value) ?? [], label])
  return [...grouped].sort(([a], [b]) => b - a).map(([price, labels]) => ({ price, label: labels.join(" / "), kind: "brief" }))
}

/** Disabled limits do not constrain the allowance; missing active inputs do. */
export function lossAllowance(account: Account | undefined, risk: Risk | undefined): { value: Money | null; reason: string; parts: { label: string; value: Money | null }[] } {
  if (!account || !risk) return { value: null, reason: "account or risk unavailable", parts: [] }
  if (account.account_version !== risk.account_version) return { value: null, reason: "account and risk updating", parts: [] }
  if (!account.evaluation.valuation_complete || !risk.complete) return { value: null, reason: "incomplete valuation", parts: [] }
  const parts: { label: string; value: Money | null }[] = []
  if (compareMoney(risk.limits.max_daily_loss, "0") === 1)
    parts.push({ label: "Risk-limit daily loss left", value: subtractMoney(risk.limits.max_daily_loss, risk.daily_loss) })
  // The plan's own daily loss limit fails the attempt or locks the day: often tighter than the floor.
  if (account.evaluation.daily_loss) parts.push({ label: "Plan daily loss room", value: account.evaluation.daily_loss.room })
  if (account.evaluation.floor != null)
    parts.push({ label: "Floor room", value: subtractMoney(account.evaluation.equity, account.evaluation.floor) })
  const state = risk.guardrail_state ?? account.guardrail_state
  if (!state) return { value: null, reason: "soft floor state unavailable", parts }
  if (state.soft_floor != null) parts.push({ label: "Soft floor room", value: subtractMoney(account.evaluation.equity, state.soft_floor) })
  if (!parts.length) return { value: null, reason: "no loss limit or floor configured", parts }
  if (parts.some(part => compareMoney(part.value, "0") == null)) return { value: null, reason: "loss allowance inputs unavailable", parts }
  const smallest = parts.reduce((a, b) => compareMoney(a.value, b.value) === -1 ? a : b)
  return { value: compareMoney(smallest.value, "0") === -1 ? "0" : smallest.value, reason: smallest.label, parts }
}

/** Use the held contract's last trade and the selected account's actual cutoff. */
export function positionDeadlines(position: Position, cutoff: number, day: string | null) {
  if (!day) return []
  const events = [{ label: "expires", time: Date.parse(position.expiry_time ?? "") },
    { label: "last trade", time: Date.parse(position.last_trade_time ?? "") }]
  if (cutoff > 0) events.push({ label: "auto-close", time: Date.parse(position.last_trade_time ?? "") - cutoff * 1000 })
  return events.filter(event => marketDate(event.time) === day).sort((a, b) => a.time - b.time)
}


/** The order's contracts' own deadlines (expiry, last trade, auto-close), shown beside its day_end when known. */
export function orderDeadlines(order: Order, positions: readonly Position[], summary: Summary | undefined, cutoff: number, day: string | null) {
  const symbols = order.legs?.map(leg => leg.symbol) ?? (order.symbol ? [order.symbol] : [])
  const events: { label: string; time: number }[] = []
  let complete = true
  for (const symbol of symbols) {
    const position = positions.find(row => row.symbol === symbol)
    const terms = parseOsi(symbol)
    const expiries = summary?.symbol === order.underlying ? summary.expiries.filter(row => row.expiry === terms?.expiry) : []
    // A date with both AM and PM settlements cannot identify an unheld contract alone.
    const expiry = expiries.length === 1 ? expiries[0] : undefined
    const expiryTime = position?.expiry_time ?? expiry?.expiry_time
    const lastTrade = position?.last_trade_time ?? expiry?.last_trade
    if (!expiryTime || !lastTrade) complete = false
    for (const event of [{ label: "contract expires", time: Date.parse(expiryTime ?? "") },
      { label: "last trade", time: Date.parse(lastTrade ?? "") },
      ...(cutoff > 0 ? [{ label: "auto-close cutoff", time: Date.parse(lastTrade ?? "") - cutoff * 1000 }] : [])]) {
      if (day && marketDate(event.time) === day && !events.some(row => row.time === event.time && row.label === event.label)) events.push(event)
    }
  }
  return { events: events.sort((a, b) => a.time - b.time), complete }
}
