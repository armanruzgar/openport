import type { Fill, Order, Position, Side, Trade, WholeTrade } from "../api/trading-types"
import type { ChainRow } from "../api/types"
import { expiryLabel } from "./format"
import { riskProfile, strategyLabel, type RiskProfile, type StrategyLeg } from "./strategy"

const sign = (side: Side) => (side === "buy" ? 1 : -1)
const number = (value: string | null | undefined) => (value == null ? null : Number.isFinite(Number(value)) ? Number(value) : null)
const expiryId = (p: Position) => `${p.expiry}${p.settlement}`
const newYorkHour = new Intl.DateTimeFormat("en-US", { timeZone: "America/New_York", hour: "numeric", hourCycle: "h23" })

/** When a contract settles: 09:30 ET on its date for AM settlement, 16:00 ET for PM. Servers send each
 * position's own `expiry_time`, which is 16:15 for ETF options that trade until then. */
export function settlementTime(date: string, settlement: "AM" | "PM"): number | null {
  const [y, m, d] = date.split("-").map(Number)
  if (!y || !m || !d) return null
  const [hour, minute] = settlement === "AM" ? [9, 30] : [16, 0]
  // New York is four hours behind UTC in daylight time, five otherwise.
  for (const offset of [4, 5]) {
    const time = Date.UTC(y, m - 1, d, hour + offset, minute)
    if (Number(newYorkHour.format(time)) === hour) return time
  }
  return null
}

/** One leg of a held strategy: the position it draws on and how many of its contracts belong here (signed). */
export interface GroupLeg { leg: StrategyLeg; position: Position; contracts: number }

/** Positions opened together by one multi-leg order and still held together. */
export interface StrategyGroup {
  /** The opening order. */
  order: Order
  label: string
  /** "SPX Oct 16 6900/6890 P" style summary of the strikes. */
  title: string
  underlying: string
  units: number
  /** Working closing exits on these held legs, including attached pairs. */
  exits?: Order[]
  legs: GroupLeg[]
  /** Net per unit when opened, debit positive. */
  cost: number | null
  /** Net per unit at the legs' marks; null while any mark is missing. */
  value: number | null
  /** Open P&L before fees: (value − cost) per unit, times units and the multiplier. */
  pnl: number | null
  greeks: { dollar_delta: number | null; dollar_gamma_1pct: number | null; vega_dollars: number | null; theta_dollars: number | null }
  /** At expiry, for legs sharing one expiry; null for calendars and diagonals. */
  profile: RiskProfile | null
  /** The earliest leg expiry id, and when it settles. */
  expiry: string
  expires: number | null
}

function title(underlying: string, legs: StrategyLeg[]): string {
  const expiries = [...new Set(legs.map((l) => l.expiry))]
  const types = new Set(legs.map((l) => l.type))
  const strikes = [...new Set(legs.map((l) => l.strike))].sort((a, b) => b - a).join("/")
  const when = expiries.map((e) => expiryLabel(e)).join(" · ")
  return `${underlying} ${when} ${strikes}${types.size === 1 ? ` ${[...types][0] === "call" ? "C" : "P"}` : ""}`
}

/**
 * Strategies still held: each multi-leg order since `since` (the attempt's start)
 * claims, newest first, as many whole units as the positions still hold on every
 * leg with the order's sides. Positions legged in separately, or strategies
 * partly closed leg by leg, stay single contracts. A lens over the positions:
 * the per-contract table remains the account's record.
 */
