import { describe, expect, it } from "vitest"
import type { AccountRules } from "../api/trading-types"
import { customPlan, planForm } from "../components/PlanEditor"
import { account, fundedAccount } from "../test/trading-fixtures"
import { phaseFact, clockText, dailyLossFact, dayEndFact, decisionLabel, drawdownFact, floorMoves, objectiveFacts, objectiveValue, targetFact, planEntryNotice, timeRuleFacts, timeRuleNotices } from "./plan-rules"

const rules: AccountRules = { ...account.rules, buy_only: false }

describe("plan rule texts", () => {
  it("labels practice using the server's evaluation conditions and preserves program steps", () => {
    const practice: AccountRules = { ...rules, plan_id: "practice", plan: "Practice",
      profit_target: null, max_drawdown: null, daily_loss_limit: null,
      time_limit_days: 0, inactivity_days: 0, no_overnight: false, hold_restrictions: [] }
    expect(phaseFact(practice)).toBe("Practice")
    expect(phaseFact({ ...practice, profit_target: "0.00", max_drawdown: "0", daily_loss_limit: "0" })).toBe("Practice")
    const triggers: Partial<AccountRules>[] = [
      { profit_target: "0.01" }, { max_drawdown: "0.01" }, { daily_loss_limit: "0.01" },
      { time_limit_days: 1 }, { inactivity_days: 1 }, { no_overnight: true }, { hold_restrictions: ["weekend"] },
    ]
    for (const trigger of triggers) expect(phaseFact({ ...practice, ...trigger })).toBe("Evaluation")
    expect(phaseFact({ ...practice, phase: "verification" })).toBe("Step 2 of 2: verification")
    expect(phaseFact({ ...practice, phase: "funded" })).toBe("Funded account (simulated)")
    expect(phaseFact({ ...practice, plan_id: "two-step-challenge" })).toBe("Step 1 of 2: challenge")
  })
  it("preserves cents in rule amounts and keeps whole-dollar presets compact", () => {
    expect(dailyLossFact({ ...rules, daily_loss_limit: "13.25" })).toContain("$13.25 daily loss limit")
    expect(drawdownFact({ ...rules, max_drawdown: "2812.50" }, "50000")).toContain("$2,812.50 trailing drawdown")
    expect(targetFact({ ...rules, profit_target: "3000.50" })).toBe("$3,000.50 profit target")
    expect(targetFact({ ...rules, profit_target: "3000.00" })).toBe("$3,000 profit target")
    expect(drawdownFact({ ...rules, max_drawdown: "13.25", drawdown_mode: "static" }, "1000"))
      .toBe("$13.25 static drawdown: the floor stays at $986.75")
    expect(drawdownFact({ ...rules, lock_balance: "100000.50" }, "100000")).toContain("locks at $100,000.50")
    expect(objectiveFacts({ ...rules, min_profitable_days: 1, profitable_day_profit: "13.25" }))
      .toContain("At least 1 profitable day of $13.25+")
    expect(objectiveValue({ code: "PROFIT_TARGET", met: false, actual: 0, required: 3000.50, message: "" }))
      .toBe("$0.00 of $3,000.50")
  })
  it("names static and locking floors, and older servers' floors as before", () => {
    expect(drawdownFact(rules, "100000.00")).toBe("$5,000 trailing drawdown (intraday)")
    expect(drawdownFact({ ...rules, drawdown_mode: "static" }, "100000.00")).toBe("$5,000 static drawdown: the floor stays at $95,000")
    expect(drawdownFact({ ...rules, drawdown_mode: "end_of_day", lock_at_start: true }, "100000.00"))
      .toBe("$5,000 trailing drawdown (end of day), locks at the starting balance")
    expect(drawdownFact({ ...rules, lock_balance: "100000.00" }, "100000.00")).toBe("$5,000 trailing drawdown (intraday), locks at $100,000")
    expect(floorMoves({ ...rules, drawdown_mode: "static" })).toBe("Never (static)")
    expect(floorMoves({ ...rules, drawdown_mode: "end_of_day", lock_at_start: true })).toBe("At each close, to the start")
    expect(floorMoves({ ...rules, max_drawdown: null })).toBe("—")
  })
  it("states the daily loss limit, objectives, target basis and day end only when set", () => {
    expect(dailyLossFact(rules)).toBeNull()
    expect(objectiveFacts(rules)).toEqual([])
    expect(dayEndFact(rules)).toBeNull()
    expect(targetFact(rules)).toBe("$10,000 profit target")
    const plan: AccountRules = { ...rules, profit_basis: "balance", daily_loss_limit: "2000.00", daily_loss_basis: "higher",
      daily_loss_action: "fail", min_trading_days: 4, min_profitable_days: 1, profitable_day_profit: "500.00",
      consistency_percent: 40, consistency_basis: "positive_days", day_end: "18:00" }
    expect(targetFact(plan)).toBe("$10,000 profit target on the closed balance")
    expect(dailyLossFact(plan)).toBe("$2,000 daily loss limit from the higher of the day's opening balance and equity: fails the attempt")
    expect(dailyLossFact({ ...plan, daily_loss_action: "lock", daily_loss_basis: "peak" }))
      .toBe("$2,000 daily loss limit from the day's equity high: closes every position and locks the day")
    expect(objectiveFacts(plan)).toEqual(["At least 4 trading days", "At least 1 profitable day of $500+", "Best day at most 40% of the profitable days' total"])
    expect(dayEndFact(plan)).toBe("Trading day ends at 6:00 pm ET")
    expect(clockText("24:00")).toBe("midnight")
    expect(clockText("16:15")).toBe("4:15 pm")
  })
  it("shows objectives' standing and decision codes in a trader's words", () => {
    expect(objectiveValue({ code: "PROFIT_TARGET", met: false, actual: 267.5, required: 10000, message: "" })).toBe("$267.50 of $10,000")
    expect(objectiveValue({ code: "MIN_TRADING_DAYS", met: false, actual: 2, required: 4, message: "" })).toBe("2 of 4")
    expect(objectiveValue({ code: "CONSISTENCY", met: false, actual: 66.66, required: 50, message: "" })).toBe("67% of 50% max")
    expect(objectiveValue({ code: "CONSISTENCY", met: true, actual: null, required: 50, message: "" })).toBe("— of 50% max")
    expect(decisionLabel("DAILY_LOSS_LIMIT")).toBe("daily loss limit")
    expect(decisionLabel("SOME_NEW_CODE")).toBe("some new code")
    expect(decisionLabel(null)).toBeNull()
  })
})

