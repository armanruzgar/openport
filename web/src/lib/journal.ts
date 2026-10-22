import type { ShareSource, ShareTrade, Trade } from "../api/trading-types"
import type { TradeGroup } from "./positions"

/** A closed or open trade in the journal: an option contract's, or shares from exercise and assignment. */
type JournalOptionTrade = Trade & {
  /** Display identity and side of every leg in an aggregated journal entry. */
  journalGroup?: { label: string; side: "call" | "put" | "mixed" }
}
export type JournalTrade = JournalOptionTrade | ShareTrade
export const isShares = (trade: JournalTrade): trade is ShareTrade => "kind" in trade && trade.kind === "shares"

/** Journal analytics are display summaries in dollars; accounting stays exact on the server. */
const dollars = (value: string | null | undefined) => {
  const parsed = value == null ? NaN : Number(value)
  return Number.isFinite(parsed) ? parsed : 0
}
const tradeUnderlying = (trade: JournalTrade) => isShares(trade) ? trade.symbol : trade.underlying
export const tradeNet = (trade: JournalTrade) => dollars(trade.net)

export interface JournalStats {
  trades: number
  wins: number
  losses: number
  /** Wins over decided (nonzero) trades. */
  winRate: number | null
  net: number
  grossWin: number
  grossLoss: number
  /** Gross wins over gross losses; Infinity with wins and no losses. */
  profitFactor: number | null
  averageWin: number | null
  averageLoss: number | null
  best: JournalTrade | null
  worst: JournalTrade | null
  contracts: number
  shares: number
  averageHoldSeconds: number | null
  fees: number
  /** Mean of the closed trades' net over the buying power their entries needed; null when none says. */
  returnOnBuyingPower: number | null
}

export function journalStats(trades: readonly JournalTrade[]): JournalStats {
  const closed = trades.filter((t) => t.status === "closed")
  let wins = 0, losses = 0, grossWin = 0, grossLoss = 0, contracts = 0, shares = 0, fees = 0, hold = 0, held = 0
  let best: JournalTrade | null = null, worst: JournalTrade | null = null
  let robp = 0, robpCount = 0
  for (const trade of closed) {
    const ratio = isShares(trade) ? null : trade.return_on_buying_power
    if (ratio != null && Number.isFinite(ratio)) { robp += ratio; robpCount++ }
    const net = tradeNet(trade)
    if (net > 0) { wins++; grossWin += net } else if (net < 0) { losses++; grossLoss += net }
    if (!best || net > tradeNet(best)) best = trade
    if (!worst || net < tradeNet(worst)) worst = trade
    if (isShares(trade)) shares += trade.opened_shares
    else contracts += trade.opened_contracts
    fees += dollars(trade.fees)
    if (trade.duration_seconds != null) { hold += trade.duration_seconds; held++ }
  }
  return {
    trades: closed.length, wins, losses,
    winRate: wins + losses > 0 ? wins / (wins + losses) : null,
    net: grossWin + grossLoss, grossWin, grossLoss,
    profitFactor: grossLoss < 0 ? grossWin / -grossLoss : wins > 0 ? Infinity : null,
    averageWin: wins ? grossWin / wins : null,
    averageLoss: losses ? grossLoss / losses : null,
    best, worst, contracts, shares, fees,
    averageHoldSeconds: held ? hold / held : null,
    returnOnBuyingPower: robpCount ? robp / robpCount : null,
  }
}