export function strategyGroups(positions: readonly Position[], orders: readonly Order[], since: string | null): StrategyGroup[] {
  const held = new Map(positions.filter((p) => !p.awaiting_settlement).map((p) => [p.symbol, p]))
  const remaining = new Map([...held].map(([symbol, p]) => [symbol, p.quantity]))
  const start = since ? Date.parse(since) : NaN
  const combos = orders.filter((o) => o.legs && o.legs.length >= 2 && o.filled_quantity > 0 &&
      (!Number.isFinite(start) || Date.parse(o.accepted_at) >= start))
    .sort((a, b) => Date.parse(b.accepted_at) - Date.parse(a.accepted_at) || Number(b.id) - Number(a.id))
  const groups: StrategyGroup[] = []
  for (const order of combos) {
    let legs = order.legs!
    // A four-leg roll leaves only its new vertical held; rolling one side of a
    // condor leaves the other vertical from its original order.
    const matching = legs.filter((leg) => Math.sign(remaining.get(leg.symbol) ?? 0) === sign(leg.side))
    if (legs.length === 4 && matching.length === 2) {
      const [a, b] = matching
      if (held.get(a!.symbol)?.type === held.get(b!.symbol)?.type && a!.side !== b!.side && a!.ratio === b!.ratio)
        legs = matching
    }
    let units = order.filled_quantity
    for (const leg of legs) {
      const quantity = remaining.get(leg.symbol) ?? 0
      units = Math.sign(quantity) === sign(leg.side) ? Math.min(units, Math.floor(Math.abs(quantity) / leg.ratio)) : 0
    }
    if (units < 1) continue
    const members: GroupLeg[] = legs.map((leg) => {
      const position = held.get(leg.symbol)!
      const contracts = sign(leg.side) * leg.ratio * units
      remaining.set(leg.symbol, (remaining.get(leg.symbol) ?? 0) - contracts)
      return { position, contracts, leg: { symbol: leg.symbol, underlying: position.underlying, side: leg.side, ratio: leg.ratio,
        type: position.type, strike: position.strike, expiry: expiryId(position), quote: null } }
    })
    const strategy = members.map((m) => m.leg)
    const cost = legs.length === order.legs!.length ? number(order.average_fill_price)
      : members.every(({ position }) => number(position.average_price) != null)
        ? members.reduce((total, { leg, position }) => total + sign(leg.side) * leg.ratio * Number(position.average_price), 0) : null
    const marks = members.map((m) => number(m.position.mark))
    const value = marks.every((m) => m != null)
      ? members.reduce((total, m, i) => total + sign(m.leg.side) * m.leg.ratio * marks[i]!, 0) : null
    const share = (m: GroupLeg, x: number | null) => (x == null || m.position.quantity === 0 ? null : x * (m.contracts / m.position.quantity))
    const sum = (pick: (p: Position) => number | null) => {
      const parts = members.map((m) => share(m, pick(m.position)))
      return parts.every((p) => p != null) ? parts.reduce((a, b) => a! + b!, 0) : null
    }
    groups.push({
      order, units, legs: members, cost, value,
      exits: orders.filter((exit) => exit.role && ["working", "partially_filled", "armed"].includes(exit.status) &&
        exit.legs?.length === legs.length && exit.legs.every((leg) => legs.some((entry) =>
          entry.symbol === leg.symbol && entry.side !== leg.side && entry.ratio === leg.ratio))),
      label: strategyLabel(strategy),
      title: title(members[0]!.position.underlying, strategy),
      underlying: members[0]!.position.underlying,
      pnl: cost != null && value != null ? Math.round((value - cost) * units * 100 * 100) / 100 : null,
      greeks: { dollar_delta: sum((p) => p.greeks.dollar_delta), dollar_gamma_1pct: sum((p) => p.greeks.dollar_gamma_1pct), vega_dollars: sum((p) => p.greeks.vega_dollars), theta_dollars: sum((p) => p.greeks.theta_dollars) },
      profile: cost == null ? null : riskProfile(strategy, units, cost),
      expiry: strategy.map((l) => l.expiry).sort()[0]!,
      expires: Math.min(...members.map(({ position: p }) =>
        (p.expiry_time ? Date.parse(p.expiry_time) : settlementTime(p.expiry, p.settlement)) ?? Infinity)),
    })
  }
  return groups
}

/** The legs that close a held strategy: every leg reversed, per unit. */
export function closingPlan(group: StrategyGroup): { legs: StrategyLeg[]; units: number } {
  return { units: group.units, legs: group.legs.map(({ leg }) => ({ ...leg, side: leg.side === "buy" ? "sell" : "buy" })) }
}

/**
 * A roll as one order: close the strategy's legs and open the same types and sides
 * at a later expiry or new strikes, the same expiry included. One side of a condor
 * rolls as a four-leg order, the whole condor as an eight-leg one (the server takes
 * up to eight legs when no more than four open); the target expiry must list every
 * strike.
 */
export function rollSides(group: StrategyGroup): ("put" | "call")[] {
  return (["put", "call"] as const).filter((type) => {
    const legs = group.legs.filter(({ leg }) => leg.type === type)
    return legs.length === 2 && legs[0]!.leg.side !== legs[1]!.leg.side && legs[0]!.leg.ratio === legs[1]!.leg.ratio
  })
}

