import type { Account, AccountRules, DailyLossBasis, Evaluation, EventWindow, Money, Objective, PlanEvent, SizeScaling } from "../api/trading-types"
import { compareMoney, ruleMoney, validMoney, formatMoney, subtractMoney } from "./trading"
import { newYorkDate } from "./journal"

export function phaseFact(rules: AccountRules): string {
  if (rules.phase === "verification") return "Step 2 of 2: verification"
  if (rules.phase === "funded") return "Funded account (simulated)"
  if (rules.plan_id?.startsWith("two-step-") || rules.plan?.startsWith("Two-step Challenge")) return "Step 1 of 2: challenge"
  // Match AccountRules::evaluation() on the server.
  const evaluation = [rules.profit_target, rules.max_drawdown, rules.daily_loss_limit].some((amount) => compareMoney(amount, "0") === 1)
    || (rules.time_limit_days ?? 0) > 0 || (rules.inactivity_days ?? 0) > 0
    || rules.no_overnight || (rules.hold_restrictions?.length ?? 0) > 0
  return evaluation ? "Evaluation" : "Practice"
}

/** "18:00" as "6:00 pm"; "24:00" as "midnight". */
export function clockText(clock: string): string {
  const [hours = 0, minutes = 0] = clock.split(":").map(Number)
  if (hours === 24 || hours === 0) return "midnight"
  return `${hours % 12 || 12}:${String(minutes).padStart(2, "0")} ${hours < 12 ? "am" : "pm"}`
}
/** When the plan's trading day ends, New York time; servers before plan days end it at 17:00. */
export function dayEnd(rules: Pick<AccountRules, "day_end">): string { return rules.day_end ?? "17:00" }

export const dailyLossBasisText: Record<DailyLossBasis, string> = {
  equity: "the day's opening equity",
  balance: "the day's opening balance",
  higher: "the higher of the day's opening balance and equity",
  peak: "the day's equity high",
}

/** The floor in a few words: static, or trailing and where it locks. */
export function drawdownFact(rules: AccountRules, initialCash: Money): string {
  if (!rules.max_drawdown) return "No drawdown floor"
  const amount = ruleMoney(rules.max_drawdown)
  if (rules.drawdown_mode === "static") return `${amount} static drawdown: the floor stays at ${ruleMoney(subtractMoney(initialCash, rules.max_drawdown))}`
  const lock = rules.lock_at_start ? ", locks at the starting balance" : rules.lock_balance ? `, locks at ${ruleMoney(rules.lock_balance)}` : ""
  return `${amount} trailing drawdown (${rules.drawdown_mode === "intraday" ? "intraday" : "end of day"})${lock}`
}
/** How the floor moves, for plan tables. */
export function floorMoves(rules: AccountRules): string {
  if (!rules.max_drawdown) return "—"
  if (rules.drawdown_mode === "static") return "Never (static)"
  const moves = rules.drawdown_mode === "intraday" ? "Every new high" : "At each close"
  return rules.lock_at_start ? `${moves}, to the start` : moves
}
export function dailyLossFact(rules: AccountRules): string | null {
  if (!rules.daily_loss_limit) return null
  return `${ruleMoney(rules.daily_loss_limit)} daily loss limit from ${dailyLossBasisText[rules.daily_loss_basis ?? "equity"]}: ${
    rules.daily_loss_action === "fail" ? "fails the attempt" : "closes every position and locks the day"}`
}
export function scalingFact(rules: Pick<AccountRules, "scaling">): string | null {
  if (!rules.scaling?.length) return null
  return `Scaling: ${rules.scaling.map((s) => `${s.contracts} contracts at ${formatMoney(s.profit)} profit`).join("; ")} · next session, on closed balance`
}
/** The pass conditions beyond the target, in a few words each. */
export function objectiveFacts(rules: AccountRules): string[] {
  const facts: string[] = []
  if (rules.min_trading_days) facts.push(`At least ${rules.min_trading_days} trading ${rules.min_trading_days === 1 ? "day" : "days"}`)
  if (rules.min_profitable_days) facts.push(`At least ${rules.min_profitable_days} profitable ${rules.min_profitable_days === 1 ? "day" : "days"}${
    rules.profitable_day_profit ? ` of ${ruleMoney(rules.profitable_day_profit)}+` : ""}`)
  if (rules.consistency_percent) facts.push(`Best day at most ${rules.consistency_percent}% of ${
    rules.consistency_basis === "positive_days" ? "the profitable days' total" : "the total profit"}`)
  if (rules.phase !== "funded" && rules.microscalp_percent) facts.push(`Profit from round trips under ${rules.microscalp_seconds}s at most ${rules.microscalp_percent}% of attempt profit`)
  if (rules.phase !== "funded" && rules.min_trades) facts.push(`At least ${rules.min_trades} closed whole ${rules.min_trades === 1 ? "trade" : "trades"}`)
  if (rules.phase !== "funded" && rules.trade_consistency_percent) facts.push(`Best closed whole trade at most ${rules.trade_consistency_percent}% of attempt profit`)
  return facts
}
export function targetFact(rules: AccountRules): string {
  if (!rules.profit_target) return rules.phase === "funded" ? "Funded: no profit target" : "No profit target"
  return `${ruleMoney(rules.profit_target)} profit target${rules.profit_basis === "balance" ? " on the closed balance" : ""}`
}
export function dayEndFact(rules: AccountRules): string | null {
  return dayEnd(rules) === "17:00" ? null : `Trading day ends at ${clockText(dayEnd(rules))} ET`
}