const newYorkParts = new Intl.DateTimeFormat("en-CA", { timeZone: "America/New_York", year: "numeric", month: "2-digit", day: "2-digit", weekday: "short" })
/** ISO timestamp -> New York trading date "YYYY-MM-DD" and weekday (0 = Sunday). */
export function newYorkDate(iso: string): { date: string; weekday: number } | null {
  const time = Date.parse(iso)
  if (!Number.isFinite(time)) return null
  const parts = Object.fromEntries(newYorkParts.formatToParts(time).map((p) => [p.type, p.value]))
  return { date: `${parts.year}-${parts.month}-${parts.day}`, weekday: ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"].indexOf(parts.weekday ?? "") }
}

const newYorkHour = new Intl.DateTimeFormat("en-US", { timeZone: "America/New_York", hour: "2-digit", hourCycle: "h23" })
const newYorkEntryTime = new Intl.DateTimeFormat("en-US", { timeZone: "America/New_York", hour: "2-digit", minute: "2-digit", hourCycle: "h23" })
/**
 * ISO timestamp -> the engine's trading date and its weekday (1 = Monday): a weekday's New York date until 17:00,
 * when its curb session ends; after that, and over the weekend, the next weekday, whose overnight session opens that
 * evening. So a Sunday-evening close counts toward Monday. Holidays are unknown here, so the evening before one counts
 * toward it rather than the next session; trades from current servers carry the exact `trading_day` instead.
 */
export function tradingDate(iso: string): { date: string; weekday: number } | null {
  const day = newYorkDate(iso)
  if (!day) return null
  const hour = Number(newYorkHour.formatToParts(Date.parse(iso)).find((p) => p.type === "hour")?.value)
  let ahead = hour >= 17 ? 1 : 0
  while ([0, 6].includes((day.weekday + ahead) % 7)) ahead++
  const date = new Date(Date.parse(`${day.date}T12:00:00Z`) + ahead * 86_400_000).toISOString().slice(0, 10)
  return { date, weekday: (day.weekday + ahead) % 7 }
}

/**
 * The trading date a closed trade counts toward and its weekday (1 = Monday): the server's `trading_day`, which knows
 * market holidays, or else the weekday estimate above for older servers.
 */
export function closingDay(trade: JournalTrade): { date: string; weekday: number } | null {
  if (trade.trading_day && /^\d{4}-\d{2}-\d{2}$/.test(trade.trading_day))
    return { date: trade.trading_day, weekday: new Date(`${trade.trading_day}T12:00:00Z`).getUTCDay() }
  return trade.closed ? tradingDate(trade.closed) : null
}

export interface DayResult { date: string; net: number; trades: number; wins: number }
/** Closed trades by the New York date they closed. */
export function dailyResults(trades: readonly JournalTrade[]): Map<string, DayResult> {
  const days = new Map<string, DayResult>()
  for (const trade of trades) {
    if (trade.status !== "closed" || !trade.closed) continue
    const day = newYorkDate(trade.closed)
    if (!day) continue
    const cell = days.get(day.date) ?? { date: day.date, net: 0, trades: 0, wins: 0 }
    const net = tradeNet(trade)
    cell.net += net
    cell.trades++
    if (net > 0) cell.wins++
    days.set(day.date, cell)
  }
  return days
}

/** Sunday-first weeks of a month as ISO dates, with null padding. month is 1-12. */
export function monthWeeks(year: number, month: number): (string | null)[][] {
  const first = new Date(Date.UTC(year, month - 1, 1))
  const length = new Date(Date.UTC(year, month, 0)).getUTCDate()
  const cells: (string | null)[] = Array(first.getUTCDay()).fill(null)
  for (let day = 1; day <= length; day++) cells.push(`${year}-${String(month).padStart(2, "0")}-${String(day).padStart(2, "0")}`)
  while (cells.length % 7) cells.push(null)
  const weeks: (string | null)[][] = []
  for (let i = 0; i < cells.length; i += 7) weeks.push(cells.slice(i, i + 7))
  return weeks
}

/** Every tag on the trades, alphabetically. */
export function tradeTags(trades: readonly JournalTrade[]): string[] {
  return [...new Set(trades.flatMap((t) => t.tags ?? []))].sort()
}
/** "breakout, 0DTE ,fomc" -> ["breakout", "0dte", "fomc"]: trimmed, lowercased, each once. */
export function parseTags(text: string): string[] {
  return [...new Set(text.split(",").map((tag) => tag.trim().toLowerCase()).filter(Boolean))]
}

export type Dimension = "duration" | "weekday" | "month" | "tag" | "time" | "dte" | "delta" | "iv" | "underlying" | "template" | "playbook" | "coach"
export type Side = "all" | "call" | "put"
export const durationBuckets = [
  { label: "< 1m", max: 60 }, { label: "1–5m", max: 300 }, { label: "5–15m", max: 900 },
  { label: "15m–1h", max: 3600 }, { label: "1–4h", max: 14_400 }, { label: "4h–1d", max: 86_400 },
  { label: "1–3d", max: 259_200 }, { label: "3d+", max: Infinity },
] as const
const weekdays = ["Mon", "Tue", "Wed", "Thu", "Fri"]
const months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]

