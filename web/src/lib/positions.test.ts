import { describe, expect, it } from "vitest"
import type { Fill, Order, Position, Trade, WholeTrade } from "../api/trading-types"
import type { ChainRow } from "../api/types"
import { order, portfolio, quote, trades } from "../test/trading-fixtures"
import { contractLabel, journalLabel, journalStats, tradeBuckets, wholeEntries } from "./journal"
import { closingPlan, rollPlan, settlementTime, strategyGroups, tradeGroups } from "./positions"

const base = portfolio.positions[0]!
const greeks = { delta: null, gamma: null, vega: null, theta: null, dollar_delta: -200, dollar_gamma_1pct: null, vega_dollars: 10, theta_dollars: 20 }
function put(strike: number, quantity: number, mark: string): Position {
  return { ...base, symbol: `SPXW  261016P0${strike}000`, type: "put", strike, quantity, mark, greeks: { ...greeks, dollar_delta: quantity * 100 } }
}
function combo(id: string, legs: [number, "buy" | "sell"][], units: number, price: string, at = "2026-09-23T15:00:00Z"): Order {
  return { ...order, id, symbol: null, side: null, status: "filled", quantity: units, filled_quantity: units, remaining_quantity: 0,
    limit_price: price, average_fill_price: price, accepted_at: at,
    legs: legs.map(([strike, side]) => ({ symbol: `SPXW  261016P0${strike}000`, side, ratio: 1 })) }
}

describe("held strategies", () => {
  const short = put(6900, -2, "3.00")
  const long = put(6890, 2, "2.50")

  it("group the positions one multi-leg order opened, with net value and P&L", () => {
    const [spread] = strategyGroups([short, long], [combo("7", [[6900, "sell"], [6890, "buy"]], 2, "-1.20")], null)
    expect(spread).toBeDefined()
    expect(spread!.label).toBe("Bull put spread")
    expect(spread!.title).toBe("SPX Oct 16 6900/6890 P")
    expect(spread!.units).toBe(2)
    expect(spread!.cost).toBe(-1.2)
    expect(spread!.value).toBeCloseTo(-0.5, 9)  // short 3.00, long 2.50 per unit
    expect(spread!.pnl).toBe(140)  // (−0.50 − −1.20) × 2 units × 100
    expect(spread!.greeks.dollar_delta).toBe(0)  // −200 and +200, all of both positions
    expect(spread!.profile).toEqual({ maxProfit: 240, maxLoss: 1760, breakevens: [6898.8] })
    expect(spread!.expires).toBe(Date.parse("2026-10-16T20:00:00Z"))
    // The server's own expiry wins: ETF options such as SPY's trade until 16:15.
    const late = (p: typeof short) => ({ ...p, expiry_time: "2026-10-16T20:15:00Z" })
    const [etf] = strategyGroups([late(short), late(long)], [combo("7", [[6900, "sell"], [6890, "buy"]], 2, "-1.20")], null)
    expect(etf!.expires).toBe(Date.parse("2026-10-16T20:15:00Z"))
  })

  it("keep what is still held together, sharing a contract between strategies", () => {
    const orders = [
      combo("7", [[6900, "sell"], [6890, "buy"]], 1, "-1.20", "2026-09-23T15:00:00Z"),
      combo("8", [[6900, "sell"], [6880, "buy"]], 1, "-1.60", "2026-09-23T15:05:00Z"),
    ]
    const groups = strategyGroups([put(6900, -2, "3.00"), put(6890, 1, "2.50"), put(6880, 1, "2.10")], orders, null)
    expect(groups.map((g) => [g.order.id, g.units])).toEqual([["8", 1], ["7", 1]])
    expect(groups[1]!.greeks.dollar_delta).toBe(-100 + 100)  // half the short's delta, all of the long's
  })

  it("leave partly closed or reversed strategies as single contracts", () => {
    const opened = combo("7", [[6900, "sell"], [6890, "buy"]], 2, "-1.20")
    // One unit of the long leg sold separately: one unit is still a spread.
    expect(strategyGroups([short, put(6890, 1, "2.50")], [opened], null).map((g) => g.units)).toEqual([1])
    // Sides no longer match the order's.
    expect(strategyGroups([put(6900, 2, "3.00"), long], [opened], null)).toEqual([])
    // Orders from an earlier attempt do not count.
    expect(strategyGroups([short, long], [opened], "2026-09-23T16:00:00Z")).toEqual([])
    // Expired positions wait for settlement.
    expect(strategyGroups([{ ...short, awaiting_settlement: true }, long], [opened], null)).toEqual([])
  })

  it("close every leg reversed, and roll up to two legs to a later expiry", () => {
    const [spread] = strategyGroups([short, long], [combo("7", [[6900, "sell"], [6890, "buy"]], 2, "-1.20")], null)
    const close = closingPlan(spread!)
    expect(close.units).toBe(2)
    expect(close.legs.map((l) => `${l.side} ${l.strike}`)).toEqual(["buy 6900", "sell 6890"])
    const row = (strike: number, tradable = true): ChainRow => ({ strike, iv: 0.2, gex: null, vex: null, call: null,
      put: { ...quote, symbol: `SPXW  261023P0${strike}000`, tradable, untradable_reason: tradable ? null : "Too far out" } })
    const roll = rollPlan(spread!, { id: "2026-10-23PM", strikes: [row(6890), row(6900)] })
    expect("legs" in roll && roll.legs.map((l) => `${l.side} ${l.strike} ${l.expiry}`)).toEqual([
      "buy 6900 2026-10-16PM", "sell 6890 2026-10-16PM", "sell 6900 2026-10-23PM", "buy 6890 2026-10-23PM"])
    expect(rollPlan(spread!, { id: "2026-10-23PM", strikes: [row(6900)] })).toEqual({ reason: "Oct 23 lists no 6890 put." })
    expect(rollPlan(spread!, { id: "2026-10-23PM", strikes: [row(6890), row(6900, false)] })).toEqual({ reason: "Too far out" })
    expect(rollPlan(spread!, { id: "2026-10-09PM", strikes: [] })).toEqual({ reason: "Roll to the same or a later expiry." })
    // Within the same expiry, a roll moves every strike.
    const same = (strike: number): ChainRow => ({ ...row(strike), put: { ...row(strike).put!, symbol: `SPXW  261016P0${strike}000` } })
    expect(rollPlan(spread!, { id: "2026-10-16PM", strikes: [same(6900), same(6890)] })).toEqual({ reason: "Choose new strikes for every leg to roll within the same expiry." })
    const down = rollPlan(spread!, { id: "2026-10-16PM", strikes: [same(6850), same(6840)] }, undefined, [6850, 6840])
    expect("legs" in down && down.legs.map((l) => `${l.side} ${l.strike} ${l.expiry}`)).toEqual([
      "buy 6900 2026-10-16PM", "sell 6890 2026-10-16PM", "sell 6850 2026-10-16PM", "buy 6840 2026-10-16PM"])
  })

  it("settle AM contracts at the 09:30 open and PM ones at the 16:00 close, New York time", () => {
    expect(settlementTime("2026-10-16", "PM")).toBe(Date.parse("2026-10-16T20:00:00Z"))
    expect(settlementTime("2026-12-18", "AM")).toBe(Date.parse("2026-12-18T14:30:00Z"))
    expect(settlementTime("bad", "PM")).toBeNull()
  })
})