describe("custom plans", () => {
  it("start from a preset's rules and send every edited rule", () => {
    const form = { ...planForm({ initial_cash: "100000.00", rules }), name: "My plan", drawdown_mode: "static" as const, lock: "start" as const,
      daily_loss_limit: "1500", daily_loss_basis: "peak" as const, daily_loss_action: "fail" as const, consistency_percent: "40",
      min_trading_days: "3", min_profitable_days: "2", profitable_day_profit: "250", profit_basis: "balance" as const, day_end: "24:00" }
    const result = customPlan(form, rules)
    expect("error" in result).toBe(false)
    if ("error" in result) return
    expect(result.initial_cash).toBe("100000.00")
    expect(result.rules).toMatchObject({ plan: "My plan", phase: "evaluation", payouts: null, profit_target: "10000.00", max_drawdown: "5000.00",
      drawdown_mode: "static", lock_at_start: false, lock_balance: null, profit_basis: "balance", daily_loss_limit: "1500",
      daily_loss_basis: "peak", daily_loss_action: "fail", consistency_percent: 40, consistency_basis: "total", min_trading_days: 3,
      min_profitable_days: 2, profitable_day_profit: "250", day_end: "24:00", buy_only: false, defined_risk: false,
      buying_power: true, expiry_cutoff_seconds: 300 })
    // A trailing floor keeps its lock; blank amounts turn rules off.
    const trailing = customPlan({ ...form, drawdown_mode: "end_of_day", daily_loss_limit: "", consistency_percent: "" }, rules)
    expect("error" in trailing ? null : trailing.rules).toMatchObject({ lock_at_start: true, daily_loss_limit: null, consistency_percent: 0 })
  })
  it("edits funded payout rules and validates consistency and buffer settings", () => {
    const base = fundedAccount.rules
    const form = { ...planForm({ initial_cash: "100000.00", rules: base }),
      payout_consistency_percents: "20, 25, 30", payout_buffer: "2100.000001", buffer_payouts: "3" }
    const result = customPlan(form, base)
    expect("error" in result ? result : result.rules).toMatchObject({ phase: "funded", profit_target: null,
      payouts: { ...base.payouts, consistency_percents: [20, 25, 30], buffer: "2100.000001", buffer_payouts: 3 } })
    for (const value of ["0", "101", "40.5", "20,", ",20", "abc", Array(65).fill("40").join(",")])
      expect(customPlan({ ...form, payout_consistency_percents: value }, base)).toHaveProperty("error")
    for (const value of ["-1", "101", "1.5", "abc"])
      expect(customPlan({ ...form, buffer_payouts: value }, base)).toHaveProperty("error")
    expect(customPlan({ ...form, payout_buffer: "-0.01" }, base)).toHaveProperty("error")
    const off = customPlan({ ...form, payout_consistency_percents: "", payout_buffer: "0", buffer_payouts: "0" }, base)
    expect("error" in off ? off : off.rules.payouts).toMatchObject({ consistency_percents: [], buffer: "0", buffer_payouts: 0 })
    const restored = planForm({ initial_cash: "100000.00", rules: { ...base,
      payouts: { ...base.payouts!, consistency_percents: [40], buffer: "2100.00", buffer_payouts: 2 } } })
    expect(restored).toMatchObject({ phase: "funded", payout_consistency_percents: "40", payout_buffer: "2100.00", buffer_payouts: "2" })
  })
  it("clears hidden pass objectives when converting an evaluation template to funded", () => {
    const form = { ...planForm({ initial_cash: "100000.00", rules }), phase: "funded" as const,
      consistency_percent: "40", min_trading_days: "4", min_profitable_days: "2", profitable_day_profit: "250" }
    const result = customPlan(form, rules)
    expect("error" in result ? result : result.rules).toMatchObject({ phase: "funded", profit_target: null,
      consistency_percent: 0, min_trading_days: 0, min_profitable_days: 0, profitable_day_profit: null })
    expect(customPlan({ ...form, consistency_percent: "x", profitable_day_profit: "x" }, rules)).not.toHaveProperty("error")
  })
  it("names the first problem before the server sees it", () => {
    const form = planForm({ initial_cash: "100000.00", rules })
    expect(customPlan({ ...form, name: " " }, rules)).toEqual({ error: "Name the plan" })
    expect(customPlan({ ...form, daily_loss_limit: "1,000" }, rules)).toEqual({ error: "Daily loss limit must be a dollar amount" })
    expect(customPlan({ ...form, consistency_percent: "120" }, rules)).toEqual({ error: "Consistency must be a whole number from 0 to 100" })
    expect(customPlan({ ...form, min_trading_days: "2.5" }, rules)).toEqual({ error: "Minimum trading days must be a whole number from 0 to 366" })
    expect(customPlan({ ...form, day_end: "16:00" }, rules)).toEqual({ error: "The trading day ends between 16:15 and 24:00" })
    expect(customPlan({ ...form, day_end: "24:30" }, rules)).toEqual({ error: "The trading day ends between 16:15 and 24:00" })
    expect(customPlan({ ...form, lock: "balance", lock_balance: "" }, rules)).toEqual({ error: "Enter the balance the floor locks at" })
    expect(customPlan({ ...form, initial_cash: "0" }, rules)).toEqual({ error: "The starting balance must be above zero" })
  })
})