export const dimensions: { value: Dimension; label: string }[] = [
  { value: "duration", label: "Hold time" }, { value: "weekday", label: "Weekday" }, { value: "month", label: "Month" },
  { value: "tag", label: "Tag" }, { value: "time", label: "Entry time (ET)" }, { value: "dte", label: "Entry DTE" },
  { value: "delta", label: "Entry |delta|" }, { value: "iv", label: "Entry IV" }, { value: "underlying", label: "Underlying" },
  { value: "template", label: "Strategy template" }, { value: "playbook", label: "Playbook" }, { value: "coach", label: "Coach flag" },
]
export const mistakePicks = ["chased", "oversized", "moved stop", "revenge", "broke plan", "early exit", "A+ setup"] as const
export interface ScenarioMark { type: string; time: string; end?: string; symbols?: string[] }
export interface CoachContext { events?: ScenarioMark[]; brokenOrders?: Set<string> }
export function coachFlags(trade: JournalTrade, peers: readonly JournalTrade[], context: CoachContext = {}): string[] {
  if (trade.status !== "closed") return []
  const flags: string[] = []
  if (!isShares(trade)) {
    const r = trade.review
    if (r?.mfe != null && Number(r.mfe) > 0 && r.give_back != null && Number(r.give_back) > Number(r.mfe) * .5) flags.push("Gave back >50%")
    if (r?.heat != null && r.heat > 1) flags.push("Heat >1R")
    if ((trade.entry_order ?? trade.strategy_id) && context.brokenOrders?.has((trade.entry_order ?? trade.strategy_id)!)) flags.push("Broke playbook rule")
  }
  if (context.events?.some(e => (!e.symbols?.length || e.symbols.includes(tradeUnderlying(trade))) &&
    Date.parse(e.time) <= Date.parse(trade.closed!) && Date.parse(e.end ?? e.time) >= Date.parse(trade.opened))) flags.push("Held through scenario event")
  const size = (t: JournalTrade) => isShares(t) ? t.max_shares : t.max_quantity
  const sizes = peers.filter(t => t.status === "closed" && isShares(t) === isShares(trade) && tradeUnderlying(t) === tradeUnderlying(trade)).map(size).sort((a, b) => a - b)
  const middle = Math.floor(sizes.length / 2)
  const median = sizes.length ? (sizes[middle]! + sizes[Math.floor((sizes.length - 1) / 2)]!) / 2 : 0
  if (sizes.length >= 3 && median > 0 && size(trade) > 2 * median) flags.push("Size >2× median")
  return flags
}
/** Descriptive association, not a counterfactual estimate of avoiding a mistake. */
export function mistakeCosts(trades: readonly JournalTrade[]) {
  const closed = trades.filter(t => t.status === "closed")
  return mistakePicks.map(label => {
    const withTag = closed.filter(t => t.mistakes?.includes(label))
    const withoutTag = closed.filter(t => !t.mistakes?.includes(label))
    return { label, with: journalStats(withTag), without: journalStats(withoutTag) }
  })
}
export interface Bucket {
  label: string; net: number; trades: number; wins: number; losses: number; winRate: number | null
  expectancy: number | null; averageR: number | null; profitFactor: number | null; giveBack: number | null
}
function entryLabels(t: JournalTrade, dimension: Dimension): string[] {
  const c = isShares(t) ? null : t.entry_context
  const bucket = (value: number | null | undefined, bounds: number[], labels: string[]) =>
    value == null || !Number.isFinite(value) ? "Unknown" : labels[bounds.findIndex(b => value < b)] ?? "Unknown"
  if (dimension === "time") {
    const time = Date.parse(t.opened)
    const parts = Number.isFinite(time) ? newYorkEntryTime.formatToParts(time) : []
    const minute = parts.length ? Number(parts.find(p => p.type === "hour")?.value) * 60 + Number(parts.find(p => p.type === "minute")?.value) : null
    return [bucket(minute, [570, 630, 720, 840, 960, Infinity], ["Before 09:30", "09:30–10:30", "10:30–12:00", "12:00–14:00", "14:00–16:00", "After 16:00"])]
  }
  if (dimension === "dte") return [bucket(c?.years == null ? null : c.years * 365, [1, 8, 31, Infinity], ["<1 DTE", "1–7 DTE", "8–30 DTE", "31+ DTE"])]
  if (dimension === "delta") return [bucket(c?.delta == null ? null : Math.abs(c.delta), [.2, .4, .6, .8, Infinity], ["<0.20", "0.20–0.40", "0.40–0.60", "0.60–0.80", "0.80+"])]
  if (dimension === "iv") return [bucket(c?.iv, [.15, .25, .40, Infinity], ["<15%", "15–25%", "25–40%", "40%+"])]
  if (dimension === "underlying") return [tradeUnderlying(t)]
  if (dimension === "playbook") return t.tags?.filter(v => v.startsWith("playbook:")).map(v => v.slice(9).split("@v")[0]!).filter(Boolean) || []
  if (dimension === "template") return (t.tags ?? []).filter(v => /^(put|call)-(credit|debit|calendar|diagonal)-|^iron-(condor|butterfly)-|^(long|short)-(straddle|strangle)-|^long-(put|call)-butterfly-/.test(v))
  return []
}
/** Closed trades only. Breakevens count in expectancy; win rate excludes them. */
export function tradeBuckets(trades: readonly JournalTrade[], dimension: Dimension, side: Side = "all", context: CoachContext = {}): Bucket[] {
  const fixed = dimension === "duration" ? durationBuckets.map(b => b.label) : dimension === "weekday" ? weekdays : dimension === "month" ? months : dimension === "tag" ? [...tradeTags(trades), "untagged"] : []
  const groups = new Map<string, JournalTrade[]>(fixed.map(label => [label, []]))
  for (const t of trades) {
    if (t.status !== "closed" || !t.closed || (side !== "all" && (isShares(t) || (t.journalGroup?.side ?? t.type) !== side))) continue
    const day = closingDay(t)
    let labels = dimension === "tag" ? (t.tags?.length ? t.tags : ["untagged"])
      : dimension === "duration" ? [durationBuckets.find(b => (t.duration_seconds ?? 0) < b.max)!.label]
      : dimension === "weekday" ? [day ? weekdays[day.weekday - 1] : undefined]
      : dimension === "month" ? [day ? months[Number(day.date.slice(5, 7)) - 1] : undefined]
      : dimension === "coach" ? coachFlags(t, trades, context) : entryLabels(t, dimension)
    if (!labels.length) labels = [dimension === "coach" ? "No flags" : "Unknown"]
    for (const label of new Set(labels)) if (label) {
      const members = groups.get(label)
      if (members) members.push(t)
      else groups.set(label, [t])
    }
  }
  return [...groups].map(([label, members]) => {
    const s = journalStats(members)
    const reviews = members.filter((t): t is JournalOptionTrade => !isShares(t)).map(t => t.review)
    const rs = reviews.flatMap(r => r?.r_multiple == null ? [] : [r.r_multiple])
    const backs = reviews.flatMap(r => r?.give_back == null ? [] : [Number(r.give_back)])
    const average = (values: number[]) => values.length ? values.reduce((a, b) => a + b, 0) / values.length : null
    return { label, net: s.net, trades: s.trades, wins: s.wins, losses: s.losses, winRate: s.winRate,
      expectancy: s.trades ? s.net / s.trades : null, averageR: average(rs), profitFactor: s.profitFactor, giveBack: average(backs) }
  })
}

