import type { Decision, Fill, Money, Order, OrderChange, OrderChangeRecord, Position } from "../api/trading-types"
import { formatDuration } from "./journal"
import { compareMoney, formatMoney } from "./trading"

export const isOpen = (order: Order) => order.status === "working" || order.status === "partially_filled" || order.status === "armed"

/** Resting orders the trader placed can change: DAY limit orders, armed orders and bracket exits. */
export function editable(order: Order): boolean {
  return isOpen(order) && order.origin !== "system" &&
    (order.status === "armed" || (order.type === "limit" && order.time_in_force !== "ioc"))
}

/**
 * The terms an order can change: its size (a bracket exit's up to the position it
 * protects), its limit, an armed order's trigger, and a resting limit entry's time
 * in force between DAY and GTC (bracket exits are good until expiry).
 */
export function editableFields(order: Order) {
  return { quantity: !order.reduce_only, limit: order.type === "limit",
    trigger: order.status === "armed" && order.trigger != null && order.trigger.source !== "time",
    tif: !order.role && order.type === "limit" && (order.time_in_force === "day" || order.time_in_force === "gtc") }
}

export interface OrderDraft { quantity: string; limit_price: string; trigger_level: string; time_in_force?: "day" | "gtc" }

export function orderDraft(order: Order): OrderDraft {
  return { quantity: String(order.quantity), limit_price: order.limit_price ?? "", trigger_level: order.trigger?.level ?? "",
    time_in_force: order.time_in_force === "gtc" ? "gtc" : order.time_in_force === "day" ? "day" : undefined }
}

const decimalText = /^-?\d+(\.\d{1,6})?$/

/** The terms that changed, the first problem with the draft, or nothing to send. */
export function orderChange(order: Order, draft: OrderDraft): { change: OrderChange } | { error: string } | { unchanged: true } {
  const fields = editableFields(order)
  const change: OrderChange = {}
  if (fields.quantity) {
    const quantity = Number(draft.quantity)
    if (!/^\d+$/.test(draft.quantity.trim()) || !Number.isSafeInteger(quantity) || quantity <= 0)
      return { error: `Enter a whole number of ${order.legs ? "units" : "contracts"}` }
    if (quantity <= order.filled_quantity)
      return { error: `Keep more than the ${order.filled_quantity} already filled, or cancel the rest of the order` }
    if (quantity !== order.quantity) change.quantity = quantity
  }
  if (fields.limit) {
    const text = draft.limit_price.trim()
    if (!decimalText.test(text)) return { error: "Enter a limit price" }
    if (!order.legs && Number(text) <= 0) return { error: "The limit price must be positive" }
    if (compareMoney(text, order.limit_price) !== 0) change.limit_price = text
  }
  if (fields.trigger) {
    const text = draft.trigger_level.trim()
    if (!decimalText.test(text) || (order.trigger?.source !== "combo" && Number(text) <= 0)) return { error: order.trigger?.source === "combo" ? "Enter a signed combo net level" : "Enter a positive trigger level" }
    if (compareMoney(text, order.trigger?.level) !== 0) change.trigger_level = text
  }
  if (fields.tif && draft.time_in_force && draft.time_in_force !== order.time_in_force) change.time_in_force = draft.time_in_force
  return Object.keys(change).length ? { change } : { unchanged: true }
}

/** The side and size of the order that closes a position: "Sell 2" for a long, "Buy 1" for a short. */
export const closingAction = (position: Position) => `${position.quantity > 0 ? "Sell" : "Buy"} ${Math.abs(position.quantity)}`

/** The open orders on one underlying, or on every one. */
export function openOrdersIn(orders: readonly Order[], underlying: string | null): Order[] {
  return orders.filter((o) => isOpen(o) && (underlying == null || o.underlying === underlying))
}

/**
 * What flattening does, as the server will: cancel the open orders in scope but the
 * bracket exits (`exits` of them, kept until the position they protect is flat) and
 * the account's own closes, then close each position in scope that can still trade,
 * short positions first.
 */
