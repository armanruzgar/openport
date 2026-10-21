import { describe, expect, it } from "vitest"
import type { Order, Position } from "../api/trading-types"
import { fill, order, portfolio } from "../test/trading-fixtures"
import { changeText, checkCode, closingAction, orderTimeline, editable, editableFields, fillBook, flattenPlan, orderChange, orderDraft, outcome, reasonEvidence, underlyingsOf } from "./orders"

const stop: Order = { ...order, id: "9", status: "armed", role: "stop_loss", side: "sell", type: "market", time_in_force: "ioc",
  limit_price: null, quantity: 2, filled_quantity: 0, remaining_quantity: 2, trigger: { source: "option", direction: "at_or_below", level: "3.50" } }
const spread: Order = { ...order, id: "10", symbol: null, side: null, limit_price: "-1.20", filled_quantity: 0, quantity: 2,
  legs: [{ symbol: "SPXW  261016P06900000", side: "sell", ratio: 1 }, { symbol: "SPXW  261016P06890000", side: "buy", ratio: 1 }] }

describe("order edits", () => {
  it("apply to resting orders the trader placed", () => {
    expect(editable(order)).toBe(true)  // a partially filled DAY limit
    expect(editable(stop)).toBe(true)
    expect(editable({ ...order, time_in_force: "gtc" })).toBe(true)
    expect(editable({ ...order, status: "filled" })).toBe(false)
    expect(editable({ ...order, time_in_force: "ioc" })).toBe(false)
    expect(editable({ ...order, type: "market", limit_price: null })).toBe(false)
    expect(editable({ ...order, origin: "system" })).toBe(false)
    expect(editableFields(order)).toEqual({ quantity: true, limit: true, trigger: false, tif: true })
    // A bracket exit resizes up to its position and keeps its time in force.
    expect(editableFields(stop)).toEqual({ quantity: true, limit: false, trigger: true, tif: false })
  })

  it("send only what changed, and explain what cannot", () => {
    const draft = orderDraft(order)
    expect(orderChange(order, draft)).toEqual({ unchanged: true })
    expect(orderChange(order, { ...draft, limit_price: "4.600" })).toEqual({ unchanged: true })
    expect(orderChange(order, { ...draft, limit_price: "4.70" })).toEqual({ change: { limit_price: "4.70" } })
    expect(orderChange(order, { ...draft, quantity: "8", limit_price: "4.50" })).toEqual({ change: { quantity: 8, limit_price: "4.50" } })
    expect(orderChange(order, { ...draft, quantity: "2" })).toEqual({ error: "Keep more than the 2 already filled, or cancel the rest of the order" })
    expect(orderChange(order, { ...draft, quantity: "2.5" })).toEqual({ error: "Enter a whole number of contracts" })
    expect(orderChange(order, { ...draft, limit_price: "0" })).toEqual({ error: "The limit price must be positive" })
    expect(orderChange(order, { ...draft, limit_price: "abc" })).toEqual({ error: "Enter a limit price" })
    expect(orderChange(stop, { ...orderDraft(stop), trigger_level: "3.8" })).toEqual({ change: { trigger_level: "3.8" } })
    expect(orderChange(stop, { ...orderDraft(stop), trigger_level: "0" })).toEqual({ error: "Enter a positive trigger level" })
    // A multi-leg order's net limit is negative for a credit.
    expect(orderChange(spread, { ...orderDraft(spread), limit_price: "-1.10" })).toEqual({ change: { limit_price: "-1.10" } })
    expect(orderChange(spread, { ...orderDraft(spread), quantity: "x" })).toEqual({ error: "Enter a whole number of units" })
    // A resting limit switches between DAY and GTC in place.
    expect(orderChange(order, { ...draft, time_in_force: "gtc" })).toEqual({ change: { time_in_force: "gtc" } })
    expect(orderChange(order, { ...draft, time_in_force: "day" })).toEqual({ unchanged: true })
    expect(changeText({ time: "", actor: "web", quantity: null, limit_price: null, trigger_level: null, time_in_force: "gtc",
      previous: { quantity: 1, limit_price: "3.90", trigger_level: null, time_in_force: "day" }, applied: true, reason: null })).toBe("DAY → GTC")
  })
})