/** 290 -> "4m 50s"; 7500 -> "2h 5m"; 273600 -> "3d 4h". */
export function formatDuration(seconds: number | null | undefined): string {
  if (seconds == null || !Number.isFinite(seconds) || seconds < 0) return "—"
  const s = Math.round(seconds)
  if (s < 60) return `${s}s`
  if (s < 3600) return `${Math.floor(s / 60)}m${s % 60 ? ` ${s % 60}s` : ""}`
  if (s < 86_400) return `${Math.floor(s / 3600)}h${Math.floor(s % 3600 / 60) ? ` ${Math.floor(s % 3600 / 60)}m` : ""}`
  return `${Math.floor(s / 86_400)}d${Math.floor(s % 86_400 / 3600) ? ` ${Math.floor(s % 86_400 / 3600)}h` : ""}`
}

/** Canonical padded OSI -> its terms; null when malformed. */
export function parseOsi(symbol: string): { root: string; expiry: string; type: "call" | "put"; strike: number } | null {
  const match = /^(.{6})(\d{2})(\d{2})(\d{2})([CP])(\d{8})$/.exec(symbol)
  if (!match) return null
  const [, root, yy, mm, dd, kind, strike] = match
  return { root: root!.trim(), expiry: `20${yy}-${mm}-${dd}`, type: kind === "C" ? "call" : "put", strike: Number(strike) / 1000 }
}
/** Order and fill rows: "SPX Oct 22 5000C", falling back to the raw symbol. */
export function osiLabel(symbol: string, underlying: string): string {
  const terms = parseOsi(symbol)
  return terms ? contractLabel({ underlying: underlying || terms.root, expiry: terms.expiry, strike: terms.strike, type: terms.type }) : symbol
}