export function flattenPlan(positions: readonly Position[], orders: readonly Order[], underlying: string | null) {
  const inScope = (symbol: string) => underlying == null || symbol === underlying
  const closing = positions.filter((p) => p.quantity !== 0 && !p.awaiting_settlement && inScope(p.underlying))
    .sort((a, b) => Number(a.quantity > 0) - Number(b.quantity > 0))
  const open = openOrdersIn(orders, underlying)
  const cancelling = open.filter((o) => o.role == null && o.origin !== "system")
  return { closing, cancelling, exits: open.filter((o) => o.role != null).length }
}

/** Underlyings with something to act on, in the order they first appear. */
export function underlyingsOf(items: readonly { underlying: string }[]): string[] {
  return [...new Set(items.map((item) => item.underlying))]
}

/** Checks whose evidence is in dollars; the others count contracts, dollar delta or vega. */
const dollarChecks = new Set(["DAILY_LOSS", "BUYING_POWER", "PRICE_BAND", "PROFIT_LOCK", "SOFT_FLOOR", "INVALID_PAYOUT", "MAX_TRADE_RISK"])
const units: Record<string, string> = { MAX_ORDER_CONTRACTS: " contracts", DELTA_LIMIT: " dollar delta", VEGA_LIMIT: " vega", TRADE_LIMIT: " opening trades", COOLDOWN: " minutes" }
/** The check behind a reason: a RISK_CHANGED cancel names it at the start of its message. */
export function checkCode(reason: Decision): string {
  if (reason.code !== "RISK_CHANGED") return reason.code
  const named = /^([A-Z_]+):/.exec(reason.message)
  return named ? named[1]! : reason.code
}
/** A reason's numbers: "$15.65 against a limit of $15.64 · aggregate", or null without any. */
export function reasonEvidence(reason: Decision | null | undefined): string | null {
  if (!reason) return null
  const code = checkCode(reason)
  const value = (n: number | string) => typeof n === "string" ? n : dollarChecks.has(code) ? formatMoney(n.toFixed(2))
    : `${n.toLocaleString("en-US", { maximumFractionDigits: 2 })}${units[code] ?? ""}`
  const parts: string[] = []
  if (reason.actual != null && reason.limit != null) parts.push(`${value(reason.actual)} against a limit of ${value(reason.limit)}`)
  else if (reason.actual != null) parts.push(value(reason.actual))
  else if (reason.limit != null) parts.push(`Limit ${value(reason.limit)}`)
  if (reason.scope) parts.push(reason.scope)
  return parts.length ? parts.join(" · ") : null
}

/** A closing order's outcome: "Filled 2 at $4.00", or how much filled and why the rest did not, with its numbers. */
export function outcome(order: Order): string {
  if (order.status === "filled") return `Filled ${order.filled_quantity} at ${formatMoney(order.average_fill_price)}`
  if (order.reduce_only && (order.status === "working" || order.status === "partially_filled"))
    return `${order.filled_quantity ? `Filled ${order.filled_quantity} of ${order.quantity} at ${formatMoney(order.average_fill_price)}; ` : ""}working the rest on later quotes`
  if (order.status === "rejected" || order.status === "cancelled") {
    const evidence = reasonEvidence(order.reason)
    return `${order.filled_quantity ? `Filled ${order.filled_quantity} of ${order.quantity}, then ` : ""}${order.status === "rejected" ? "rejected" : "cancelled"}${order.reason ? `: ${order.reason.message}` : ""}${evidence ? ` (${evidence})` : ""}`
  }
  return order.status
}