it("edits trade-entry rules and validates their ranges", () => {
  const base = { ...rules, max_contracts_held: 5, require_stop_loss: true, max_trade_risk: "200.50", max_trade_risk_percent: 10 }
  const form = planForm({ initial_cash: "10000", rules: base })
  expect(form).toMatchObject({ max_contracts_held: "5", require_stop_loss: "yes", max_trade_risk: "200.50", max_trade_risk_percent: "10" })
  const result = customPlan(form, base)
  expect("error" in result ? result : result.rules).toMatchObject({ max_contracts_held: 5, require_stop_loss: true, max_trade_risk: "200.50", max_trade_risk_percent: 10 })
  for (const max_contracts_held of ["-1", "100001", "1.5"])
    expect(customPlan({ ...form, max_contracts_held }, base)).toHaveProperty("error")
  for (const max_trade_risk_percent of ["-1", "101", "0.5"])
    expect(customPlan({ ...form, max_trade_risk_percent }, base)).toHaveProperty("error")
  expect(customPlan({ ...form, max_trade_risk: "-1" }, base)).toHaveProperty("error")
  const off = customPlan({ ...form, max_contracts_held: "", require_stop_loss: "no", max_trade_risk: "", max_trade_risk_percent: "" }, base)
  expect("error" in off ? off : off.rules).toMatchObject({ max_contracts_held: 0, require_stop_loss: false, max_trade_risk: null, max_trade_risk_percent: 0 })
  })