describe("strategies in the journal", () => {
  const leg = (id: string, strike: number, direction: "long" | "short", fill: string, status: "open" | "closed", net: string): Trade => ({
    ...trades[0]!, id, symbol: `SPXW  261016P0${strike}000`, strike, type: "put", direction, status, fills: [fill], net,
    closed: status === "closed" ? "2026-09-23T17:00:00Z" : null, unrealised: status === "open" ? "15.00" : null })
  const fill = (id: string, order_id: string): Fill => ({ id, order_id, symbol: "", underlying: "SPX", side: "buy", quantity: 1, price: "1.00", fee: "0.65", quote_time: "", time: "" })

  it("group round trips by the multi-leg order that opened them", () => {
    const spread = combo("7", [[6900, "sell"], [6890, "buy"]], 1, "-1.20")
    const list = [leg("a", 6900, "short", "f1", "closed", "100.00"), leg("b", 6890, "long", "f2", "open", "0"), leg("c", 6800, "long", "f3", "closed", "-40.00")]
    const groups = tradeGroups(list, [fill("f1", "7"), fill("f2", "7"), fill("f3", "9")], [spread, { ...order, id: "9" }])
    expect(groups.map((g) => g.key)).toEqual(["order-7", "trade-c"])
    expect(groups[0]!.label).toBe("Bull put spread · SPX Oct 16 6900/6890 P")
    expect(groups[0]!.status).toBe("open")
    expect(groups[0]!.net).toBe(100)
    expect(groups[0]!.unrealised).toBe(15)
    expect(groups[1]!.order).toBeNull()
    expect(groups[1]!.net).toBe(-40)
  })

  it("group a rolled trade's round trips as one whole trade, and count it once", () => {
    const spread = combo("7", [[6900, "sell"], [6890, "buy"]], 1, "-1.20")
    const roll = combo("8", [[6900, "buy"], [6890, "sell"], [6850, "sell"], [6840, "buy"]], 1, "-0.40")
    const list = [
      { ...leg("d", 6840, "long", "f6", "closed", "-10.00"), group: "a", trading_day: "2026-09-24" },
      { ...leg("c", 6850, "short", "f5", "closed", "60.00"), group: "a", trading_day: "2026-09-24" },
      { ...leg("b", 6890, "long", "f2", "closed", "-30.00"), group: "a", trading_day: "2026-09-23" },
      { ...leg("a", 6900, "short", "f1", "closed", "-20.00"), group: "a", trading_day: "2026-09-23" },
      { ...leg("e", 6800, "long", "f7", "closed", "5.00"), group: "e" },
    ]
    const whole: WholeTrade = { id: "a", attempt: 1, underlying: "SPX", status: "closed", opened: list[3]!.opened, closed: "2026-09-24T17:00:00Z",
      trading_day: "2026-09-24", round_trips: ["a", "b", "c", "d"], entries: 2, gross: "2.60", fees: "2.60", net: "0.00", unrealised: null,
      review: null, review_since: null }
    const fills = [fill("f1", "7"), fill("f2", "7"), fill("f5", "8"), fill("f6", "8"), fill("f7", "9")]
    const groups = tradeGroups(list, fills, [spread, roll, { ...order, id: "9" }], [whole])
    expect(groups.map((g) => g.key)).toEqual(["whole-a", "trade-e"])
    expect(groups[0]!.whole).toBe(whole)
    expect(groups[0]!.order).toBeNull()
    expect(groups[0]!.trades).toHaveLength(4)
    expect(groups[0]!.label).toBe("Whole trade · SPX Oct 16 6900/6890/6850/6840 P")
    // Older servers list no whole trades: the strategies stay apart.
    expect(tradeGroups(list, fills, [spread, roll, { ...order, id: "9" }]).map((g) => g.key)).toEqual(["order-8", "order-7", "trade-e"])
    const entries = wholeEntries(groups)
    expect(entries).toHaveLength(2)
    expect(entries[0]!.id).toBe("a")
    expect(entries[0]!.net).toBe("0.00")
    expect(entries[0]!.status).toBe("closed")
    expect(entries[0]!.trading_day).toBe("2026-09-24")
    expect(entries[1]).toBe(list[4])
    const stats = journalStats([entries[0]!])
    expect(journalLabel(stats.best!)).toBe("Whole trade · SPX Oct 16 6900/6890/6850/6840 P")
    expect(journalLabel(stats.worst!)).toBe(groups[0]!.label)
    expect(journalLabel(entries[1]!)).toBe(contractLabel(list[4]!))
    expect(tradeBuckets([entries[0]!], "duration", "put").reduce((n, b) => n + b.trades, 0)).toBe(1)
    expect(tradeBuckets([entries[0]!], "duration", "call").every((b) => b.trades === 0)).toBe(true)
  })

  it.each([false, true])("counts a condor only under All, regardless of leg order (whole: %s)", (whole) => {
    const opening = combo("7", [[6900, "sell"], [6890, "buy"]], 1, "-2.00")
    opening.legs!.push(
      { symbol: "SPXW  261016C07100000", side: "sell", ratio: 1 },
      { symbol: "SPXW  261016C07110000", side: "buy", ratio: 1 },
    )
    const list = opening.legs!.map((l, i): Trade => ({
      ...leg(String(i), [6900, 6890, 7100, 7110][i]!, l.side === "buy" ? "long" : "short", `f${i}`, "closed", "10.00"),
      symbol: l.symbol, type: i < 2 ? "put" : "call", expiry: "2026-10-16", group: "0",
    }))
    const fills = list.map((_, i) => fill(`f${i}`, "7"))
    const wholes: WholeTrade[] = whole ? [{ id: "0", attempt: 1, underlying: "SPX", status: "closed",
      opened: list[0]!.opened, closed: list[0]!.closed, trading_day: "2026-09-23", round_trips: list.map((t) => t.id), entries: 2,
      gross: "40.00", fees: "0", net: "40.00", unrealised: null, review: null, review_since: null }] : []
    for (const members of [list, [...list].reverse()]) {
      const groups = tradeGroups(members, fills, [opening], wholes)
      const entries = wholeEntries(groups)
      expect(entries).toHaveLength(1)
      expect(journalLabel(journalStats(entries).best!)).toBe(groups[0]!.label)
      expect(groups[0]!.label).toContain(whole ? "Whole trade" : "Iron condor")
      const count = (items: typeof entries, side: "all" | "call" | "put") =>
        tradeBuckets(items, "duration", side).reduce((n, b) => n + b.trades, 0)
      expect(count(entries, "all")).toBe(1)
      expect(count(entries, "call")).toBe(0)
      expect(count(entries, "put")).toBe(0)
      expect(count(members, "all")).toBe(4)
      expect(count(members, "call")).toBe(2)
      expect(count(members, "put")).toBe(2)
      const calls = wholeEntries(tradeGroups(members.filter((t) => t.type === "call"), fills, [opening], wholes))
      expect(count(calls, "call")).toBe(1)
      expect(count(calls, "put")).toBe(0)
    }
  })
})

