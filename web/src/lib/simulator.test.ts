import { describe, expect, it } from "vitest"
import type { PendingOrder } from "../api/trading-types"
import { quote, shareTrades, trades } from "../test/trading-fixtures"
import { signedPercent } from "./format"
import { closingDay, contractLabel, dailyResults, formatDuration, journalLabel, journalStats, monthWeeks, newYorkDate, osiLabel, parseOsi, parseTags, shareSourceLabel, tradeBuckets, tradeTags, tradingDate } from "./journal"
import { crossDirection, describeChain, describePending, describeTrigger, marketability, opposite, split, stopDirection, strategyName } from "./ticket"
import { ratio, roundToTick, signedMoney, stepLimitPrice, subtractMoney } from "./trading"

describe("journal analytics", () => {
  it("summarises closed trades only, with exact profit factor and hold time", () => {
    const stats = journalStats(trades)
    expect(stats.trades).toBe(2)
    expect(stats.wins).toBe(1)
    expect(stats.losses).toBe(1)
    expect(stats.winRate).toBe(0.5)
    expect(stats.net).toBeCloseTo(167.2)
    expect(stats.profitFactor).toBeCloseTo(268.5 / 101.3)
    expect(stats.averageWin).toBeCloseTo(268.5)
    expect(stats.averageLoss).toBeCloseTo(-101.3)
    expect(stats.best?.id).toBe("1")
    expect(stats.worst?.id).toBe("2")
    expect(stats.contracts).toBe(10)
    expect(stats.averageHoldSeconds).toBe((289 + 9000) / 2)
    expect(journalStats([trades[2]!]).profitFactor).toBe(Infinity)
    expect(journalStats([]).winRate).toBeNull()
  })
  it("counts share round trips beside the options", () => {
    const both = [...trades, ...shareTrades]
    const stats = journalStats(both)
    expect(stats.trades).toBe(3)
    expect(stats.net).toBeCloseTo(167.2 + 320)
    expect(stats.contracts).toBe(10)
    expect(stats.shares).toBe(100)
    expect(stats.best?.id).toBe("s1")
    expect(journalLabel(stats.best!)).toBe("SPY 100 shares")
    expect(journalLabel({ ...shareTrades[1]!, direction: "short" })).toBe("SPY 100 shares short")
    expect(journalLabel(trades[2]!)).toBe(contractLabel(trades[2]!))
    expect(dailyResults(both).get("2026-09-23")).toEqual({ date: "2026-09-23", net: 320 - 101.3, trades: 2, wins: 1 })
    // Shares are neither calls nor puts, and carry no tags.
    expect(tradeBuckets(both, "month").find((b) => b.label === "Sep")?.trades).toBe(3)
    expect(tradeBuckets(both, "month", "call").find((b) => b.label === "Sep")?.trades).toBe(2)
    expect(tradeBuckets(both, "tag").find((b) => b.label === "untagged")?.trades).toBe(3)
    expect(tradeTags(both)).toEqual([])
    // Shares take tags like any trade.
    const tagged = [...trades, { ...shareTrades[1]!, tags: ["income"] }]
    expect(tradeTags(tagged)).toEqual(["income"])
    expect(tradeBuckets(tagged, "tag").find((b) => b.label === "income")?.trades).toBe(1)
  })
  it("says how shares came and went", () => {
    expect(shareSourceLabel("expiry_exercise", "SPY   260922C00500000", "SPY", "opened", "long")).toBe("Exercised SPY Sep 22 500C at expiry")
    expect(shareSourceLabel("early_exercise", "SPY   261016C00500000", "SPY", "opened", "long")).toBe("Exercised SPY Oct 16 500C early")
    expect(shareSourceLabel("assignment", "QQQ   260923P00480000", "QQQ", "opened", "long")).toBe("Assigned QQQ Sep 23 480P")
    expect(shareSourceLabel("trade", null, "SPY", "closed", "long")).toBe("Sold")
    expect(shareSourceLabel("trade", null, "SPY", "closed", "short")).toBe("Bought")
    expect(shareSourceLabel("rule", null, "SPY", "closed", "long")).toBe("Closed by the evaluation")
    expect(shareSourceLabel("reset", null, "SPY", "closed", "long")).toBe("Account reset")
    expect(shareSourceLabel(null, null, "SPY", "closed", "long")).toBe("—")
  })
  it("groups by the New York close date across the UTC midnight", () => {
    expect(newYorkDate("2026-09-23T03:30:00Z")).toEqual({ date: "2026-09-22", weekday: 2 })
    const days = dailyResults(trades)
    expect([...days.keys()]).toEqual(["2026-09-23", "2026-09-22"])
    expect(days.get("2026-09-22")).toEqual({ date: "2026-09-22", net: 268.5, trades: 1, wins: 1 })
  })
  it("lays out Sunday-first month weeks with padding", () => {
    const weeks = monthWeeks(2026, 9)
    expect(weeks[0]).toEqual([null, null, "2026-09-01", "2026-09-02", "2026-09-03", "2026-09-04", "2026-09-05"])
    expect(weeks.at(-1)).toEqual(["2026-09-27", "2026-09-28", "2026-09-29", "2026-09-30", null, null, null])
    expect(monthWeeks(2026, 2).flat().filter(Boolean)).toHaveLength(28)
  })
  it("buckets by hold time, weekday and month, filtered by calls or puts", () => {
    const duration = tradeBuckets(trades, "duration")
    expect(duration.find((b) => b.label === "1–5m")).toMatchObject({ trades: 1, net: 268.5, winRate: 1 })
    expect(duration.find((b) => b.label === "1–4h")).toMatchObject({ trades: 1, wins: 0, winRate: 0 })
    expect(duration.find((b) => b.label === "< 1m")?.winRate).toBeNull()
    const weekday = tradeBuckets(trades, "weekday")
    expect(weekday.map((b) => b.trades)).toEqual([0, 1, 1, 0, 0])
    expect(tradeBuckets(trades, "month").find((b) => b.label === "Sep")?.trades).toBe(2)
    expect(tradeBuckets(trades, "month", "put").every((b) => b.trades === 0)).toBe(true)
  })
  it("counts an overnight close toward the weekday and month of the session it trades for", () => {
    expect(tradingDate("2026-09-21T01:00:00Z")).toEqual({ date: "2026-09-21", weekday: 1 })  // Sunday 21:00 ET
    expect(tradingDate("2026-09-22T20:59:00Z")).toEqual({ date: "2026-09-22", weekday: 2 })  // Tuesday 16:59 ET
    expect(tradingDate("2026-09-22T21:00:00Z")).toEqual({ date: "2026-09-23", weekday: 3 })  // Tuesday 17:00 ET
    expect(tradingDate("2026-09-25T22:00:00Z")).toEqual({ date: "2026-09-28", weekday: 1 })  // Friday 18:00 ET
    expect(tradingDate("2026-12-01T01:00:00Z")).toEqual({ date: "2026-12-01", weekday: 2 })  // Monday 20:00 EST
    expect(tradingDate("not a time")).toBeNull()
    // Two closes on Sunday evening and one after midnight, all in Monday's overnight session.
    const closed = trades.find((t) => t.status === "closed")!
    const overnight = [{ ...closed, id: "a", closed: "2026-09-21T01:00:00Z", net: "-241.30" },
      { ...closed, id: "b", closed: "2026-09-21T01:30:00Z", net: "-71.30" },
      { ...closed, id: "c", closed: "2026-09-21T04:30:00Z", net: "-591.30" }]
    const weekday = tradeBuckets(overnight, "weekday")
    expect(weekday.map((b) => b.trades)).toEqual([3, 0, 0, 0, 0])
    expect(weekday[0]!.net).toBeCloseTo(-903.9, 9)
    // Wednesday 30 September at 20:30 ET trades for Thursday 1 October.
    const month = tradeBuckets([{ ...closed, closed: "2026-10-01T00:30:00Z" }], "month")
    expect(month.find((b) => b.label === "Oct")?.trades).toBe(1)
    expect(month.find((b) => b.label === "Sep")?.trades).toBe(0)
  })
  it("counts a close before a market holiday toward the server's next session", () => {
    // Wednesday 25 November 2026 at 18:00 ET: Thanksgiving is closed, so the engine's trading date is Friday the 27th.
    const closed = { ...trades.find((t) => t.status === "closed")!, closed: "2026-11-25T23:00:00Z" }
    expect(closingDay(closed)).toEqual({ date: "2026-11-26", weekday: 4 })
    const served = { ...closed, trading_day: "2026-11-27" }
    expect(closingDay(served)).toEqual({ date: "2026-11-27", weekday: 5 })
    expect(tradeBuckets([served], "weekday").map((b) => b.trades)).toEqual([0, 0, 0, 0, 1])
    expect(tradeBuckets([{ ...closed, trading_day: null }], "weekday").map((b) => b.trades)).toEqual([0, 0, 0, 1, 0])
  })
  it("gives each bucket the Journal's win rate: wins over decided trades", () => {
    const closed = trades.find((t) => t.status === "closed")!
    const three = [{ ...closed, id: "w", net: "10.00" }, { ...closed, id: "l", net: "-5.00" }, { ...closed, id: "b", net: "0.00" }]
    const month = tradeBuckets(three, "month").find((b) => b.trades > 0)!
    expect(month).toMatchObject({ trades: 3, wins: 1, losses: 1, winRate: 0.5 })
    expect(journalStats(three).winRate).toBe(month.winRate)
    expect(tradeBuckets([{ ...closed, net: "0.00" }], "month").find((b) => b.trades > 0)?.winRate).toBeNull()
  })
  it("reads tags and buckets closed trades under each of them", () => {
    expect(parseTags(" Breakout, 0DTE ,, breakout ")).toEqual(["breakout", "0dte"])
    const tagged = trades.map((t, i) => i === 0 ? { ...t, tags: ["breakout", "0dte"] } : i === 1 ? { ...t, tags: ["breakout"] } : t)
    expect(tradeTags(tagged)).toEqual(["0dte", "breakout"])
    const buckets = tradeBuckets(tagged, "tag")
    expect(buckets.map((b) => b.label)).toEqual(["0dte", "breakout", "untagged"])
    const closed = tagged.filter((t) => t.status === "closed")
    const expected = (tag: string) => closed.filter((t) => (t.tags ?? ["untagged"]).includes(tag)).length
    for (const bucket of buckets) expect(bucket.trades).toBe(expected(bucket.label))
    expect(tradeBuckets(trades, "tag").map((b) => b.label)).toEqual(["untagged"])
  })
  it("formats durations and contract labels", () => {
    expect(formatDuration(45)).toBe("45s")
    expect(formatDuration(290)).toBe("4m 50s")
    expect(formatDuration(7500)).toBe("2h 5m")
    expect(formatDuration(273_600)).toBe("3d 4h")
    expect(formatDuration(null)).toBe("—")
    expect(contractLabel(trades[2]!)).toBe("SPX Oct 16 7000C")
    expect(parseOsi("SPXW  261022P05000500")).toEqual({ root: "SPXW", expiry: "2026-10-22", type: "put", strike: 5000.5 })
    expect(parseOsi("bad")).toBeNull()
    expect(osiLabel("SPXW  261022P05000000", "SPX")).toBe("SPX Oct 22 5000P")
    expect(osiLabel("bad", "SPX")).toBe("bad")
  })
})