it("round trips scaling and rejects unordered thresholds or decreasing limits", () => {
  const scaling = [{ profit: "0.00", contracts: 2 }, { profit: "1500.000001", contracts: 3 }]
  const base = { ...rules, scaling }
  const form = planForm({ initial_cash: "50000", rules: base })
  expect(form.scaling).toEqual([{ profit: "0.00", contracts: "2" }, { profit: "1500.000001", contracts: "3" }])
  const result = customPlan(form, base)
  expect("error" in result ? result : result.rules.scaling).toEqual(scaling)
  for (const steps of [
    [{ profit: "1", contracts: "2" }], [{ profit: "0", contracts: "0" }], [{ profit: "0", contracts: "10001" }],
    [{ profit: "0", contracts: "2.5" }], [{ profit: "0", contracts: "2" }, { profit: "0", contracts: "3" }],
    [{ profit: "0", contracts: "3" }, { profit: "1", contracts: "2" }], Array(17).fill({ profit: "0", contracts: "2" }),
  ]) expect(customPlan({ ...form, scaling: steps }, base)).toHaveProperty("error")
  const off = customPlan({ ...form, scaling: [] }, base)
  expect("error" in off ? off : off.rules.scaling).toEqual([])
})

describe("time and instrument rules", () => {
  const restricted = { ...rules, time_limit_days: 60, inactivity_days: 14, underlyings: ["SPX"], trading_start: "09:30", trading_end: "16:00",
    max_contracts_held: 5, require_stop_loss: true, max_trade_risk: "123.456789", max_trade_risk_percent: 25 }
  it("validates and round-trips the custom form, including midnight", () => {
    const form = planForm({ initial_cash: "100000", rules: restricted })
    const result = customPlan(form, restricted)
    expect("error" in result ? result : result.rules).toMatchObject({ ...restricted, plan: "Custom plan" })
    for (const patch of [{ time_limit_days: "367" }, { inactivity_days: "1.5" }, { underlyings: "spx" }, { underlyings: "SPX, SPX" },
      { trading_start: "16:00" }, { trading_end: "" }, { trading_start: "24:01" }])
      expect(customPlan({ ...form, ...patch }, rules)).toHaveProperty("error")
    expect(customPlan({ ...form, trading_start: "00:00", trading_end: "24:00" }, rules)).not.toHaveProperty("error")
  })
  it("blocks entries by underlying and New York hours, including DST, but permits exits", () => {
    expect(planEntryNotice(restricted, "SPY", account.time, false)).toContain("INSTRUMENT_NOT_ALLOWED")
    expect(planEntryNotice(restricted, "SPY", account.time, true)).toBeNull()
    for (const time of ["2026-09-22T13:29:59Z", "2026-09-22T20:00:00Z", "2026-12-01T21:00:00Z"])
      expect(planEntryNotice(restricted, "SPX", time, false)).toContain("OUTSIDE_PLAN_HOURS")
    for (const time of ["2026-09-22T13:30:00Z", "2026-09-22T19:59:59Z", "2026-12-01T14:30:00Z"])
      expect(planEntryNotice(restricted, "SPX", time, false)).toBeNull()
    expect(planEntryNotice(rules, "SPY", account.time, false)).toBeNull()
  })
  it("shows deadlines only while active and warns within seven inactivity days", () => {
    const e = { ...account.evaluation, days_left: 3, deadline: "2026-09-25", inactive_days: 7, inactivity_deadline: "2026-09-29" }
    expect(timeRuleNotices(e, restricted)).toEqual([
      "Evaluation: 3 calendar days left; deadline 2026-09-25.",
      "Inactivity: 7 calendar days left to execute a trade; deadline 2026-09-29.",
    ])
    expect(timeRuleNotices({ ...e, inactive_days: 6 }, restricted)).toHaveLength(1)
    expect(timeRuleNotices({ ...e, status: "passed" }, restricted)).toEqual([])
    expect(timeRuleNotices(account.evaluation, rules)).toEqual([])
  })
})