it("associates working combo exits with their held strategy", () => {
  const opening = combo("7", [[6900, "sell"], [6890, "buy"]], 2, "-1.20")
  const target: Order = { ...opening, id: "8", status: "working", role: "take_profit", filled_quantity: 0,
    legs: opening.legs!.map((leg) => ({ ...leg, side: leg.side === "buy" ? "sell" : "buy" })) }
  const groups = strategyGroups([put(6900, -2, "3.00"), put(6890, 2, "2.50")], [opening, target], null)
  expect(groups[0]!.exits).toEqual([target])
})

it("groups each annotated combo leg into the strategy's journal row", () => {
  const opening = combo("7", [[6900, "sell"], [6890, "buy"]], 1, "-1.20")
  const annotated = opening.legs!.map((leg, i) => ({ ...trades[0]!, id: String(i + 1), symbol: leg.symbol,
    fills: [String(i + 1)], tags: ["put-credit-10d-5w"], note: "Plan", type: "put" as const }))
  const fills = annotated.map((trade) => ({ id: trade.id, order_id: "7" } as Fill))
  const groups = tradeGroups(annotated, fills, [opening])
  expect(groups).toHaveLength(1)
  expect(groups[0]!.trades.every((trade) => trade.tags?.includes("put-credit-10d-5w") && trade.note === "Plan")).toBe(true)
})