const eventStamp = new Intl.DateTimeFormat("en-US", {
  timeZone: "America/New_York",
  weekday: "short",
  month: "short",
  day: "numeric",
  hour: "2-digit",
  minute: "2-digit",
  hourCycle: "h23",
})
const eventClock = new Intl.DateTimeFormat("en-US", {
  timeZone: "America/New_York",
  hour: "2-digit",
  minute: "2-digit",
  hourCycle: "h23",
})
function eventTimestampET(iso: string): string {
  const timestamp = Date.parse(iso)
  return Number.isFinite(timestamp) ? `${eventStamp.format(timestamp)} ET` : iso
}
function eventWindowET(start: string, end: string): string {
  const startTime = Date.parse(start), endTime = Date.parse(end)
  if (!Number.isFinite(startTime) || !Number.isFinite(endTime)) return `${start} until ${end}`
  const sameDay = newYorkDate(start)?.date === newYorkDate(end)?.date
  return `${eventStamp.format(startTime)} ET until ${sameDay ? eventClock.format(endTime) : eventStamp.format(endTime)} ET`
}
const sessionText: Record<NonNullable<PlanEvent["session"]>, string> = { before_open: "before open", after_close: "after close" }
function planEventFact(e: PlanEvent): string {
  const when = e.kind === "news" ? eventTimestampET(e.time) : e.time.slice(0, 10)
  return `${e.label || e.kind} ${e.symbol || "all underlyings"} ${when}${e.session ? ` ${sessionText[e.session]}` : ""}`
}
function eventWindowKey(w: EventWindow): string {
  return `${w.kind}\0${w.symbol ?? ""}\0${w.label ?? ""}\0${w.start}\0${w.end}`
}
function uniqueWindows(windows: EventWindow[]): EventWindow[] {
  const seen = new Set<string>()
  return windows.filter((window) => {
    const key = eventWindowKey(window)
    if (seen.has(key)) return false
    seen.add(key)
    return true
  })
}