it("round trips flat rules and matches server validation", () => {
  const base = { ...rules, flat_time: "15:45", no_overnight: true }
  const form = planForm({ initial_cash: "100000", rules: base })
  expect(form.flat_time).toBe("15:45")
  expect(form.no_overnight).toBe("yes")
  expect(customPlan(form, rules)).toMatchObject({ rules: { flat_time: "15:45", no_overnight: true } })
  for (const flat_time of ["17:00", "24:00", "9:30", "15:60"])
    expect(customPlan({ ...form, flat_time }, rules)).toEqual({ error: "flat_time must be HH:MM New York time from 00:00 to 23:59, before day_end" })
  expect(customPlan({ ...form, flat_time: "", no_overnight: "no" }, rules)).toMatchObject({ rules: { flat_time: null, no_overnight: false } })
})
it("blocks flat-time entries at the market clock and announces the deadline", () => {
  const r = { ...rules, flat_time: "15:45", day_end: "18:00" }
  const before = "2026-11-03T20:30:00Z", after = "2026-11-03T20:45:00Z"
  expect(timeRuleNotices(account.evaluation, r, before)).toEqual(["Flat by 15:45 ET, positions will be closed"])
  expect(timeRuleNotices(account.evaluation, r, after)).toEqual(["Flat time passed; openings blocked until the day ends"])
  expect(planEntryNotice(r, "SPX", before, false)).toBeNull()
  expect(planEntryNotice(r, "SPX", after, false)).toContain("FLAT_TIME")
  expect(planEntryNotice(r, "SPX", after, true)).toBeNull()
  expect(planEntryNotice(r, "SPX", "2026-11-03T23:00:00Z", false)).toBeNull()
  expect(decisionLabel("OVERNIGHT_HOLD")).toBe("overnight hold")
})

it("uses the server flat flag for the same clock, including holidays", () => {
  const holiday = { ...account, time: "2026-11-26T21:00:00Z", rules: { ...rules, flat_time: "15:45" },
    evaluation: { ...account.evaluation, flat_now: false } }
  expect(planEntryNotice(holiday.rules, "SPX", holiday.time, false, holiday)).toBeNull()
  expect(timeRuleNotices(holiday.evaluation, holiday.rules, holiday.time)).toEqual([])
  expect(planEntryNotice(holiday.rules, "SPX", "2026-11-28T21:00:00Z", false)).toBeNull()
})

it("round trips account size scaling, disables it and validates every field", () => {
  const size_scaling = { profit_percent: 10, payouts: 2, days: 80, increase_percent: 25, max_balance: "200000.000001" }
  const base = { ...rules, phase: "funded" as const, size_scaling }
  const form = planForm({ initial_cash: "100000", rules: base })
  const result = customPlan(form, base)
  expect("error" in result ? result : result.rules.size_scaling).toEqual(size_scaling)
  for (const patch of [{ size_profit_percent: "0" }, { size_profit_percent: "101" }, { size_payouts: "-1" },
    { size_payouts: "101" }, { size_days: "0" }, { size_days: "367" }, { size_days: "1.5" }, { size_days: "" },
    { size_increase_percent: "0" }, { size_increase_percent: "101" }, { size_max_balance: "99999.999999" },
    { size_max_balance: "oops" }]) expect(customPlan({ ...form, ...patch }, base)).toHaveProperty("error")
  const off = customPlan({ ...form, size_scaling_enabled: "no", size_days: "oops" }, base)
  expect("error" in off ? off : off.rules.size_scaling).toBeNull()
  for (const phase of ["evaluation", "verification"] as const) {
    const evaluation = customPlan({ ...form, phase }, base)
    expect("error" in evaluation ? evaluation : evaluation.rules.size_scaling).toBeNull()
  }
})