export function rollPlan(group: StrategyGroup, target: { id: string; strikes: readonly ChainRow[] }, side?: "put" | "call", strikes?: readonly number[]):
  { legs: StrategyLeg[]; units: number } | { reason: string } {
  if (side) {
    if (!rollSides(group).includes(side)) return { reason: "Choose a held put or call vertical to roll." }
    group = { ...group, legs: group.legs.filter(({ leg }) => leg.type === side) }
  }
  if (group.legs.length > 4) return { reason: "Choose the put or call side to roll." }
  if (new Set(group.legs.map(({ leg }) => leg.expiry)).size > 1) return { reason: "Calendars and diagonals roll leg by leg." }
  if (group.legs.some(({ leg }) => leg.expiry > target.id)) return { reason: "Roll to the same or a later expiry." }
  const opening: StrategyLeg[] = []
  for (const [index, { leg }] of group.legs.entries()) {
    const strike = strikes?.[index] ?? leg.strike
    const quote = target.strikes.find((row) => row.strike === strike)?.[leg.type]
    if (!quote?.symbol) return { reason: `${expiryLabel(target.id)} lists no ${strike} ${leg.type}.` }
    if (!quote.tradable) return { reason: quote.untradable_reason ?? `The ${strike} ${leg.type} is not tradable.` }
    opening.push({ ...leg, strike, symbol: quote.symbol, expiry: target.id, quote })
  }
  if (opening.some((leg, index) => leg.symbol === group.legs[index]!.leg.symbol))
    return { reason: "Choose new strikes for every leg to roll within the same expiry." }
  return { units: group.units, legs: [...closingPlan(group).legs, ...opening] }
}

/** A multi-leg order's round trips, or a whole trade's, as the journal groups them. */
export interface TradeGroup {
  key: string
  /** The opening multi-leg order, or null for a single contract's round trip or a whole trade. */
  order: Order | null
  /** The whole trade (a roll, an adjustment or grouped legs), from the server; absent otherwise. */
  whole?: WholeTrade | null
  label: string
  trades: Trade[]
  status: "open" | "closed"
  opened: string
  closed: string | null
  net: number
  unrealised: number | null
}

/**
 * Round trips grouped by the whole trade they are in, when the server lists it (a
 * roll, an adjustment or legs the account grouped), else by the order that opened
 * them: a strategy's legs become one row, other trades stay alone. Newest first,
 * as the trades arrive.
 */
export function tradeGroups(trades: readonly Trade[], fills: readonly Fill[], orders: readonly Order[], wholes: readonly WholeTrade[] = []): TradeGroup[] {
  const fillOrder = new Map(fills.map((f) => [f.id, f.order_id]))
  const byId = new Map(orders.map((o) => [o.id, o]))
  const byWhole = new Map(wholes.map((w) => [w.id, w]))
  const groups = new Map<string, TradeGroup>()
  for (const trade of trades) {
    const whole = trade.group ? byWhole.get(trade.group) ?? null : null
    const opening = byId.get(fillOrder.get(trade.fills[0] ?? "") ?? "")
    const combo = !whole && opening?.legs && opening.legs.length >= 2 ? opening : null
    const key = whole ? `whole-${whole.id}` : combo ? `order-${combo.id}` : `trade-${trade.id}`
    let group = groups.get(key)
    if (!group) {
      group = { key, order: combo, whole, label: "", trades: [], status: "closed", opened: trade.opened, closed: trade.closed, net: 0, unrealised: 0 }
      groups.set(key, group)
    }
    group.trades.push(trade)
  }
  for (const group of groups.values()) {
    const { trades: members } = group
    group.status = members.some((t) => t.status === "open") ? "open" : "closed"
    group.opened = members.map((t) => t.opened).sort()[0]!
    group.closed = group.status === "open" ? null : members.map((t) => t.closed ?? "").sort().at(-1) ?? null
    group.net = members.reduce((total, t) => total + Number(t.net), 0)
    const open = members.filter((t) => t.status === "open")
    group.unrealised = open.length ? open.reduce<number | null>((total, t) => total == null || t.unrealised == null ? null : total + Number(t.unrealised), 0) : null
    // The order's own sides and ratios name the strategy; the trades supply each leg's terms.
    const bySymbol = new Map(members.map((t) => [t.symbol, t]))
    const legs: StrategyLeg[] = (group.order?.legs ?? []).flatMap((leg) => {
      const t = bySymbol.get(leg.symbol)
      return t ? [{ symbol: t.symbol, underlying: t.underlying, side: leg.side, ratio: leg.ratio, type: t.type, strike: t.strike,
        expiry: `${t.expiry}${t.settlement}`, quote: null }] : []
    })
    group.label = legs.length ? `${strategyLabel(legs)} · ${title(members[0]!.underlying, legs)}` : ""
    if (group.whole) {
      const all: StrategyLeg[] = members.map((t) => ({ symbol: t.symbol, underlying: t.underlying, side: t.direction === "long" ? "buy" : "sell",
        ratio: 1, type: t.type, strike: t.strike, expiry: `${t.expiry}${t.settlement}`, quote: null }))
      group.label = `Whole trade · ${title(members[0]!.underlying, all)}`
    }
  }
  return [...groups.values()]
}