export const objectiveLabels: Record<string, string> = {
  PROFIT_TARGET: "Profit target",
  MIN_TRADING_DAYS: "Trading days",
  MIN_PROFITABLE_DAYS: "Profitable days",
  CONSISTENCY: "Consistency",
  MICROSCALPING: "Microscalping",
  MIN_TRADES: "Closed trades",
  TRADE_CONSISTENCY: "Trade consistency",
}
/** An objective's standing in a few characters: dollars, days or percent. */
export function objectiveValue(o: Objective): string {
  if (o.code === "PROFIT_TARGET") return `${formatMoney(o.actual?.toFixed(2))} of ${ruleMoney(o.required.toFixed(2))}`
  if (o.code === "CONSISTENCY" || o.code === "TRADE_CONSISTENCY" || o.code === "MICROSCALPING") return `${o.actual == null ? "—" : `${Math.round(o.actual)}%`} of ${o.required}% max`
  return `${o.actual ?? 0} of ${o.required}`
}
export const decisionLabels: Record<string, string> = {
  PROFIT_TARGET: "profit target",
  DRAWDOWN_FLOOR: "drawdown floor",
  DAILY_LOSS_LIMIT: "daily loss limit",
  FLAT_TIME: "mandatory flat time", OVERNIGHT_HOLD: "overnight hold",
  NEWS_BLACKOUT: "news blackout", HOLD_RESTRICTED: "holding restriction",
  TIME_LIMIT: "evaluation time limit", INACTIVITY: "inactivity limit",
  INSTRUMENT_NOT_ALLOWED: "underlying not allowed", OUTSIDE_PLAN_HOURS: "outside plan trading hours",
}
/** Why an attempt ended, as a short label: "daily loss limit". */
export function decisionLabel(code: string | null | undefined): string | null {
  return code ? decisionLabels[code] ?? code.toLowerCase().replaceAll("_", " ") : null
}
/** The plan's daily-loss room as a share of the limit, for meters; null without a limit. */
export function dailyLossShare(e: Pick<Evaluation, "daily_loss">): number | null {
  const d = e.daily_loss
  if (!d) return null
  const room = Number(d.room), limit = Number(d.limit)
  return Number.isFinite(room) && limit > 0 ? Math.max(0, Math.min(1, room / limit)) : null
}
/** The order tickets' notice while a plan limit locks the day. */
export const dayLockNotice = "The plan's daily loss limit locked trading until the next trading day · reduce-only: closing orders still work."

/** Optional entry rules; older servers and plans leave them off. */
export function tradeRuleFacts(r: AccountRules): string[] {
  return [
    ...(r.min_hold_seconds ? [`Hold at least ${r.min_hold_seconds}s before user reductions; protective exits and flatten remain available`] : []),
    ...(r.max_volume_percent ? [`Held plus opening contracts at most ${r.max_volume_percent}% of current-date option volume; checked again at fill; unknown volume refuses`] : []),
    ...(r.no_hedging ? ["No opening hedges against this account’s held direction"] : []),
    ...(r.no_counter_positions ? ["No counter positions against other live accounts’ holdings or working entries; checked again at fill"] : []),
    ...(r.max_contracts_held ? [`At most ${r.max_contracts_held} option contracts held or opening`] : []),
    ...(r.require_stop_loss ? ["Stop-loss required on every entry"] : []),
    ...(r.max_trade_risk && Number(r.max_trade_risk) > 0 ? [`Trade risk at most ${formatMoney(r.max_trade_risk)} before fees`] : []),
    ...(r.max_trade_risk_percent ? [`Trade risk at most ${r.max_trade_risk_percent}% of room to the plan floor`] : []),
  ]
}

