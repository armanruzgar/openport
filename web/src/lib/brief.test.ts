import { describe, expect, it } from "vitest"
import type { Candle } from "../api/types"
import { briefAccount, briefRisk, briefSummary, briefTime, briefVolatility, sessionMove } from "../test/brief-fixtures"
import { order, portfolio } from "../test/trading-fixtures"
import { briefDay, briefLevels, invertedTerm, lossAllowance, notePhase, overnightMove, overnightRange, orderDeadlines, positionDeadlines, priorSeries, priorSession, todayMove, volChange, weekMove } from "./brief"

const now = Date.parse(briefTime)
const bar = (time: string, h = 7010, l = 6990, c = 7000): Candle => ({ t: Date.parse(time) / 1000, o: 7000, h, l, c })
const prior = bar("2026-09-22T13:30:00Z")

describe("brief market derivations", () => {
  it("measures points and percent from the prior close, including negative and missing moves", () => {
    expect(overnightMove(7070, 7000)?.points).toBe(70)
    expect(overnightMove(7070, 7000)?.percent).toBeCloseTo(1)
    expect(overnightMove(6930, 7000)?.percent).toBeCloseTo(-1)
    expect(overnightMove(7000, 7000)).toEqual({ points: 0, percent: 0 })
    for (const bad of [null, undefined, NaN, 0]) expect(overnightMove(7000, bad)).toBeNull()
  })
  it("uses the engine trading date at night and never a browser date", () => {
    expect(briefDay(Date.parse("2026-09-23T00:30:00Z"), "2026-09-23")).toBe("2026-09-23")
    expect(briefDay(Date.parse("2026-09-23T00:30:00Z"), null, briefVolatility)).toBe("2026-09-23")
    expect(briefDay(NaN, "2026-09-23")).toBeNull()
    expect(briefDay(Date.parse("2026-09-27T12:00:00Z"), null, briefVolatility)).toBe("2026-09-28")
    expect(briefDay(Date.parse("2026-09-27T12:00:00Z"))).toBeNull()
  })
  it("excludes the current daily bar and refuses an older bar when the prior session is known", () => {
    const bars = [bar("2026-09-21T13:30:00Z"), prior, bar("2026-09-23T13:30:00Z")]
    expect(priorSession(bars, "2026-09-23", now)).toEqual(prior)
    expect(priorSession(bars.slice(0, 1), "2026-09-23", now, "2026-09-22")).toBeUndefined()
    expect(priorSession(bars, null, now)).toBeUndefined()
  })
  it("handles a holiday gap using the supplied prior date", () => {
    const friday = bar("2026-09-04T13:30:00Z")
    expect(priorSession([friday], "2026-09-08", Date.parse("2026-09-08T12:00Z"), "2026-09-04")).toEqual(friday)
  })
  it("keeps only the observed overnight window, clipped to market time", () => {
    const range = overnightRange([prior, bar("2026-09-22T20:10Z", 8000), bar("2026-09-23T00:15Z", 7020),
      bar("2026-09-23T11:59Z", 7050, 6980), bar("2026-09-23T12:01Z", 9000), bar("2026-09-23T13:30Z", 9000)], "2026-09-23", now)
    expect(range).toEqual({ high: 7050, low: 6980, bars: 2 })
    expect(overnightRange([prior], "2026-09-23", now)).toBeNull()
  })
  it("handles winter time and an overnight session across a weekend", () => {
    const result = overnightRange([bar("2026-12-07T01:15Z", 7020), bar("2026-12-07T14:30Z", 9000)], "2026-12-07", Date.parse("2026-12-07T15:00Z"))
    expect(result?.high).toBe(7020)
  })
  it("uses same-day expiry variance, falling back only when no same-day expiry exists", () => {
    expect(todayMove(briefVolatility, "2026-09-23")).toMatchObject({ points: 70, shared: true })
    const direct = { ...briefVolatility, implied_moves: { ...briefVolatility.implied_moves, today_points: 90, today_percent: 1.2, today_reason: null } }
    expect(todayMove(direct, "2026-09-23")).toMatchObject({ points: 90, shared: false })
    expect(todayMove({ ...direct, implied_moves: { ...direct.implied_moves, today_points: null, today_reason: "unusable_variance" } }, "2026-09-23").points).toBeNull()
    expect(todayMove({ ...direct, implied_moves: { ...direct.implied_moves, today_calendar_arbitrage: true } }, "2026-09-23").points).toBeNull()
  })
  it("adds the remaining week's variances at spot, excluding next week", () => {
    const move = weekMove(briefVolatility, "2026-09-23", 7100)
    expect(move.percent).toBeCloseTo(Math.sqrt(3))
    expect(move.points).toBeCloseTo(71 * Math.sqrt(3))
    expect(weekMove(briefVolatility, "2026-09-25", 7000).points).toBe(70)
  })
  it("does not invent holiday sessions, skip missing sessions or hide arbitrage", () => {
    const moves = (sessions: typeof briefVolatility.implied_moves.sessions) => ({ ...briefVolatility, implied_moves: { ...briefVolatility.implied_moves, sessions } })
    expect(weekMove(moves([sessionMove("2026-04-02"), sessionMove("2026-04-06")]), "2026-04-02", 7000).points).toBe(70)
    expect(weekMove(moves([sessionMove("2026-09-23"), sessionMove("2026-09-24", null)]), "2026-09-23", 7000).points).toBeNull()
    expect(weekMove(moves([{ ...sessionMove("2026-09-23"), calendar_arbitrage: true }]), "2026-09-23", 7000)).toMatchObject({ points: null, calendar_arbitrage: true })
    expect(weekMove(briefVolatility, "2026-09-23", null)).toMatchObject({ points: null, percent: Math.sqrt(3) })
  })
  it("matches the exact prior-close series row without bridging missing days or taking today's close", () => {
    const rows = [{ t: Date.parse("2026-09-21T20:00Z") / 1000, mfiv30: 17, sources: {} },
      { t: Date.parse("2026-09-22T20:00Z") / 1000, mfiv30: null, sources: {} },
      { t: Date.parse("2026-09-23T20:00Z") / 1000, mfiv30: 19, sources: {} }]
    expect(priorSeries(rows, "2026-09-22", now)?.mfiv30).toBeNull()
    expect(priorSeries(rows, "2026-09-23", now)).toBeUndefined()
    expect(volChange(20, 18)).toBe(2)
    expect(volChange(20, null)).toBeNull()
  })
  it("flags backwardation only above one and keeps missing ratios unknown", () => {
    expect(invertedTerm(1.01)).toBe(true)
    expect(invertedTerm(1)).toBe(false)
    expect(invertedTerm(.9)).toBe(false)
    expect(invertedTerm(null)).toBeNull()
    expect(invertedTerm(NaN)).toBeNull()
  })
  it("combines coincident levels, preserves their names and omits missing levels", () => {
    const levels = briefLevels(prior, { high: 7100, low: 6990, bars: 3 }, briefSummary.exposure, 7070, todayMove(briefVolatility, "2026-09-23"))
    expect(levels[0]).toEqual({ price: 7140, label: "Today +1σ", kind: "brief" })
    expect(levels.find(level => level.price === 7100)?.label).toBe("Overnight high / Call wall")
    expect(levels.find(level => level.price === 6990)?.label).toBe("Prior low / Overnight low")
    expect(briefLevels(undefined, null, undefined, null, todayMove(undefined, null))).toEqual([])
  })
})