it("keeps combined rules while switching phase-specific time and size scaling", () => {
  const base: AccountRules = { ...rules, time_limit_days: 30, inactivity_days: 14, underlyings: ["SPX"],
    flat_time: "15:45", no_overnight: true,
    events: [{ kind: "news", time: "2026-09-22T14:00:00Z" }, { kind: "split", time: "2026-09-23", symbol: "SPX" }],
    news_before_minutes: 5, news_after_minutes: 10, news_action: "flatten", hold_restrictions: ["split"], hold_cutoff: "15:40",
    trading_start: "09:30", trading_end: "16:00", scaling: [{ profit: "0.00", contracts: 2 }],
    size_scaling: { profit_percent: 10, payouts: 2, days: 80, increase_percent: 25, max_balance: "200000.00" } }
  const form = planForm({ initial_cash: "100000", rules: base })
  const shared = { inactivity_days: 14, underlyings: ["SPX"], trading_start: "09:30", trading_end: "16:00",
    events: base.events, news_before_minutes: 5, news_after_minutes: 10, news_action: "flatten",
    hold_restrictions: ["split"], hold_cutoff: "15:40",
    flat_time: "15:45", no_overnight: true, scaling: base.scaling }
  const evaluation = customPlan({ ...form, phase: "evaluation" }, base)
  expect("error" in evaluation ? evaluation : evaluation.rules).toMatchObject({ ...shared, time_limit_days: 30, size_scaling: null })
  const funded = customPlan({ ...form, phase: "funded" }, base)
  expect("error" in funded ? funded : funded.rules).toMatchObject({ ...shared, time_limit_days: 0, size_scaling: base.size_scaling })
})

it.each(["evaluation", "verification"] as const)("edits and explains trade consistency with integer validation (%s)", (phase) => {
  const form = { ...planForm({ initial_cash: "100000", rules: { ...rules, phase } }), trade_consistency_percent: "40" }
  const result = customPlan(form, rules)
  expect("error" in result ? null : result.rules.trade_consistency_percent).toBe(40)
  expect(objectiveFacts({ ...rules, phase, trade_consistency_percent: 40 })).toContain("Best closed whole trade at most 40% of attempt profit")
  for (const value of ["-1", "101", "1.5"]) expect(customPlan({ ...form, trade_consistency_percent: value }, rules)).toHaveProperty("error")
  expect(objectiveValue({ code: "TRADE_CONSISTENCY", met: false, actual: 75, required: 40, message: "" })).toBe("75% of 40% max")
})

it.each(["evaluation", "verification"] as const)("edits minimum closed trades and clears evaluation rules when funded (%s)", (phase) => {
  const form = { ...planForm({ initial_cash: "100000", rules: { ...rules, phase } }), min_trades: "10" }
  const result = customPlan(form, rules)
  expect("error" in result ? null : result.rules.min_trades).toBe(10)
  for (const min_trades of ["-1", "10001", "1.5"]) expect(customPlan({ ...form, min_trades }, rules)).toHaveProperty("error")
  expect(objectiveFacts({ ...rules, phase, min_trades: 10 })).toContain("At least 10 closed whole trades")
  const funded = customPlan({ ...form, phase: "funded", trade_consistency_percent: "40" }, rules)
  expect("error" in funded ? null : funded.rules).toMatchObject({ min_trades: 0, trade_consistency_percent: 0 })
})