/** A multi-leg order: "SPX Oct 22 −4900P +4890P", with ratios above one ("+2×4890P") and each leg's date when they differ. */
export function legsLabel(legs: { symbol: string; side: "buy" | "sell"; ratio: number }[], underlying: string): string {
  const terms = legs.map((leg) => ({ leg, terms: parseOsi(leg.symbol) }))
  if (terms.some((t) => !t.terms)) return legs.map((leg) => `${leg.side === "buy" ? "+" : "−"}${leg.symbol}`).join(" ")
  const dates = new Set(terms.map((t) => t.terms!.expiry))
  const day = (expiry: string) => new Date(`${expiry}T12:00:00Z`).toLocaleDateString("en-US", { month: "short", day: "numeric", timeZone: "UTC" })
  const parts = terms.map(({ leg, terms: t }) => `${leg.side === "buy" ? "+" : "−"}${leg.ratio > 1 ? `${leg.ratio}×` : ""}${dates.size > 1 ? `${day(t!.expiry)} ` : ""}${t!.strike}${t!.type === "call" ? "C" : "P"}`)
  return `${underlying || terms[0]!.terms!.root}${dates.size === 1 ? ` ${day(terms[0]!.terms!.expiry)}` : ""} ${parts.join(" ")}`
}
/** An order's contract, or its legs. */
export function orderLabel(order: { symbol: string | null; underlying: string; legs?: { symbol: string; side: "buy" | "sell"; ratio: number }[] | null }): string {
  return order.legs?.length ? legsLabel(order.legs, order.underlying) : osiLabel(order.symbol ?? "", order.underlying)
}

/** A trade's name in the journal: "SPX Oct 22 5000C", or "SPY 200 shares" (short ones say so). */
export function journalLabel(trade: JournalTrade): string {
  return isShares(trade) ? `${trade.symbol} ${trade.max_shares} shares${trade.direction === "short" ? " short" : ""}` : trade.journalGroup?.label ?? contractLabel(trade)
}

/** How shares came or went: "Exercised SPY Oct 16 500C at expiry", "Assigned SPY Oct 16 500P", "Sold". */
export function shareSourceLabel(source: ShareSource | null, option: string | null, underlying: string, shares: "opened" | "closed", direction: "long" | "short"): string {
  const contract = option ? osiLabel(option, underlying) : "an option"
  switch (source) {
    case "expiry_exercise": return `Exercised ${contract} at expiry`
    case "early_exercise": return `Exercised ${contract} early`
    case "assignment": return `Assigned ${contract}`
    case "trade": return (shares === "closed") === (direction === "long") ? "Sold" : "Bought"
    case "rule": return "Closed by the evaluation"
    case "reset": return "Account reset"
    default: return "—"
  }
}

/** How a closed option trade ended when the trader's own order did not close it: "stop", "liquidated: drawdown", "settlement"... */
export function exitLabel(trade: Trade): string | null {
  switch (trade.closed_by ?? trade.closure) {
    case "stop_loss": return "stop"
    case "take_profit": return "target"
    case "flatten": return "flatten"
    case "playbook": return "playbook time stop"
    case "system": return trade.system_reason === "expiry" ? "auto-closed before expiry"
      : trade.system_reason === "soft_floor" ? "liquidated: soft floor" : `liquidated${trade.system_reason ? `: ${trade.system_reason}` : ""}`
    case "settlement": case "exercise": case "assignment": case "abandon": case "reset": return trade.closed_by ?? trade.closure
    default: return null
  }
}