describe("ticket logic", () => {
  it("names strategies from the held position", () => {
    expect(strategyName("buy", "call", 0, 1)).toBe("Long Call")
    expect(strategyName("sell", "put", 0, 2)).toBe("Short Put")
    expect(strategyName("sell", "call", 3, 3)).toBe("Close Long Call")
    expect(strategyName("buy", "put", -2, 1)).toBe("Close Short Put")
    expect(strategyName("sell", "call", 2, 5)).toBe("Reverse to Short Call")
    expect(split("sell", 5, 2)).toEqual({ closing: 2, opening: 3 })
    expect(split("buy", 4, 2)).toEqual({ closing: 0, opening: 4 })
  })
  it("mirrors the matcher's far-side marketability", () => {
    expect(marketability("buy", "limit", "4.60", quote)).toMatchObject({ marketable: true, price: 4.6, size: 3 })
    expect(marketability("buy", "limit", "4.55", quote).message).toBe("Rests below the ask ($4.60). Fills when the ask reaches $4.55.")
    expect(marketability("sell", "limit", "4.50", quote).message).toBe("Marketable: fills now at the bid $4.50, up to 10 displayed.")
    expect(marketability("sell", "market", "", quote).message).toBe("Fills now at the bid $4.50, up to 10 displayed; any remainder cancels.")
    expect(marketability("buy", "limit", "", quote).message).toBe("Enter a limit price.")
    expect(marketability("buy", "market", "", { ...quote, ask: null }).marketable).toBe(false)
  })

})