describe("brief account derivations", () => {
  it("uses the smallest exact allowance with a different binding constraint each time", () => {
    expect(lossAllowance(briefAccount, briefRisk)).toMatchObject({ value: "4267.50", reason: "Soft floor room" })
    expect(lossAllowance(briefAccount, { ...briefRisk, daily_loss: "4900.000001" })).toMatchObject({ value: "99.999999", reason: "Risk-limit daily loss left" })
    expect(lossAllowance({ ...briefAccount, evaluation: { ...briefAccount.evaluation, floor: "100000" } }, briefRisk)).toMatchObject({ value: "267.50", reason: "Floor room" })
  })
  it("counts the plan's daily loss limit, the loss that fails or locks the attempt", () => {
    // static-100k: a $4,000 daily loss fails the attempt while the floor is $8,000 away.
    const daily = { limit: "4000.00", basis: "equity" as const, action: "fail" as const, reference: "100267.50", level: "96267.50", room: "4000.00" }
    const planned = { ...briefAccount, evaluation: { ...briefAccount.evaluation, floor: "92267.50", daily_loss: daily } }
    const allowance = lossAllowance(planned, { ...briefRisk, guardrail_state: { ...briefRisk.guardrail_state, soft_floor: null } })
    expect(allowance).toMatchObject({ value: "4000.00", reason: "Plan daily loss room" })
    expect(allowance.parts).toEqual([{ label: "Risk-limit daily loss left", value: "4988.70" },
      { label: "Plan daily loss room", value: "4000.00" }, { label: "Floor room", value: "8000.00" }])
    const exceeded = { ...planned, evaluation: { ...planned.evaluation, daily_loss: { ...daily, room: "-25.00" } } }
    expect(lossAllowance(exceeded, briefRisk).value).toBe("0")
  })
  it("clamps an exhausted allowance and excludes disabled limits", () => {
    expect(lossAllowance(briefAccount, { ...briefRisk, daily_loss: "5001" }).value).toBe("0")
    expect(lossAllowance(briefAccount, { ...briefRisk, limits: { ...briefRisk.limits, max_daily_loss: "0" } }).value).toBe("4267.50")
    const noFloor = { ...briefAccount, evaluation: { ...briefAccount.evaluation, floor: null }, guardrail_state: { ...briefAccount.guardrail_state, soft_floor: null } }
    expect(lossAllowance(noFloor, { ...briefRisk, guardrail_state: noFloor.guardrail_state, limits: { ...briefRisk.limits, max_daily_loss: "0" } }).reason).toBe("no loss limit or floor configured")
  })
  it("withholds allowances with missing, mixed-version or incomplete account inputs", () => {
    expect(lossAllowance(undefined, briefRisk).value).toBeNull()
    expect(lossAllowance(briefAccount, { ...briefRisk, complete: false }).value).toBeNull()
    expect(lossAllowance(briefAccount, { ...briefRisk, account_version: "18" }).value).toBeNull()
    expect(lossAllowance({ ...briefAccount, guardrail_state: undefined }, { ...briefRisk, guardrail_state: undefined }).value).toBeNull()
  })
  it("uses the plan before the open and review at the normal or scheduled early close", () => {
    expect(notePhase("2026-09-23", now, briefVolatility)).toBe("plan")
    expect(notePhase("2026-09-23", Date.parse("2026-09-23T20:00Z"))).toBe("review")
    const early = { ...briefVolatility, as_of: "2026-11-27T18:00:00Z", implied_moves: { ...briefVolatility.implied_moves, sessions: [sessionMove("2026-11-30")] } }
    expect(notePhase("2026-11-27", Date.parse(early.as_of), early)).toBe("review")
    expect(notePhase("2026-11-30", Date.parse("2026-11-30T01:15Z"), early)).toBe("plan")
  })
  it("uses actual AM last trade and account cutoff instead of assuming expiry-day close", () => {
    const position = { ...portfolio.positions[0]!, expiry_time: "2026-09-24T13:30:00Z", last_trade_time: "2026-09-23T20:15:00Z" }
    expect(positionDeadlines(position, 600, "2026-09-23").map(event => [event.label, new Date(event.time).toISOString()])).toEqual([
      ["auto-close", "2026-09-23T20:05:00.000Z"], ["last trade", "2026-09-23T20:15:00.000Z"],
    ])
    expect(positionDeadlines(position, 0, "2026-09-24").map(event => event.label)).toEqual(["expires"])
    expect(positionDeadlines(portfolio.positions[0]!, 300, "2026-09-23")).toEqual([])
  })
  it("shows order contract deadlines separately from session end without guessing AM/PM", () => {
    const row = { ...briefSummary.expiries[0]!, expiry: "2026-10-16", expiry_time: "2026-10-16T20:00Z", last_trade: "2026-10-16T20:00Z" }
    const summary = { ...briefSummary, expiries: [row] }
    const result = orderDeadlines(order, [], summary, 300, "2026-10-16")
    expect(result.complete).toBe(true)
    expect(result.events.map(event => [event.label, new Date(event.time).toISOString()])).toContainEqual(["auto-close cutoff", "2026-10-16T19:55:00.000Z"])
    expect(orderDeadlines(order, [], { ...summary, expiries: [row, { ...row, settlement: "AM" }] }, 300, "2026-10-16")).toEqual({ complete: false, events: [] })
  })

})