/** "SPXW  261022C05000000"-style trade -> "SPX Oct 22 5000C". */
export function contractLabel(t: { underlying: string; expiry: string; strike: number; type: "call" | "put" }): string {
  const date = new Date(`${t.expiry}T12:00:00Z`)
  const day = date.toLocaleDateString("en-US", { month: "short", day: "numeric", timeZone: "UTC" })
  return `${t.underlying} ${day} ${t.strike}${t.type === "call" ? "C" : "P"}`
}

/** Net closing premium per strategy unit, signed as order prices are (positive a debit, negative a credit), and return on absolute net entry premium. */
export function strategyResults(trades: readonly Trade[], filledUnits?: number): { close: number | null; return: number | null } {
  if (!trades.length || trades.some((t) => t.status !== "closed" || t.average_close == null)) return { close: null, return: null }
  const units = filledUnits ?? trades.reduce((n, t) => {
    const gcd = (a: number, b: number): number => b ? gcd(b, a % b) : a
    return gcd(n, t.opened_contracts)
  }, 0)
  // Closing sells a long leg, a credit, and buys back a short one, a debit.
  const close = trades.reduce((sum, t) => sum + (t.direction === "long" ? -1 : 1) * Number(t.average_close) * t.closed_contracts, 0)
  const cost = Math.abs(trades.reduce((sum, t) => sum + (t.direction === "long" ? 1 : -1) * Number(t.cost), 0))
  return { close: units ? close / units : null, return: cost ? trades.reduce((sum, t) => sum + tradeNet(t), 0) / cost : null }
}

/**
 * Each strategy or whole trade as one journal entry, for headline stats and reports
 * counted by trade rather than by round trip: its legs' P&L and fees together, from
 * its first opening to its last close, closed only once every leg is. Single round
 * trips pass through as they are.
 */
export function wholeEntries(groups: readonly TradeGroup[]): JournalOptionTrade[] {
  return groups.map((group) => {
    const [first, ...rest] = group.trades
    if (!first || (!rest.length && !group.whole && !group.order)) return first!
    const sum = (pick: (t: Trade) => string | null | undefined) => group.trades.reduce((total, t) => total + dollars(pick(t)), 0).toFixed(6)
    const open = group.trades.some((t) => t.status === "open")
    const closed = open ? null : group.trades.map((t) => t.closed ?? "").sort().at(-1) ?? null
    const opened = group.trades.map((t) => t.opened).sort()[0]!
    const days = group.trades.map((t) => t.trading_day ?? "").filter(Boolean).sort()
    // A tag or playbook filter can leave out some of a whole trade's round trips: total only those shown.
    const whole = group.whole?.round_trips.every((id) => group.trades.some((t) => t.id === id)) ? group.whole : null
    return {
      ...first, id: group.whole?.id ?? first.id, status: open ? "open" : "closed", opened, closed,
      journalGroup: {
        label: group.label,
        side: group.trades.every((t) => t.type === first.type) ? first.type : "mixed",
      },
      trading_day: open ? null : days.at(-1) ?? first.trading_day,
      duration_seconds: closed ? (Date.parse(closed) - Date.parse(opened)) / 1000 : null,
      opened_contracts: group.trades.reduce((total, t) => total + t.opened_contracts, 0),
      closed_contracts: group.trades.reduce((total, t) => total + t.closed_contracts, 0),
      gross: whole?.gross ?? sum((t) => t.gross), fees: whole?.fees ?? sum((t) => t.fees),
      net: whole?.net ?? sum((t) => t.net), return: null,
      buying_power: whole?.buying_power ?? first.strategy_buying_power ?? undefined,
      return_on_buying_power: whole ? whole.return_on_buying_power ?? null : first.strategy_return_on_buying_power ?? null,
      tags: [...new Set(group.trades.flatMap((t) => t.tags ?? []))],
      mistakes: [...new Set(group.trades.flatMap((t) => t.mistakes ?? []))],
      review: group.whole?.review ?? first.strategy_review ?? first.review,
    }
  })
}