describe("conditional orders", () => {
  it("places stops against the position and conditional entries by the level's side of spot", () => {
    expect(stopDirection("option", "buy", "call")).toBe("at_or_below")
    expect(stopDirection("option", "sell", "put")).toBe("at_or_above")
    expect(stopDirection("underlying", "buy", "call")).toBe("at_or_below")
    expect(stopDirection("underlying", "buy", "put")).toBe("at_or_above")
    expect(stopDirection("underlying", "sell", "call")).toBe("at_or_above")
    expect(stopDirection("underlying", "sell", "put")).toBe("at_or_below")
    expect(opposite("at_or_below")).toBe("at_or_above")
    expect(crossDirection(5010, 5000)).toBe("at_or_above")
    expect(crossDirection(4990, 5000)).toBe("at_or_below")
    expect(crossDirection(4990, null)).toBe("at_or_above")
    expect(describeTrigger({ source: "option", direction: "at_or_below", level: "3.5" }, "sell", "SPX")).toBe("bid ≤ $3.50")
    expect(describeTrigger({ source: "underlying", direction: "at_or_above", level: "5010" }, "buy", "SPX")).toBe("SPX ≥ 5,010.00")
    expect(describeTrigger({ source: "underlying", symbol: "VIX", direction: "at_or_above", level: "20" }, "buy", "SPX")).toBe("VIX ≥ 20.00")
    expect(describeTrigger({ source: "study", study: "term_ratio", direction: "at_or_above", level: "1" }, "buy", "SPX")).toBe("SPX 9d/30d IV ratio ≥ 1.00")
    expect(describeTrigger({ source: "time", direction: "at_or_above", level: "0.00", at: "15:30" }, "sell", "SPX")).toBe("it is 15:30 New York time or later")
    expect(describeTrigger({ source: "option", direction: "at_or_below", level: "3.60", reference: "mid", trail: { unit: "amount", value: "0.50" } }, "sell", "SPX"))
      .toBe("mid ≤ $3.60, trailing $0.50")
    expect(describeTrigger({ source: "option", direction: "at_or_above", level: "4.50", reference: "bid_ask", trail: { unit: "ticks", value: 3 } }, "buy", "SPX"))
      .toBe("ask ≥ $4.50, trailing 3 ticks")
    expect(describeTrigger({ source: "combo", direction: "at_or_above", level: "0.90", reference: "mark", trail: { unit: "percent", value: "10" } }, "buy", "SPX"))
      .toBe("closing mark net ≥ $0.90, trailing 10%")
    const then = { symbol: "SPXW  261022C05000000", side: "sell" as const, legs: null, type: "limit" as const, time_in_force: "gtc" as const, quantity: 2,
      limit_price: "4.30", good_till: null, walk: null, group: null, trigger: null, bracket: null, then: null, oco: null }
    expect(describeChain({ role: null, oco: "2", then, chained_order: null, chained_from: null, underlying: "SPX" }))
      .toBe("one cancels other with #2 · when filled, places sell 2 @ $4.30 GTC")
    expect(describeChain({ role: null, oco: null, then, chained_order: "3", chained_from: "1", underlying: "SPX" })).toBe("placed by #1 · placed #3 when filled")
    expect(describeChain({ role: "stop_loss", oco: "4", then: null, chained_order: null, chained_from: null, underlying: "SPX" })).toBe("")
  })
  it("describes pending GTD deadlines, including the order that cancels it", () => {
    const pending: PendingOrder = {
      symbol: "SPXW  261022C05000000", side: "sell", legs: null, type: "limit", quantity: 1,
      time_in_force: "gtd", good_till: "2026-09-23T19:30:00Z", limit_price: "5.00",
      walk: null, group: null, trigger: null, bracket: null, then: null, oco: null,
    }
    expect(describePending(pending, "SPX")).toBe("sell 1 @ $5.00 GTD until Wed, Sep 23, 2026, 15:30 ET")
    const gtc: PendingOrder = { ...pending, time_in_force: "gtc", good_till: null }
    expect(describePending(gtc, "SPX")).toBe("sell 1 @ $5.00 GTC")
    expect(describePending({ ...gtc, oco: pending }, "SPX"))
      .toBe("sell 1 @ $5.00 GTC, or sell 1 @ $5.00 GTD until Wed, Sep 23, 2026, 15:30 ET")
  })
  it("rounds suggested prices to the root's tier tick", () => {
    expect(roundToTick("SPXW", 6.31)).toBe("6.30")
    expect(roundToTick("SPXW", 2.93)).toBe("2.95")
    expect(roundToTick("SPY", 6.317)).toBe("6.32")
    expect(roundToTick("AAPL", 3.12)).toBe("3.10")
    expect(roundToTick("SPXW", 0.01)).toBe("0.05")
    expect(roundToTick("SPXW", -1)).toBeNull()
  })
  it("rounds a price between ticks to the side a buy or a sell asks for", () => {
    // The mids of odd-tick spreads: 47.60 × 48.70 and 16.00 × 16.50 on SPXW.
    expect(roundToTick("SPXW", 48.15, "down")).toBe("48.10")
    expect(roundToTick("SPXW", 48.15, "up")).toBe("48.20")
    expect(roundToTick("SPXW", 16.25, "down")).toBe("16.20")
    expect(roundToTick("SPXW", 16.25, "up")).toBe("16.30")
    // Across the $3 tier and on a tick already, float noise included.
    expect(roundToTick("SPXW", 2.975, "down")).toBe("2.95")
    expect(roundToTick("SPXW", 2.975, "up")).toBe("3.00")
    expect(roundToTick("SPXW", 48.1, "up")).toBe("48.10")
    expect(roundToTick("SPXW", 48.1, "down")).toBe("48.10")
    expect(roundToTick("XSP", 3.125, "up")).toBe("3.15")
    expect(roundToTick("SPY", 1.005, "up")).toBe("1.01")
    expect(roundToTick("SPXW", 0.02, "down")).toBe("0.05")
  })
})