export interface TimelineEntry { time: string | null; text: string; detail?: string; tone?: "negative" }
/** A price as the order states it: a multi-leg net is a debit ("db") or a credit ("cr"). */
function termPrice(value: Money | null, multi: boolean): string {
  if (value == null) return "none"
  if (!multi) return formatMoney(value)
  const n = Number(value)
  return n === 0 ? "even" : `${formatMoney(value.replace("-", ""))} ${n > 0 ? "db" : "cr"}`
}
/** The terms a change asked for, each beside the one before: "quantity 3 → 5, limit $4.00 → $4.10". */
export function changeText(change: OrderChangeRecord, multi = false): string {
  const parts: string[] = []
  if (change.quantity != null) parts.push(`quantity ${change.previous.quantity} → ${change.quantity}`)
  if (change.limit_price != null) parts.push(`limit ${termPrice(change.previous.limit_price, multi)} → ${termPrice(change.limit_price, multi)}`)
  if (change.trigger_level != null)
    parts.push(`trigger ${termPrice(change.previous.trigger_level, multi)} → ${termPrice(change.trigger_level, multi)}`)
  if (change.time_in_force) parts.push(`${(change.previous.time_in_force ?? "?").toUpperCase()} → ${change.time_in_force.toUpperCase()}`)
  if (change.walk !== undefined) parts.push(change.walk ? `walk ${formatMoney(change.walk.step)} every ${change.walk.seconds}s to ${termPrice(change.walk.limit, multi)}` : "walk removed")
  return parts.length ? parts.join(", ") : "no new terms"
}
function ending(order: Order): string {
  if (order.status === "filled") return "Filled"
  if (order.status === "rejected") return "Rejected"
  return order.reason?.code === "USER_CANCEL" ? "Cancelled by you" : `Cancelled${order.reason ? ` (${order.reason.code})` : ""}`
}
const at = (time: string | null) => time == null ? Number.POSITIVE_INFINITY : Date.parse(time)
/**
 * An order's life in time order: accepted, triggered, each change asked of it
 * (applied, or refused and why) and how it ended; a working order ends with the
 * time it lasts until.
 */
export function orderTimeline(order: Order): TimelineEntry[] {
  const multi = order.legs != null
  const reason = order.reason ? `${order.reason.message}${reasonEvidence(order.reason) ? ` (${reasonEvidence(order.reason)})` : ""}` : undefined
  if (order.status === "rejected") return [{ time: order.ended_at ?? order.accepted_at, text: "Rejected", detail: reason, tone: "negative" }]
  const entries: TimelineEntry[] = [{ time: order.accepted_at, text: "Accepted" }]
  if (order.triggered_at) entries.push({ time: order.triggered_at, text: "Triggered" })
  for (const change of order.changes ?? []) {
    entries.push(change.applied
      ? { time: change.time, text: `Changed: ${changeText(change, multi)}`, detail: `by ${change.actor}` }
      : { time: change.time, text: `Change refused: ${changeText(change, multi)}`, tone: "negative",
          detail: change.reason ? `${change.reason.code}: ${change.reason.message}${reasonEvidence(change.reason) ? ` (${reasonEvidence(change.reason)})` : ""}` : undefined })
  }
  entries.sort((a, b) => at(a.time) - at(b.time))
  if (!isOpen(order)) entries.push({ time: order.ended_at ?? null, text: ending(order), detail: reason, tone: order.status === "filled" ? undefined : "negative" })
  else if (order.day_end) entries.push({ time: order.day_end, text: "Lasts until, unless it fills or is cancelled" })
  return entries
}

/** The book a fill traded against, as its bid × ask and a detail line, or null when the server did not record it. */
export function fillBook(fill: Fill): { book: string; detail: string } | null {
  const q = fill.quote
  if (!q) return null
  const taken = fill.side === "buy" ? q.ask_size : q.bid_size
  return {
    book: `${formatMoney(q.bid)} × ${formatMoney(q.ask)}`,
    detail: `${q.bid_size} × ${q.ask_size} · ${q.size_left} of ${taken} left · quoted ${formatDuration(q.age_seconds)} before`,
  }
}
