import type { OptionQuote } from "../api/types"
import type { Order, PendingOrder, Side, Trail, Trigger, TriggerReference, TriggerStudy } from "../api/trading-types"
import { price as formatPrice } from "./format"
import { timestampET } from "./freshness"

type Kind = "call" | "put"
const title = (type: Kind) => (type === "call" ? "Call" : "Put")

/** Closing and opening quantities of an order against the held position. */
export function split(side: Side, quantity: number, held: number) {
  const closing = Math.max(0, Math.min(quantity, side === "buy" ? -held : held))
  return { closing, opening: Math.max(0, quantity - closing) }
}

/** "Long Call", "Close Short Put", "Reverse to Short Call"... */
export function strategyName(side: Side, type: Kind, held: number, quantity: number): string {
  const { closing, opening } = split(side, quantity, held)
  const direction = side === "buy" ? "Long" : "Short"
  if (closing === 0) return `${direction} ${title(type)}`
  if (opening === 0) return `Close ${side === "buy" ? "Short" : "Long"} ${title(type)}`
  return `Reverse to ${direction} ${title(type)}`
}

export interface Marketability {
  marketable: boolean
  /** The executable far side, when there is one. */
  price: number | null
  size: number | null
  message: string
}
/** Mirrors the matcher: buys execute at the ask when ask <= limit, sells at the bid when bid >= limit. */
export function marketability(side: Side, type: "limit" | "market", limit: string, quote: OptionQuote | null): Marketability {
  const buy = side === "buy"
  const far = buy ? quote?.ask : quote?.bid
  const size = buy ? quote?.ask_size : quote?.bid_size
  const book = buy ? "ask" : "bid"
  if (far == null || !Number.isFinite(far) || far <= 0)
    return { marketable: false, price: null, size: null, message: `No executable ${book}; the order cannot fill until one is quoted.` }
  const depth = size != null && Number.isFinite(size) ? `, up to ${size} displayed` : ""
  if (type === "market")
    return { marketable: true, price: far, size: size ?? null, message: `Fills now at the ${book} $${formatPrice(far)}${depth}; any remainder cancels.` }
  const value = Number(limit)
  if (!limit || !Number.isFinite(value) || value <= 0)
    return { marketable: false, price: far, size: size ?? null, message: "Enter a limit price." }
  if (buy ? far <= value : far >= value)
    return { marketable: true, price: far, size: size ?? null, message: `Marketable: fills now at the ${book} $${formatPrice(far)}${depth}.` }
  const [quoted, limited] = samePrecision(far, value)
  return { marketable: false, price: far, size: size ?? null,
    message: `Rests ${buy ? "below the ask" : "above the bid"} ($${quoted}). Fills when the ${book} reaches $${limited}.` }
}
/** Two prices in one sentence at one precision: cents, or to the tenth of a cent when either has one. */
function samePrecision(a: number, b: number): [string, string] {
  const subCent = (x: number) => Math.abs(x * 100 - Math.round(x * 100)) > 1e-6
  const digits = subCent(a) || subCent(b) ? 3 : 2
  return [a.toFixed(digits), b.toFixed(digits)]
}

export type Direction = "at_or_below" | "at_or_above"
export const opposite = (direction: Direction): Direction => (direction === "at_or_below" ? "at_or_above" : "at_or_below")
/**
 * Where a stop sits for an entry. Option-price stops watch the exit's side
 * (the bid when selling out of a long), so they trigger on the way down for
 * longs and up for shorts. Underlying stops trigger when spot moves against the
 * position: down for long calls and short puts, up for long puts and short calls.
 */
export function stopDirection(source: "option" | "underlying", entry: Side, type: Kind): Direction {
  if (source === "option") return entry === "buy" ? "at_or_below" : "at_or_above"
  return (entry === "buy") === (type === "call") ? "at_or_below" : "at_or_above"
}
/** A conditional entry fires when spot reaches the level from where it is now. */
export function crossDirection(level: number, spot: number | null | undefined): Direction {
  return spot != null && Number.isFinite(spot) && level < spot ? "at_or_below" : "at_or_above"
}
/** "$0.50", "10%" or "3 ticks". */
export function describeTrail(trail: Trail): string {
  if (trail.unit === "ticks") return `${trail.value} tick${Number(trail.value) === 1 ? "" : "s"}`
  return trail.unit === "percent" ? `${Number(trail.value)}%` : `$${Number(trail.value).toFixed(2)}`
}
/** "bid ≤ $3.50", "mid ≤ $3.60, trailing $0.50", "SPX ≥ 5,010.00", "VIX ≥ 20.00", "SPX 30-day IV ≤ 15.00" or "it is 15:30 New York time or later". */
export function describeTrigger(trigger: { source: Trigger["source"]; direction?: Direction; level?: string; symbol?: string; study?: TriggerStudy; at?: string; reference?: TriggerReference; trail?: Trail | null },
  side: Side, underlying: string): string {
  const sign = trigger.direction === "at_or_below" ? "≤" : "≥"
  if (trigger.source === "time") return `it is ${trigger.at ?? "?"} New York time${trigger.direction === "at_or_below" ? " or earlier" : " or later"}`
  const level = Number(trigger.level)
  const text = Number.isFinite(level) ? level.toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 }) : trigger.level ?? ""
  const watched = trigger.symbol || underlying
  if (trigger.source === "study") return `${watched} ${trigger.study === "term_ratio" ? "9d/30d IV ratio" : trigger.study === "iv7" ? "7-day IV" : "30-day IV"} ${sign} ${text}`
  const reference = trigger.reference === "mid" || trigger.reference === "mark" ? trigger.reference : null
  const base = trigger.source === "combo" ? `closing ${reference ? `${reference} ` : ""}net ${sign} $${text}`
    : trigger.source === "option" ? `${reference ?? (side === "buy" ? "ask" : "bid")} ${sign} $${text}` : `${watched} ${sign} ${text}`
  return trigger.trail ? `${base}, trailing ${describeTrail(trigger.trail)}` : base
}
/** "sell 2 @ $4.30 GTC" or "buy 1 at market when SPX ≥ 5,010.00", with any order that cancels it. */
export function describePending(order: PendingOrder, underlying: string): string {
  const what = order.legs ? `${order.quantity} × ${order.legs.length}-leg` : `${order.side ?? "buy"} ${order.quantity}`
  const price = order.type === "limit" && order.limit_price != null ? ` @ $${order.limit_price}` : " at market"
  const deadline = order.time_in_force === "gtd" && order.good_till ? ` until ${timestampET(order.good_till)}` : ""
  const when = order.trigger ? ` when ${describeTrigger(order.trigger, order.side ?? "buy", underlying)}` : ""
  return `${what}${price} ${order.time_in_force.toUpperCase()}${deadline}${when}${order.oco ? `, or ${describePending(order.oco, underlying)}` : ""}`
}
/** How an order is chained: the order that cancels it, the one its fill will place or placed, and the one that placed it. */
export function describeChain(order: Pick<Order, "role" | "oco" | "then" | "chained_order" | "chained_from" | "underlying">): string {
  const parts: string[] = []
  if (order.chained_from) parts.push(`placed by #${order.chained_from}`)
  if (order.oco && !order.role) parts.push(`one cancels other with #${order.oco}`)
  if (order.chained_order) parts.push(`placed #${order.chained_order} when filled`)
  else if (order.then) parts.push(`when filled, places ${describePending(order.then, order.underlying)}`)
  return parts.join(" · ")
}