it("keeps both verticals visible after rolling one side of a four-leg strategy", () => {
  const p1 = put(6900, -2, "3.00"), p2 = put(6890, 2, "2.50")
  const call = (p: Position): Position => ({ ...p, symbol: p.symbol.replace("261016P", "261016C"), type: "call" })
  const c1 = call(put(7100, -2, "3.00")), c2 = call(put(7110, 2, "2.50"))
  const original = combo("7", [[6900, "sell"], [6890, "buy"]], 2, "-2.00")
  original.legs!.push({ symbol: c1.symbol, side: "sell", ratio: 1 }, { symbol: c2.symbol, side: "buy", ratio: 1 })
  const next = (p: Position): Position => ({ ...p, expiry: "2026-10-23", symbol: p.symbol.replace("261016", "261023"), average_price: p.quantity < 0 ? "4.00" : "2.00" })
  const n1 = next(p1), n2 = next(p2)
  const roll: Order = { ...original, id: "8", average_fill_price: "-0.30", accepted_at: "2026-09-23T16:00:00Z",
    legs: [{ symbol: p1.symbol, side: "buy", ratio: 1 }, { symbol: p2.symbol, side: "sell", ratio: 1 },
      { symbol: n1.symbol, side: "sell", ratio: 1 }, { symbol: n2.symbol, side: "buy", ratio: 1 }] }
  const groups = strategyGroups([n1, n2, c1, c2], [original, roll], null)
  expect(groups).toHaveLength(2)
  expect(groups[0]!.legs.map((m) => m.leg.symbol)).toEqual([n1.symbol, n2.symbol])
  expect(groups[0]!.cost).toBe(-2) // held basis, never the roll's combined close/open net
  expect(groups[1]!.legs.map((m) => m.leg.symbol)).toEqual([c1.symbol, c2.symbol])
})

it("allocates dollar gamma and vega to held units, preserving missing Greeks", () => {
  const short = { ...put(6900, -4, "3"), greeks: { ...greeks, dollar_gamma_1pct: -400, vega_dollars: -80 } }
  const long = { ...put(6890, 2, "2"), greeks: { ...greeks, dollar_gamma_1pct: 120, vega_dollars: 30 } }
  const opened = combo("7", [[6900, "sell"], [6890, "buy"]], 2, "-1")
  const [group] = strategyGroups([short, long], [opened], null)
  expect(group!.greeks.dollar_gamma_1pct).toBe(-80) // half the short, all the long
  expect(group!.greeks.vega_dollars).toBe(-10)
  const [missing] = strategyGroups([short, { ...long, greeks: { ...long.greeks, dollar_gamma_1pct: null, vega_dollars: null } }], [opened], null)
  expect(missing!.greeks.dollar_gamma_1pct).toBeNull()
  expect(missing!.greeks.vega_dollars).toBeNull()
})