describe("flatten", () => {
  const long = portfolio.positions[0]!
  const short: Position = { ...long, symbol: "SPXW  261016P06900000", type: "put", strike: 6900, quantity: -1 }
  const xsp: Position = { ...long, symbol: "XSP   261016C00700000", underlying: "XSP", quantity: 3 }
  const expired = portfolio.positions[1]!  // awaiting settlement

  it("closes short positions first, keeps the exits and leaves expired ones to settle", () => {
    const system: Order = { ...order, id: "12", origin: "system", type: "market", time_in_force: "ioc", limit_price: null }
    const plan = flattenPlan([long, expired, short, xsp], [order, stop, { ...order, id: "11", underlying: "XSP" }, system], null)
    expect(plan.closing.map((p) => p.symbol)).toEqual([short.symbol, long.symbol, xsp.symbol])
    // Bracket exits stay until the position is flat, and the account's own closes keep working.
    expect(plan.cancelling.map((o) => o.id)).toEqual(["order-1", "11"])
    expect(plan.exits).toBe(1)
    expect(closingAction(short)).toBe("Buy 1")
    expect(closingAction(long)).toBe("Sell 2")
  })

  it("can take one underlying at a time", () => {
    const plan = flattenPlan([long, short, xsp], [order, { ...order, id: "11", underlying: "XSP" }], "XSP")
    expect(plan.closing).toEqual([xsp])
    expect(plan.cancelling.map((o) => o.id)).toEqual(["11"])
    expect(underlyingsOf([long, short, xsp])).toEqual(["SPX", "XSP"])
  })

  it("reports each closing order's outcome", () => {
    expect(outcome({ ...order, status: "filled", filled_quantity: 2, average_fill_price: "4.00" })).toBe("Filled 2 at $4.00")
    expect(outcome({ ...order, status: "rejected", filled_quantity: 0, reason: { code: "PRICE_BAND", message: "Price is outside the configured band around mid" } }))
      .toBe("rejected: Price is outside the configured band around mid")
    expect(outcome({ ...order, status: "cancelled", quantity: 2, filled_quantity: 1, reason: { code: "IOC_REMAINDER", message: "IOC exhausted available displayed liquidity" } }))
      .toBe("Filled 1 of 2, then cancelled: IOC exhausted available displayed liquidity")
    // A refused Flatten leg shows which bucket blocked it, and by how much.
    expect(outcome({ ...order, status: "rejected", filled_quantity: 0, reason: { code: "DELTA_LIMIT",
      message: "Worst reachable absolute dollar delta exceeds limit", actual: 878617.4, limit: 700000, scope: "SPX" } }))
      .toBe("rejected: Worst reachable absolute dollar delta exceeds limit (878,617.4 dollar delta against a limit of 700,000 dollar delta · SPX)")
    // A flatten's close works the rest on later quotes.
    expect(outcome({ ...order, reduce_only: true, status: "partially_filled", quantity: 5, filled_quantity: 2, average_fill_price: "4.00" }))
      .toBe("Filled 2 of 5 at $4.00; working the rest on later quotes")
    expect(outcome({ ...order, reduce_only: true, status: "working", quantity: 5, filled_quantity: 0 })).toBe("working the rest on later quotes")
  })

  it("reads a reason's numbers, a RISK_CHANGED cancel by the check it names", () => {
    const changed = { code: "RISK_CHANGED", message: "DAILY_LOSS: Marked loss from start-of-day equity exceeds limit", actual: 15.65, limit: 15.64, scope: "aggregate" }
    expect(checkCode(changed)).toBe("DAILY_LOSS")
    expect(reasonEvidence(changed)).toBe("$15.65 against a limit of $15.64 · aggregate")
    expect(reasonEvidence({ code: "MAX_ORDER_CONTRACTS", message: "", actual: 3, limit: 2, scope: "SPX" })).toBe("3 contracts against a limit of 2 contracts · SPX")
    expect(reasonEvidence({ code: "MAX_TRADE_RISK", message: "", actual: 512.5, limit: 500, scope: "trade" })).toBe("$512.50 against a limit of $500.00 · trade")
    // Older servers send neither numbers nor scope.
    expect(reasonEvidence({ code: "USER_CANCEL", message: "Cancelled by caller" })).toBeNull()
    expect(reasonEvidence(null)).toBeNull()
  })
})