/** Optional time and product restrictions, shared by the plan chooser and Rules. */
export function timeRuleEntries(r: AccountRules): { title: string; body: string }[] {
  return [
    ...(r.flat_time ? [{ title: "Mandatory flat time", body: `Flat by ${r.flat_time} ET: options and shares close; openings blocked until ${dayEnd(r)} ET. Unfilled closes retry on fresh quotes.` }] : []),
    ...(r.no_overnight ? [{ title: "No overnight holds", body: `Positions held at the ${dayEnd(r)} ET rollover fail the attempt (OVERNIGHT_HOLD); positions awaiting settlement are excluded` }] : []),
    ...((r.news_before_minutes || r.news_after_minutes) ? [{ title: "News blackouts", body: `News: ${r.news_before_minutes ?? 0} minutes before / ${r.news_after_minutes ?? 0} after; ${r.news_action === "flatten" ? "closes positions once" : "blocks openings"}; exits keep working` }] : []),
    ...(r.hold_restrictions?.length ? [{ title: "Holding restrictions", body: `Close before ${r.hold_cutoff ?? "15:45"} ET for ${r.hold_restrictions.join(", ")}; holding across the boundary fails the attempt` }] : []),
    ...(r.events?.length ? [{ title: "Plan event calendar", body: `${r.events.length} saved events: ${r.events.map(planEventFact).join("; ")}` }] : []),
    ...(r.time_limit_days ? [{ title: `${r.phase === "verification" ? "Verification" : "Evaluation"} time limit`, body: `${r.phase === "verification" ? "Verification" : "Evaluation"} ends after ${r.time_limit_days} calendar days` }] : []),
    ...(r.inactivity_days ? [{ title: "Inactivity limit", body: `Inactivity limit: ${r.inactivity_days} calendar days without your own execution` }] : []),
    ...(r.underlyings?.length ? [{ title: "Allowed underlyings", body: `Allowed underlyings: ${r.underlyings.join(", ")}${r.underlyings.includes("SPX") ? " (SPX includes SPXW options)" : ""}` }] : []),
    ...(r.trading_start && r.trading_end ? [{ title: "Opening hours", body: `Opening hours: ${r.trading_start}–${r.trading_end} ET; exits keep working` }] : []),
  ]
}
export function timeRuleFacts(r: AccountRules): string[] { return timeRuleEntries(r).map((entry) => entry.body) }
const planClock = new Intl.DateTimeFormat("en-GB", { timeZone: "America/New_York", hour: "2-digit", minute: "2-digit", hourCycle: "h23" })
export function flatRuleNotice(r: AccountRules | undefined): string | null {
  if (!r) return null
  const parts = [
    ...(r.flat_time ? [`Flat by ${r.flat_time} ET; positions will be closed and openings blocked until ${dayEnd(r)} ET.`] : []),
    ...(r.no_overnight ? ["No overnight holds: positions at day rollover fail the attempt; awaiting settlement is excluded."] : []),
  ]
  return parts.length ? parts.join(" ") : null
}
/** Ticket checks use the account's market clock, including paused and delayed feeds. */
export function planEntryNotice(r: AccountRules | undefined, underlying: string, time: string | undefined, reducing: boolean, account?: Pick<Account, "time" | "evaluation"> | null): string | null {
  if (!r || reducing) return null
  const flat = account?.evaluation.flat_now != null && Date.parse(account.time) === Date.parse(time ?? "")
    ? account.evaluation.flat_now : flatNow(r, time)
  if (flat) return "FLAT_TIME: Flat time passed; openings blocked until the day ends. Closing orders still work."
  const timestamp = Date.parse(time ?? "")
  for (const e of r.events ?? []) {
    if (e.kind !== "news" || (e.symbol && e.symbol !== underlying) || !(r.news_before_minutes || r.news_after_minutes)) continue
    const at = Date.parse(e.time), end = at + (r.news_after_minutes ?? 0) * 60_000
    if (timestamp >= at - (r.news_before_minutes ?? 0) * 60_000 && timestamp < end)
      return `NEWS_BLACKOUT: ${e.label || "news"} blocks openings until ${eventTimestampET(new Date(end).toISOString())}; closing orders still work.`
  }
  const evaluation = account?.evaluation
  for (const e of [...(evaluation?.active_events ?? []), ...(evaluation?.next_event ? [evaluation.next_event] : [])]) {
    if (e.kind !== "news" && (!e.symbol || e.symbol === underlying) && timestamp >= Date.parse(e.start) && timestamp < Date.parse(e.end))
      return `HOLD_RESTRICTED: ${e.kind} holding cutoff reached for ${e.symbol || "the account"}; closing orders still work.`
  }
  if (r.underlyings?.length && !r.underlyings.includes(underlying))
    return `INSTRUMENT_NOT_ALLOWED: ${underlying} is outside this plan's allowed underlyings (${r.underlyings.join(", ")}); closing orders still work.`
  if (r.trading_start && r.trading_end && Number.isFinite(timestamp)) {
    const clock = planClock.format(timestamp)
    if (clock < r.trading_start || clock >= r.trading_end)
      return `OUTSIDE_PLAN_HOURS: opening orders work from ${r.trading_start} to ${r.trading_end} ET; closing orders still work.`
  }
  return null
}
export function flatNow(r: AccountRules, time: string | undefined): boolean {
  const timestamp = Date.parse(time ?? "")
  if (!r.flat_time || !Number.isFinite(timestamp)) return false
  const day = newYorkDate(time!)
  if (!day || day.weekday === 0 || day.weekday === 6) return false
  const clock = planClock.format(timestamp)
  return clock >= r.flat_time && clock < dayEnd(r)
}
export function timeRuleNotices(e: Evaluation, r: AccountRules, time?: string): string[] {
  if (e.status !== "active") return []
  const minute = (clock: string) => Number(clock.slice(0, 2)) * 60 + Number(clock.slice(3))
  const timestamp = Date.parse(time ?? "")
  const remaining = r.flat_time && Number.isFinite(timestamp) ? minute(r.flat_time) - minute(planClock.format(timestamp)) : null
  const activeNews = uniqueWindows((e.active_events ?? []).filter((w) => w.kind === "news"))
  const activeNewsKeys = new Set(activeNews.map(eventWindowKey))
  const nextEvent = e.next_event && !(e.next_event.kind === "news" && e.next_event.active && activeNewsKeys.has(eventWindowKey(e.next_event)))
    ? e.next_event : null
  return [
    ...((e.flat_now ?? flatNow(r, time)) ? ["Flat time passed; openings blocked until the day ends"]
      : remaining != null && remaining > 0 && remaining <= 30 ? [`Flat by ${r.flat_time} ET, positions will be closed`] : []),
    ...activeNews.map((w) => `Active news blackout: ${w.label || "News"}, ${w.symbol || "all underlyings"}, until ${eventTimestampET(w.end)}. Closing orders still work.`),
    ...(nextEvent ? [`${nextEvent.active ? "Active" : "Next"} ${nextEvent.kind === "news" ? "news blackout" : `${nextEvent.kind} holding cutoff`}: ${nextEvent.label || nextEvent.kind}, ${nextEvent.symbol || "all underlyings"}, ${eventWindowET(nextEvent.start, nextEvent.end)}.`] : []),
    ...(e.days_left != null && e.deadline ? [`${r.phase === "verification" ? "Verification" : "Evaluation"}: ${e.days_left} calendar days left; deadline ${e.deadline}.`] : []),
    ...(r.inactivity_days && e.inactive_days != null && e.inactivity_deadline && r.inactivity_days - e.inactive_days <= 7
      ? [`Inactivity: ${Math.max(0, r.inactivity_days - e.inactive_days)} calendar days left to execute a trade; deadline ${e.inactivity_deadline}.`] : []),
  ]
}