describe("equity ticks", () => {
  it("steps SPY in pennies, single stocks on penny-pilot tiers and OEX like SPX", () => {
    expect(stepLimitPrice("SPY", "12.30", 1)).toBe("12.31")
    expect(stepLimitPrice("AAPL", "2.99", 1)).toBe("3.00")
    expect(stepLimitPrice("AAPL", "3.00", 1)).toBe("3.05")
    expect(stepLimitPrice("AAPL", "3.00", -1)).toBe("2.99")
    expect(stepLimitPrice("OEX", "3.00", 1)).toBe("3.10")
    expect(stepLimitPrice("VIX", "3.00", 1)).toBe("3.01")
  })
})

describe("money display", () => {
  it("signs P&L with a typographic minus and keeps exact differences", () => {
    expect(signedMoney("267.50")).toBe("+$267.50")
    expect(signedMoney("-10.65")).toBe("−$10.65")
    expect(signedMoney("0.00")).toBe("$0.00")
    expect(signedMoney("0.001")).toBe("$0.00")
    expect(subtractMoney("100267.5", "100000.00")).toBe("267.50")
    expect(subtractMoney("1", "1.000001")).toBe("-0.000001")
    expect(ratio("267.50", "100000")).toBeCloseTo(0.002675)
    expect(ratio("1", "0")).toBeNull()
    expect(signedPercent(0.0112)).toBe("+1.1%")
    expect(signedPercent(-0.011)).toBe("−1.1%")
    expect(signedPercent(0.00001)).toBe("0.0%")
    expect(signedPercent(null)).toBe("—")
  })
})