it.each(["evaluation", "verification"] as const)("validates hold time and paired microscalping and shows percentage progress (%s)", (phase) => {
  const form = { ...planForm({ initial_cash: "100000", rules: { ...rules, phase } }), min_hold_seconds: "60", microscalp_seconds: "30", microscalp_percent: "25" }
  const result = customPlan(form, rules)
  expect("error" in result ? null : result.rules).toMatchObject({ min_hold_seconds: 60, microscalp_seconds: 30, microscalp_percent: 25 })
  for (const change of [{ min_hold_seconds: "3601" }, { min_hold_seconds: "1.5" }, { microscalp_seconds: "3601" }, { microscalp_percent: "101" }, { microscalp_seconds: "" }, { microscalp_percent: "" }])
    expect(customPlan({ ...form, ...change }, rules)).toHaveProperty("error")
  expect(objectiveFacts({ ...rules, phase, microscalp_seconds: 30, microscalp_percent: 25 })).toContain("Profit from round trips under 30s at most 25% of attempt profit")
  expect(objectiveValue({ code: "MICROSCALPING", met: false, actual: null, required: 25, message: "" })).toBe("— of 25% max")
  const disabled = customPlan({ ...form, microscalp_percent: "0" }, rules)
  expect("error" in disabled ? null : disabled.rules.microscalp_percent).toBe(0)
  const funded = customPlan({ ...form, phase: "funded" }, rules)
  expect("error" in funded ? null : funded.rules).toMatchObject({ min_hold_seconds: 60, microscalp_seconds: 0, microscalp_percent: 0 })
})

it.each(["evaluation", "verification"] as const)("edits direction bans and retains them in funded plans (%s)", (phase) => {
  const form = { ...planForm({ initial_cash: "100000", rules: { ...rules, phase } }), no_hedging: "yes" as const, no_counter_positions: "yes" as const }
  const result = customPlan(form, rules)
  expect("error" in result ? result : result.rules).toMatchObject({ no_hedging: true, no_counter_positions: true })
  const funded = customPlan({ ...form, phase: "funded" }, rules)
  expect("error" in funded ? funded : funded.rules).toMatchObject({ no_hedging: true, no_counter_positions: true })
  expect(planForm({ initial_cash: "100000", rules }).no_hedging).toBe("no")
})

it("validates whole volume percentages and restores the disabled default", () => {
  const form = planForm({ initial_cash: "100000", rules })
  expect(form.max_volume_percent).toBe("")
  for (const max_volume_percent of ["1", "25", "100", "0", ""]) {
    const result = customPlan({ ...form, max_volume_percent }, rules)
    expect("error" in result ? result : result.rules.max_volume_percent).toBe(Number(max_volume_percent))
  }
  for (const max_volume_percent of ["-1", "101", "1.5", "NaN"])
    expect(customPlan({ ...form, max_volume_percent }, rules)).toHaveProperty("error")
})

it("keeps verification restrictions alongside program costs and names its deadline", () => {
  const base: AccountRules = { ...rules, phase: "verification", time_limit_days: 30, inactivity_days: 14,
    underlyings: ["SPX"], trading_start: "09:30", trading_end: "16:00", flat_time: "15:45", no_overnight: true,
    evaluation_fee: "100.000001", reset_fee: "25.000002", activation_fee: "50.000003", max_resets: 2 }
  const form = planForm({ initial_cash: "100000", rules: base })
  expect(customPlan(form, base)).toMatchObject({ rules: { ...base, plan: "Custom plan", plan_id: null } })
  expect(customPlan({ ...form, time_limit_days: "1.5" }, base)).toEqual({ error: "Verification time limit must be a whole number from 0 to 366" })
  expect(customPlan({ ...form, inactivity_days: "367" }, base)).toHaveProperty("error")
  const facts = timeRuleFacts(base).join(" ")
  for (const text of ["Verification ends after 30", "Inactivity limit: 14", "Allowed underlyings: SPX", "09:30–16:00", "Flat by 15:45", "OVERNIGHT_HOLD"])
    expect(facts).toContain(text)
  expect(timeRuleNotices({ ...account.evaluation, days_left: 3, deadline: "2026-09-25" }, base))
    .toEqual(["Verification: 3 calendar days left; deadline 2026-09-25."])
  expect(customPlan({ ...form, phase: "funded", time_limit_days: "invalid" }, base))
    .toMatchObject({ rules: { phase: "funded", time_limit_days: 0, inactivity_days: 14, no_overnight: true } })
})