describe("an order's history", () => {
  const changed: Order = { ...order, status: "cancelled", reason: { code: "DAY_END", message: "The order's session ended" },
    accepted_at: "2026-09-22T14:00:00Z", ended_at: "2026-09-22T20:15:00Z", changes: [
      { time: "2026-09-22T14:05:00Z", actor: "trader", quantity: null, limit_price: "4.70", trigger_level: null,
        previous: { quantity: 5, limit_price: "4.60", trigger_level: null }, applied: true, reason: null },
      { time: "2026-09-22T14:10:00Z", actor: "trader", quantity: 2, limit_price: null, trigger_level: null,
        previous: { quantity: 5, limit_price: "4.70", trigger_level: null }, applied: false,
        reason: { code: "INVALID_ORDER", message: "The new quantity must exceed the filled quantity; cancel the order instead", actual: 2, limit: 2, scope: null } },
    ] }
  it("lists acceptance, each change and refusal, and the end with its reason", () => {
    const timeline = orderTimeline(changed)
    expect(timeline.map((entry) => entry.text)).toEqual(["Accepted", "Changed: limit $4.60 → $4.70",
      "Change refused: quantity 5 → 2", "Cancelled (DAY_END)"])
    expect(timeline[2]!.detail).toBe("INVALID_ORDER: The new quantity must exceed the filled quantity; cancel the order instead (2 against a limit of 2)")
    expect(timeline[3]!.time).toBe("2026-09-22T20:15:00Z")
  })
  it("ends a working order with when it lasts until, and names a multi-leg net", () => {
    expect(orderTimeline(order).at(-1)).toEqual({ time: order.day_end, text: "Lasts until, unless it fills or is cancelled" })
    expect(changeText({ ...changed.changes![0]!, limit_price: "-1.10", previous: { quantity: 1, limit_price: "-1.20", trigger_level: null } }, true))
      .toBe("limit $1.20 cr → $1.10 cr")
    // Older servers send no changes or end time.
    expect(orderTimeline({ ...order, status: "filled" }).at(-1)).toMatchObject({ time: null, text: "Filled" })
  })
})

describe("fill quotes", () => {
  it("show the book a fill took, the paper size left on its side and how old the quote was", () => {
    expect(fillBook(fill)).toBeNull()
    const quote = { observation: "7", bid: "4.50", ask: "4.60", bid_size: 10, ask_size: 3, size_left: 2, quoted_at: fill.time, age_seconds: 75 }
    expect(fillBook({ ...fill, quote })).toEqual({ book: "$4.50 × $4.60", detail: "10 × 3 · 2 of 3 left · quoted 1m 15s before" })
    expect(fillBook({ ...fill, side: "sell", quote: { ...quote, bid: null, age_seconds: 0 } }))
      .toEqual({ book: "— × $4.60", detail: "10 × 3 · 2 of 10 left · quoted 0s before" })
  })
})

it("lets a limit flatten change price while keeping its quantity tied to the position", () => {
  const close: typeof order = { ...order, reduce_only: true, type: "limit", time_in_force: "exto", limit_ticks: 1 }
  expect(editable(close)).toBe(true)
  expect(editableFields(close)).toEqual({ quantity: false, limit: true, trigger: false, tif: false })
})