/** The newest known market timestamp, without consulting the wall clock. */
export function planMarketTime(...times: (string | null | undefined)[]): string | undefined {
  return times.filter((time): time is string => !!time && Number.isFinite(Date.parse(time)))
    .sort((a, b) => Date.parse(b) - Date.parse(a))[0]
}

export interface SizeScalingForm {
  size_scaling_enabled: "yes" | "no"
  size_profit_percent: string
  size_payouts: string
  size_days: string
  size_increase_percent: string
  size_max_balance: string
}
export function sizeScalingRule(form: SizeScalingForm, phase: AccountRules["phase"], initial: Money): { value: SizeScaling | null } | { error: string } {
  if (phase !== "funded" || form.size_scaling_enabled === "no") return { value: null }
  for (const [label, value, min, max] of [["Review profit percent", form.size_profit_percent, 1, 100],
    ["Review payouts", form.size_payouts, 0, 100], ["Review trading days", form.size_days, 1, 366],
    ["Account size increase percent", form.size_increase_percent, 1, 100]] as const) {
    if (!/^\d+$/.test(value.trim()) || Number(value) < min || Number(value) > max)
      return { error: `${label} must be a whole number from ${min} to ${max}` }
  }
  if (!validMoney(form.size_max_balance.trim()) || compareMoney(form.size_max_balance.trim(), initial.trim()) === -1)
    return { error: "Maximum account size must be a dollar amount at least the starting balance" }
  return { value: { profit_percent: Number(form.size_profit_percent), payouts: Number(form.size_payouts), days: Number(form.size_days),
    increase_percent: Number(form.size_increase_percent), max_balance: form.size_max_balance.trim() } }
}
export function sizeScalingFact(rules: Pick<AccountRules, "size_scaling">): string | null {
  const s = rules.size_scaling
  if (!s) return null
  return `Account size scaling: review every ${s.days} finished trading days; at least ${s.profit_percent}% net realised profit and ${s.payouts} payouts. Add ${s.increase_percent}% of the original size, up to ${formatMoney(s.max_balance)}`
}
