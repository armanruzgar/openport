import { useRef, useState, type ReactNode } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { AccountRules, DailyLossBasis, Money, Plan, PlanEvent, HoldRestriction } from "../api/trading-types"
import { dailyLossBasisText, dayEnd, sizeScalingRule, type SizeScalingForm } from "../lib/plan-rules"
import { compareMoney, validMoney } from "../lib/trading"
import { propForm, propRules, type PropRulesForm } from "../lib/prop-rules"

/** The custom plan form: text fields as typed, choices as their API words. */
export interface PlanForm extends SizeScalingForm, PropRulesForm {
  events: PlanEvent[]
  news_before_minutes: string
  news_after_minutes: string
  news_action: "block" | "flatten"
  hold_restrictions: HoldRestriction[]
  hold_cutoff: string
  evaluation_fee: string
  reset_fee: string
  activation_fee: string
  max_resets: string
  name: string
  scaling: { profit: string; contracts: string }[]
  phase: AccountRules["phase"]
  qualifying_profit: string
  qualifying_days: string
  withdrawal_percent: string
  split_percent: string
  payout_minimum: string
  payout_caps: string
  payout_consistency_percents: string
  payout_buffer: string
  buffer_payouts: string
  initial_cash: string
  profit_target: string
  profit_basis: "equity" | "balance"
  max_drawdown: string
  drawdown_mode: AccountRules["drawdown_mode"]
  lock: "none" | "start" | "balance"
  lock_balance: string
  daily_loss_limit: string
  daily_loss_basis: DailyLossBasis
  daily_loss_action: "lock" | "fail"
  min_hold_seconds: string
  microscalp_seconds: string
  microscalp_percent: string
  min_trades: string
  trade_consistency_percent: string
  consistency_percent: string
  consistency_basis: "total" | "positive_days"
  min_trading_days: string
  min_profitable_days: string
  profitable_day_profit: string
  day_end: string
  max_volume_percent: string
  no_hedging: "yes" | "no"
  no_counter_positions: "yes" | "no"
  max_contracts_held: string
  require_stop_loss: "yes" | "no"
  max_trade_risk: string
  max_trade_risk_percent: string
  time_limit_days: string
  inactivity_days: string
  underlyings: string
  trading_start: string
  trading_end: string
  flat_time: string
  no_overnight: "yes" | "no"
  strategies: "buy_only" | "defined_risk" | "any"
}

/** A form that starts from a preset's own rules, under a name of its own. */
export function planForm(plan: Pick<Plan, "initial_cash" | "rules">): PlanForm {
  const r = plan.rules
  const p = r.payouts
  return {
    ...propForm(r),
    events: r.events?.map((e) => ({ ...e })) ?? [],
    news_before_minutes: String(r.news_before_minutes ?? 0), news_after_minutes: String(r.news_after_minutes ?? 0),
    news_action: r.news_action ?? "block", hold_restrictions: [...(r.hold_restrictions ?? [])], hold_cutoff: r.hold_cutoff ?? "15:45",
    phase: r.phase,
    size_scaling_enabled: r.size_scaling ? "yes" : "no",
    size_profit_percent: String(r.size_scaling?.profit_percent ?? 10), size_payouts: String(r.size_scaling?.payouts ?? 2),
    size_days: String(r.size_scaling?.days ?? 80), size_increase_percent: String(r.size_scaling?.increase_percent ?? 25),
    size_max_balance: r.size_scaling?.max_balance ?? plan.initial_cash,
    scaling: (r.scaling ?? []).map((step) => ({ profit: step.profit, contracts: String(step.contracts) })),
    evaluation_fee: r.evaluation_fee ?? "0", reset_fee: r.reset_fee ?? "0",
    activation_fee: r.activation_fee ?? "0", max_resets: String(r.max_resets ?? 0),
    qualifying_profit: p?.qualifying_profit ?? "0", qualifying_days: String(p?.qualifying_days ?? 1),
    withdrawal_percent: String(p?.withdrawal_percent ?? 50), split_percent: String(p?.split_percent ?? 80),
    payout_minimum: p?.minimum ?? "0", payout_caps: p?.caps.join(", ") ?? "",
    payout_consistency_percents: p?.consistency_percents?.join(", ") ?? "",
    payout_buffer: p?.buffer ?? "0", buffer_payouts: String(p?.buffer_payouts ?? 0),
    name: "Custom plan", initial_cash: plan.initial_cash,
    profit_target: r.profit_target ?? "", profit_basis: r.profit_basis ?? "equity",
    max_drawdown: r.max_drawdown ?? "", drawdown_mode: r.drawdown_mode,
    lock: r.lock_at_start ? "start" : r.lock_balance ? "balance" : "none", lock_balance: r.lock_balance ?? "",
    daily_loss_limit: r.daily_loss_limit ?? "", daily_loss_basis: r.daily_loss_basis ?? "equity", daily_loss_action: r.daily_loss_action ?? "lock",
    consistency_percent: r.consistency_percent ? String(r.consistency_percent) : "", consistency_basis: r.consistency_basis ?? "total",
    min_hold_seconds: r.min_hold_seconds ? String(r.min_hold_seconds) : "",
    microscalp_seconds: r.microscalp_seconds ? String(r.microscalp_seconds) : "",
    microscalp_percent: r.microscalp_percent ? String(r.microscalp_percent) : "",
    min_trades: r.min_trades ? String(r.min_trades) : "",
    trade_consistency_percent: r.trade_consistency_percent ? String(r.trade_consistency_percent) : "",
    min_trading_days: r.min_trading_days ? String(r.min_trading_days) : "",
    min_profitable_days: r.min_profitable_days ? String(r.min_profitable_days) : "",
    profitable_day_profit: r.profitable_day_profit ?? "", day_end: dayEnd(r),
    max_volume_percent: r.max_volume_percent ? String(r.max_volume_percent) : "",
    no_hedging: r.no_hedging ? "yes" : "no", no_counter_positions: r.no_counter_positions ? "yes" : "no",
    max_contracts_held: r.max_contracts_held ? String(r.max_contracts_held) : "",
    require_stop_loss: r.require_stop_loss ? "yes" : "no", max_trade_risk: r.max_trade_risk ?? "",
    max_trade_risk_percent: r.max_trade_risk_percent ? String(r.max_trade_risk_percent) : "",
    time_limit_days: r.time_limit_days ? String(r.time_limit_days) : "",
    inactivity_days: r.inactivity_days ? String(r.inactivity_days) : "",
    underlyings: r.underlyings?.join(", ") ?? "", trading_start: r.trading_start ?? "", trading_end: r.trading_end ?? "",
    flat_time: r.flat_time ?? "", no_overnight: r.no_overnight ? "yes" : "no",
    strategies: r.buy_only ? "buy_only" : r.defined_risk ? "defined_risk" : "any",
  }
}

const count = (text: string) => text.trim() === "" ? 0 : Number(text.trim())
const amount = (text: string): Money | null => text.trim() === "" || Number(text.trim()) === 0 ? null : text.trim()

/**
 * The reset request's balance and rules from the form, keeping the base preset's other
 * rules (buying power, margin, fills, expiry auto-close); or the first problem found.
 * The server checks every rule again.
 */
export function customPlan(form: PlanForm, base: AccountRules): { initial_cash: Money; rules: AccountRules } | { error: string } {
  const name = form.name.trim()
  if (!name) return { error: "Name the plan" }
  // A static floor hides the lock fields and never locks.
  const lockBalance = form.lock === "balance" && form.drawdown_mode !== "static"
  for (const [label, value, required] of [["Starting balance", form.initial_cash, true], ["Profit target", form.phase === "funded" ? "" : form.profit_target],
    ["Max drawdown", form.max_drawdown], ["Lock balance", lockBalance ? form.lock_balance : ""],
    ["Maximum trade risk", form.max_trade_risk], ["Daily loss limit", form.daily_loss_limit], ["Profitable-day profit", form.phase === "funded" ? "" : form.profitable_day_profit]] as const)
    if ((required || value.trim() !== "") && !validMoney(value.trim())) return { error: `${label} must be a dollar amount` }
  for (const [label, value] of [["Evaluation fee", form.evaluation_fee], ["Reset fee", form.reset_fee], ["Activation fee", form.activation_fee]] as const)
    if (!/^\d+(\.\d{1,6})?$/.test(value.trim()) || !validMoney(value.trim())) return { error: `${label} must be a nonnegative dollar amount` }
  if (!Number.isInteger(count(form.max_resets)) || count(form.max_resets) < 0 || count(form.max_resets) > 1000000)
    return { error: "Maximum resets must be a whole number from 0 to 1000000 (0 is unlimited)" }
  if (!(Number(form.initial_cash) > 0)) return { error: "The starting balance must be above zero" }
  for (const [label, value, most] of [...(form.phase !== "funded" ? [["Microscalp seconds", form.microscalp_seconds, 3600], ["Microscalp percent", form.microscalp_percent, 100], ["Minimum trades", form.min_trades, 10000], ["Trade consistency", form.trade_consistency_percent, 100], ["Consistency", form.consistency_percent, 100], ["Minimum trading days", form.min_trading_days, 366],
    ["Minimum profitable days", form.min_profitable_days, 366], [form.phase === "verification" ? "Verification time limit" : "Evaluation time limit", form.time_limit_days, 366]] : []),
    ["Inactivity limit", form.inactivity_days, 366]] as [string, string, number][]) {
    const n = count(value)
    if (!Number.isInteger(n) || n < 0 || n > most) return { error: `${label} must be a whole number from 0 to ${most}` }
  }
  if (form.phase !== "funded" && ((form.microscalp_seconds.trim() === "") !== (form.microscalp_percent.trim() === "") ||
      (count(form.microscalp_percent) > 0 && count(form.microscalp_seconds) === 0)))
    return { error: "Set both microscalp seconds and percent, or leave both off" }
  for (const [label, value, most] of [["Maximum volume %", form.max_volume_percent, 100], ["Minimum hold seconds", form.min_hold_seconds, 3600], ["Maximum contracts held", form.max_contracts_held, 100000],
    ["Maximum trade risk %", form.max_trade_risk_percent, 100]] as const) {
    const n = count(value)
    if (!Number.isInteger(n) || n < 0 || n > most) return { error: `${label} must be a whole number from 0 to ${most}` }
  }
  if (!/^([01]\d|2[0-4]):[0-5]\d$/.test(form.day_end) || form.day_end < "16:15" || form.day_end > "24:00")
    return { error: "The trading day ends between 16:15 and 24:00" }
  const underlyings = form.underlyings.trim() ? form.underlyings.trim().split(/[\s,]+/) : []
  if (underlyings.length > 32 || new Set(underlyings).size !== underlyings.length || underlyings.some((s) => !/^[A-Z0-9.]{1,12}$/.test(s)))
    return { error: "Use up to 32 distinct uppercase underlying symbols" }
  const clock = (s: string) => /^([01]\d|2[0-3]):[0-5]\d$/.test(s) || s === "24:00"
  if ((form.trading_start || form.trading_end) && (!clock(form.trading_start) || !clock(form.trading_end) || form.trading_start >= form.trading_end))
    return { error: "Set both trading hours as HH:MM ET, with the start before the end" }
  if (form.flat_time && (!/^([01]\d|2[0-3]):[0-5]\d$/.test(form.flat_time) || form.flat_time >= form.day_end))
    return { error: "flat_time must be HH:MM New York time from 00:00 to 23:59, before day_end" }
  for (const value of [form.news_before_minutes, form.news_after_minutes])
    if (!Number.isInteger(count(value)) || count(value) < 0 || count(value) > 240) return { error: "News minutes must be integers from 0 to 240" }
  if (!clock(form.hold_cutoff) || form.hold_cutoff >= form.day_end) return { error: "Holding cutoff must be HH:MM ET before day end" }
  if (form.events.length > 256) return { error: "Use at most 256 events" }
  for (const e of form.events) {
    if ((e.symbol || e.kind !== "news") && !/^[A-Z0-9.]{1,12}$/.test(e.symbol ?? "")) return { error: "Corporate events need an uppercase underlying symbol" }
    if (Array.from(e.label ?? "").length > 64) return { error: "Event labels have at most 64 characters" }
    if (e.kind === "news" ? !/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,9})?Z$/.test(e.time) || !Number.isFinite(Date.parse(e.time))
      : !/^\d{4}-\d{2}-\d{2}$/.test(e.time) || !Number.isFinite(Date.parse(e.time)) || new Date(e.time).toISOString().slice(0, 10) !== e.time)
      return { error: "News events need a UTC timestamp ending in Z; other events need a valid YYYY-MM-DD date" }
  }
  const drawdown = amount(form.max_drawdown)
  if (lockBalance && !amount(form.lock_balance)) return { error: "Enter the balance the floor locks at" }
  const trailing = drawdown != null && form.drawdown_mode !== "static"
  let payouts: AccountRules["payouts"] = null
  if (form.phase === "funded") {
    for (const [label, value] of [["Qualifying day profit", form.qualifying_profit], ["Minimum payout", form.payout_minimum],
      ["Payout buffer", form.payout_buffer]] as const)
      if (!validMoney(value.trim()) || Number(value) < 0) return { error: `${label} must be a nonnegative dollar amount` }
    for (const [label, value, min, max] of [["Qualifying days", form.qualifying_days, 1, 366],
      ["Withdrawal percent", form.withdrawal_percent, 0, 100], ["Trader split percent", form.split_percent, 0, 100],
      ["Buffer payouts", form.buffer_payouts, 0, 100]] as const)
      if (!Number.isInteger(count(value)) || count(value) < min || count(value) > max)
        return { error: `${label} must be a whole number from ${min} to ${max}` }
    const list = (value: string) => value.trim() ? value.split(",").map((v) => v.trim()) : []
    const percents = list(form.payout_consistency_percents)
    if (percents.length > 64 || percents.some((v) => !/^\d+$/.test(v) || Number(v) < 1 || Number(v) > 100))
      return { error: "Payout consistency must be up to 64 comma-separated whole percentages from 1 to 100" }
    const caps = list(form.payout_caps)
    if (caps.length > 64 || caps.some((v) => !validMoney(v) || Number(v) <= 0))
      return { error: "Payout caps must be up to 64 comma-separated positive dollar amounts" }
    payouts = { qualifying_profit: form.qualifying_profit.trim(), qualifying_days: count(form.qualifying_days),
      withdrawal_percent: count(form.withdrawal_percent), split_percent: count(form.split_percent), minimum: form.payout_minimum.trim(),
      caps, consistency_percents: percents.map(Number), buffer: form.payout_buffer.trim(), buffer_payouts: count(form.buffer_payouts) }
  }
  if (form.scaling.length > 16) return { error: "Scaling takes at most 16 steps" }
  for (const [i, step] of form.scaling.entries()) {
    if (!validMoney(step.profit.trim()) || compareMoney(step.profit.trim(), "0") === -1)
      return { error: "Scaling profit must be a nonnegative dollar amount" }
    if (i === 0 && compareMoney(step.profit.trim(), "0") !== 0) return { error: "The first scaling profit must be zero" }
    if (i > 0 && compareMoney(step.profit.trim(), form.scaling[i - 1]!.profit.trim()) !== 1)
      return { error: "Scaling profits must strictly increase" }
    const contracts = count(step.contracts)
    if (!Number.isInteger(contracts) || contracts < 1 || contracts > 10000)
      return { error: "Scaling contracts must be a whole number from 1 to 10000" }
    if (i > 0 && contracts < count(form.scaling[i - 1]!.contracts)) return { error: "Scaling contract limits must not decrease" }
  }
  const sizeScaling = sizeScalingRule(form, form.phase, form.initial_cash)
  if ("error" in sizeScaling) return sizeScaling
  const prop = propRules(form, form.phase, form.phase === "funded" ? null : amount(form.profit_target), drawdown, form.drawdown_mode)
  if ("error" in prop) return prop
  if (payouts) payouts = { ...payouts, ...prop.payouts }
  const rules: AccountRules = {
    ...base, plan: name, plan_id: null, phase: form.phase, payouts, size_scaling: sizeScaling.value,
    scaling: form.scaling.map((step) => ({ profit: step.profit.trim(), contracts: count(step.contracts) })),
    events: form.events, news_before_minutes: count(form.news_before_minutes), news_after_minutes: count(form.news_after_minutes),
    news_action: form.news_action, hold_restrictions: form.hold_restrictions, hold_cutoff: form.hold_cutoff,
    evaluation_fee: form.evaluation_fee.trim(), reset_fee: form.reset_fee.trim(),
    activation_fee: form.activation_fee.trim(), max_resets: count(form.max_resets),
    profit_target: form.phase === "funded" ? null : amount(form.profit_target), profit_basis: form.profit_basis,
    max_drawdown: drawdown, drawdown_mode: form.drawdown_mode,
    lock_at_start: trailing && form.lock === "start", lock_balance: trailing && form.lock === "balance" ? amount(form.lock_balance) : null,
    daily_loss_limit: amount(form.daily_loss_limit), daily_loss_basis: form.daily_loss_basis, daily_loss_action: form.daily_loss_action,
    consistency_percent: form.phase === "funded" ? 0 : count(form.consistency_percent), consistency_basis: form.consistency_basis,
    min_hold_seconds: count(form.min_hold_seconds),
    microscalp_seconds: form.phase === "funded" ? 0 : count(form.microscalp_seconds),
    microscalp_percent: form.phase === "funded" ? 0 : count(form.microscalp_percent),
    min_trades: form.phase === "funded" ? 0 : count(form.min_trades),
    trade_consistency_percent: form.phase === "funded" ? 0 : count(form.trade_consistency_percent),
    min_trading_days: form.phase === "funded" ? 0 : count(form.min_trading_days),
    min_profitable_days: form.phase === "funded" ? 0 : count(form.min_profitable_days),
    profitable_day_profit: form.phase === "funded" ? null : amount(form.profitable_day_profit), day_end: form.day_end,
    max_volume_percent: count(form.max_volume_percent),
    no_hedging: form.no_hedging === "yes", no_counter_positions: form.no_counter_positions === "yes",
    max_contracts_held: count(form.max_contracts_held), require_stop_loss: form.require_stop_loss === "yes",
    max_trade_risk: amount(form.max_trade_risk), max_trade_risk_percent: count(form.max_trade_risk_percent),
    time_limit_days: form.phase === "funded" ? 0 : count(form.time_limit_days), inactivity_days: count(form.inactivity_days), underlyings,
    trading_start: form.trading_start || null, trading_end: form.trading_end || null,
    flat_time: form.flat_time || null, no_overnight: form.no_overnight === "yes",
    buy_only: form.strategies === "buy_only", defined_risk: form.strategies === "defined_risk",
    ...prop.rules,
  }
  return { initial_cash: form.initial_cash.trim(), rules }
}

function Field({ label, hint, children }: { label: string; hint?: string; children: ReactNode }) {
  return <label className="block min-w-0 space-y-1 text-xs">
    <span className="text-muted">{label}</span>
    {children}
    {hint && <span className="block text-[11px] text-faint">{hint}</span>}
  </label>
}

/** Edits a custom plan's rules; the dialog submits it. */
export function PlanEditor({ form, onChange, disabled }: { form: PlanForm; onChange: (form: PlanForm) => void; disabled?: boolean }) {
  const [importStatus, setImportStatus] = useState("")
  const live = useLive()
  // Imports finish later; merge into the form as edited meanwhile.
  const latest = useRef(form)
  latest.current = form
  const mergeEvents = (events: PlanEvent[]) => {
    const form = latest.current
    const merged = [...form.events, ...events].filter((e, i, all) => all.findIndex((v) => JSON.stringify(v) === JSON.stringify(e)) === i)
    if (merged.length > 256) { setImportStatus("Import exceeds 256 events; remove rows first."); return }
    onChange({ ...form, events: merged }); setImportStatus(`Imported ${merged.length - form.events.length} events. Review and save the plan to apply them.`)
  }
  const importCalendar = async () => {
    try { mergeEvents((await api.calendarEvents()).events) } catch (e) { setImportStatus(e instanceof Error ? e.message : "Calendar import failed") }
  }
  const importDividends = async () => {
    try {
      const symbols = form.underlyings.trim() ? form.underlyings.trim().split(/[\s,]+/) : live.underlyings.map((u) => u.symbol)
      const summaries = await Promise.all(symbols.map(async (symbol) => ({ symbol, summary: await api.summary(symbol) })))
      mergeEvents(summaries.flatMap(({ symbol, summary }) => [...new Set(summary.expiries.flatMap((e) => e.dividends?.map((d) => d.ex_date) ?? []))]
        .map((time): PlanEvent => ({ kind: "ex_dividend", time, symbol, label: "Dividend ex-date" }))))
    } catch (e) { setImportStatus(e instanceof Error ? e.message : "Dividend import failed") }
  }
  const changeEvent = (index: number, patch: Partial<PlanEvent>) => onChange({ ...form, events: form.events.map((e, i) => i === index ? { ...e, ...patch } : e) })
  const set = <K extends keyof PlanForm>(key: K) => (value: PlanForm[K]) => onChange({ ...form, [key]: value })
  const text = (key: Exclude<keyof PlanForm, "scaling" | "events" | "hold_restrictions">, placeholder = "") => <input className="trade-input w-full" value={form[key] as string} placeholder={placeholder}
    disabled={disabled} inputMode={["name", "day_end", "payout_caps", "payout_consistency_percents", "underlyings", "trading_start", "trading_end", "flat_time", "hold_cutoff", "review_ladder"].includes(key) ? "text" : "decimal"}
    onChange={(event) => set(key)(event.target.value as never)} />
  const choice = <K extends Exclude<keyof PlanForm, "scaling" | "events" | "hold_restrictions">>(key: K, options: [PlanForm[K], string][]) => <select className="trade-input w-full"
    value={form[key] as string} disabled={disabled} onChange={(event) => set(key)(event.target.value as PlanForm[K])}>
    {options.map(([value, label]) => <option key={String(value)} value={String(value)}>{label}</option>)}
  </select>
  const trailing = form.drawdown_mode !== "static"
  return (
    <fieldset className="grid gap-3 rounded-md border border-border p-3 sm:grid-cols-2" disabled={disabled} aria-label="Custom plan rules">
      <Field label="Plan name" hint="Not a preset's name">{text("name")}</Field>
      <Field label="Evaluation fee" hint="New purchase; bookkeeping only">{text("evaluation_fee")}</Field>
      <Field label="Reset fee" hint="Restart of the same plan">{text("reset_fee")}</Field>
      <Field label="Activation fee" hint="Entering a preset unlocked by a pass">{text("activation_fee")}</Field>
      <Field label="Maximum resets" hint="0 is unlimited; changing costs keeps the count">{text("max_resets")}</Field>
      <Field label="Starting balance">{text("initial_cash")}</Field>
      <Field label="Phase">{choice("phase", [["evaluation", "Evaluation"], ["verification", "Step 2 of 2: verification"], ["funded", "Funded (simulated payouts)"]])}</Field>
      {form.phase !== "funded" && <Field label="Profit target" hint="Blank for none">{text("profit_target", "none")}</Field>}
      {form.phase !== "funded" && <Field label="Profit counts on">{choice("profit_basis", [["equity", "Marked equity"], ["balance", "Closed balance (positions closed)"]])}</Field>}
      <Field label="Max drawdown" hint="Blank for no floor">{text("max_drawdown", "none")}</Field>
      <Field label="Floor">{choice("drawdown_mode", [["intraday", "Trails every new high"], ["end_of_day", "Trails each close"], ["static", "Static: never moves"]])}</Field>
      {trailing && <Field label="Floor locks">{choice("lock", [["none", "Never"], ["start", "At the starting balance"], ["balance", "At a balance"]])}</Field>}
      {trailing && form.lock === "balance" && <Field label="Lock balance">{text("lock_balance")}</Field>}
      <Field label="Daily loss limit" hint="Blank for none">{text("daily_loss_limit", "none")}</Field>
      <Field label="Measured from">{choice("daily_loss_basis", (Object.keys(dailyLossBasisText) as DailyLossBasis[]).map((b) => [b, dailyLossBasisText[b]]))}</Field>
      <Field label="Reaching it">{choice("daily_loss_action", [["lock", "Closes positions and locks the day"], ["fail", "Fails the attempt"]])}</Field>
      <Field label="Minimum hold (seconds)" hint="User reductions only; protective exits and flatten still work">{text("min_hold_seconds", "0")}</Field>
      <Field label="Trading day ends (ET)" hint="HH:MM, 16:15 to 24:00">{text("day_end")}</Field>
      {form.phase !== "funded" && <>
        <Field label="Consistency: best day at most %" hint="Blank for none">{text("consistency_percent", "none")}</Field>
        <Field label="Of">{choice("consistency_basis", [["total", "The total profit"], ["positive_days", "The profitable days' total"]])}</Field>
        <Field label="Microscalp threshold (seconds)" hint="Final close under this age; set with percent">{text("microscalp_seconds", "off")}</Field>
        <Field label="Microscalp profit at most %" hint="Positive net profit from short round trips / attempt profit">{text("microscalp_percent", "off")}</Field>
        <Field label="Minimum closed trades" hint="Whole option trades, including system closures">{text("min_trades", "0")}</Field>
        <Field label="Trade consistency: best trade at most %" hint="Net profit of a closed whole option trade; blank for none">{text("trade_consistency_percent", "none")}</Field>
        <Field label="Minimum trading days">{text("min_trading_days", "0")}</Field>
        <Field label="Minimum profitable days">{text("min_profitable_days", "0")}</Field>
        <Field label="A profitable day makes at least" hint="Blank counts any profit">{text("profitable_day_profit", "any")}</Field>
        <Field label={`${form.phase === "verification" ? "Verification" : "Evaluation"} time limit (days)`} hint="Calendar days; blank for none">{text("time_limit_days", "none")}</Field>
      </>}
      {form.phase === "funded" && <>
        <Field label="Qualifying day profit" hint="Net realised profit after fees">{text("qualifying_profit")}</Field>
        <Field label="Qualifying days per payout">{text("qualifying_days")}</Field>
        <Field label="Withdrawal percent">{text("withdrawal_percent")}</Field>
        <Field label="Trader split percent">{text("split_percent")}</Field>
        <Field label="Minimum payout">{text("payout_minimum")}</Field>
        <Field label="Payout caps" hint="Comma-separated dollars by payout number; last repeats; blank for none">{text("payout_caps")}</Field>
        <Field label="Payout consistency percentages" hint="Best day as a share of cycle net profit. Comma-separated 1–100; last repeats; blank for none">{text("payout_consistency_percents", "20, 25, 30")}</Field>
        <Field label="Payout buffer" hint="Equity to keep above the starting balance; 0 disables">{text("payout_buffer")}</Field>
        <Field label="Buffer payouts" hint="First N payouts; 0 applies to every payout">{text("buffer_payouts")}</Field>
      </>}
      {form.phase === "funded" && <div className="space-y-3 border-t border-border pt-3 sm:col-span-2">
        <h3 className="text-sm font-medium">Account size scaling</h3>
        <label className="flex items-center gap-2 text-xs"><input type="checkbox" checked={form.size_scaling_enabled === "yes"}
          onChange={(event) => set("size_scaling_enabled")(event.target.checked ? "yes" : "no")} />Enable account size scaling</label>
        {form.size_scaling_enabled === "yes" && <div className="grid gap-3 sm:grid-cols-2">
          <Field label="Review profit percent" hint="Net realised profit as a percentage of the size at review start.">{text("size_profit_percent")}</Field>
          <Field label="Review payouts" hint="Completed payouts required; zero for none.">{text("size_payouts")}</Field>
          <Field label="Review trading days" hint="Finished plan days, not calendar days.">{text("size_days")}</Field>
          <Field label="Account size increase percent" hint="Percentage of the original starting balance each time.">{text("size_increase_percent")}</Field>
          <Field label="Maximum account size">{text("size_max_balance")}</Field>
          <p className="text-[11px] text-muted">Each review starts a new period, whether it qualifies or not. Capital growth leaves profit unchanged and scales loss limits with account size.</p>
        </div>}
      </div>}
      <Field label="Maximum volume %" hint="Held plus opening contracts per option, as a whole percent of today’s traded volume. Checked at acceptance and fill; unknown volume refuses. Blank or 0 disables.">{text("max_volume_percent", "none")}</Field>
      <Field label="No hedging" hint="Opening delta cannot oppose this account’s held direction; reductions and exits remain available.">{choice("no_hedging", [["no", "Off"], ["yes", "On"]])}</Field>
      <Field label="No counter positions" hint="Checks other live accounts’ holdings and working entries at acceptance and fill. Set this on every account you trade.">{choice("no_counter_positions", [["no", "Off"], ["yes", "On"]])}</Field>
      <Field label="Maximum contracts held" hint="Held options plus working entries; shares excluded. Blank for none.">{text("max_contracts_held", "none")}</Field>
      <Field label="Stop-loss required">{choice("require_stop_loss", [["no", "Optional"], ["yes", "Required on every entry"]])}</Field>
      <Field label="Maximum trade risk" hint="Dollars before fees. Blank for none.">{text("max_trade_risk", "none")}</Field>
      <Field label="Maximum trade risk %" hint="Of room to the plan floor; ignored without a floor. The tighter cap applies.">{text("max_trade_risk_percent", "none")}</Field>
      <Field label="Inactivity limit (days)" hint="Calendar days since your last execution; blank for none">{text("inactivity_days", "none")}</Field>
      <Field label="Allowed underlyings" hint="Comma-separated uppercase symbols; blank allows all. SPX includes SPXW options.">{text("underlyings", "SPX, XSP, VIX")}</Field>
      <Field label="Trading starts (ET)" hint="HH:MM; set both hours or leave both blank">{text("trading_start", "09:30")}</Field>
      <Field label="Trading ends (ET)" hint="Opening orders cancel at this time; exits keep working">{text("trading_end", "16:00")}</Field>
      <Field label="Flat time (ET)" hint="HH:MM before day end; blank for none. Closes options and shares and blocks openings until day end.">{text("flat_time", "15:45")}</Field>
      <Field label="No overnight holds" hint="Holding positions at day rollover fails the attempt. Positions awaiting settlement are excluded.">{choice("no_overnight", [["no", "Off"], ["yes", "Required"]])}</Field>
      <div className="space-y-2 sm:col-span-2">
        <p className="text-xs font-medium">Scaling plan</p>
        <p className="text-[11px] text-muted">Option contracts held at once; each leg counts, shares do not. Closed-balance profit selects the next session’s limit, up or down. No steps disables scaling.</p>
        {form.scaling.map((step, i) => <div key={i} className="flex items-end gap-2">
          <Field label={`Step ${i + 1} profit`}><input className="trade-input w-full" inputMode="decimal" value={step.profit}
            onChange={(event) => set("scaling")(form.scaling.map((s, n) => n === i ? { ...s, profit: event.target.value } : s))} /></Field>
          <Field label={`Step ${i + 1} contracts`}><input className="trade-input w-full" inputMode="numeric" value={step.contracts}
            onChange={(event) => set("scaling")(form.scaling.map((s, n) => n === i ? { ...s, contracts: event.target.value } : s))} /></Field>
          <button type="button" className="trade-button" aria-label={`Remove scaling step ${i + 1}`}
            onClick={() => set("scaling")(form.scaling.filter((_, n) => n !== i))}>Remove</button>
        </div>)}
        <button type="button" className="trade-button" disabled={disabled || form.scaling.length >= 16}
          onClick={() => set("scaling")([...form.scaling, { profit: form.scaling.length ? "" : "0", contracts: form.scaling.at(-1)?.contracts ?? "2" }])}>Add scaling step</button>
      </div>
      <Field label="News minutes before" hint="0–240; both zero disables blackouts">{text("news_before_minutes")}</Field>
      <Field label="News minutes after">{text("news_after_minutes")}</Field>
      <Field label="News action">{choice("news_action", [["block", "Block openings"], ["flatten", "Block and close positions once"]])}</Field>
      <Field label="Holding cutoff (ET)" hint="Before day end; defaults to 15:45">{text("hold_cutoff")}</Field>
      <div className="sm:col-span-2"><p className="text-sm">Forbid holding over</p><div className="flex flex-wrap gap-3">
        {(["weekend", "earnings", "ex_dividend", "split"] as HoldRestriction[]).map((kind) => <label key={kind} className="text-sm">
          <input type="checkbox" checked={form.hold_restrictions.includes(kind)} onChange={(e) => set("hold_restrictions")(e.target.checked
            ? [...form.hold_restrictions, kind] : form.hold_restrictions.filter((v) => v !== kind))} /> {kind.replaceAll("_", " ")}</label>)}
      </div></div>
      <div className="grid gap-2 sm:col-span-2" aria-label="Plan event calendar">
        <p className="text-sm">Event calendar · saved with this plan; imports do not update it automatically.</p>
        <div className="flex flex-wrap gap-3"><button type="button" onClick={() => void importCalendar()}>Import server calendar</button>
          <button type="button" onClick={() => void importDividends()}>Import known dividend ex-dates</button>
          <button type="button" disabled={form.events.length >= 256} onClick={() => set("events")([...form.events, { kind: "news", time: "" }])}>Add event</button></div>
        {importStatus && <p role="status" className="text-sm">{importStatus}</p>}
        {form.events.map((e, i) => <div key={i} className="grid gap-2 rounded border border-border p-2 sm:grid-cols-3">
          <Field label={`Event ${i + 1} kind`}><select className="trade-input" value={e.kind} onChange={(v) => {
            const kind = v.target.value as PlanEvent["kind"]
            const updated = { ...e, kind }; delete updated.session
            if (kind === "earnings") updated.session = "before_open"
            set("events")(form.events.map((old, n) => n === i ? updated : old))
          }}>{["news", "earnings", "ex_dividend", "split"].map((kind) => <option key={kind}>{kind}</option>)}</select></Field>
          <Field label={`Event ${i + 1} time`} hint={e.kind === "news" ? "UTC: YYYY-MM-DDTHH:MM:SSZ" : "YYYY-MM-DD"}>
            <input className="trade-input" value={e.time} onChange={(v) => changeEvent(i, { time: v.target.value })} /></Field>
          <Field label={`Event ${i + 1} underlying`} hint={e.kind === "news" ? "Blank for all underlyings" : "Required"}>
            <input className="trade-input" value={e.symbol ?? ""} onChange={(v) => changeEvent(i, { symbol: v.target.value })} /></Field>
          {e.kind === "earnings" && <Field label={`Event ${i + 1} session`}><select className="trade-input" value={e.session ?? "before_open"}
            onChange={(v) => changeEvent(i, { session: v.target.value as PlanEvent["session"] })}><option value="before_open">Before open</option><option value="after_close">After close</option></select></Field>}
          <Field label={`Event ${i + 1} label`}><input className="trade-input" maxLength={128} value={e.label ?? ""} onChange={(v) => changeEvent(i, { label: v.target.value })} /></Field>
          <button type="button" onClick={() => set("events")(form.events.filter((_, n) => n !== i))}>Remove event {i + 1}</button>
        </div>)}
      </div>
      <Field label="Strategies">{choice("strategies", [["buy_only", "Buy only, single leg"], ["defined_risk", "Defined risk"], ["any", "Any"]])}</Field>
      <div className="grid gap-3 border-t border-border pt-3 sm:col-span-2 sm:grid-cols-2" aria-label="Prop rules">
        <h3 className="text-sm font-medium sm:col-span-2">Prop rules <span className="text-[11px] font-normal text-faint">· all off unless set; illustrative rule types, not any firm's terms</span></h3>
        <Field label="Floor follows" hint="The closed balance leaves open losses out of the floor; pair it with an open-loss cap">{choice("drawdown_basis", [["equity", "Marked equity"], ["balance", "Closed balance"]])}</Field>
        <Field label="Open-loss cap" hint="One position's marked open loss">{choice("open_loss_basis", [["off", "Off"], ["amount", "Fixed amount"], ["day_profit", "% of start-of-day profit"], ["drawdown_room", "% of drawdown room"]])}</Field>
        {form.open_loss_basis !== "off" && <>
          <Field label={form.open_loss_basis === "amount" ? "Cap (dollars)" : "Cap (percent)"} hint={form.open_loss_basis === "day_profit" ? "Without profit at the day's open, the drawdown room stands in" : undefined}>{text("open_loss_value")}</Field>
          <Field label="Reaching it">{choice("open_loss_action", [["flatten", "Closes the position"], ["block_adds", "Blocks adding to it"], ["fail", "Fails the attempt"]])}</Field>
        </>}
        <Field label="Opening order at most % of the last minute's volume" hint="Per contract, at acceptance. Without a minute of reported volume the order is refused. Blank for none.">{text("minute_volume_percent", "none")}</Field>
        {form.phase !== "funded" && <Field label="One trade counts at most % of the target" hint="Profit above it is left out of the target. Blank for none.">{text("best_position_percent", "none")}</Field>}
        {form.phase === "funded" && <>
          <Field label="Payout review">{choice("review", [["no", "Off"], ["yes", "Review trades at each payout"]])}</Field>
          {form.review === "yes" && <>
            <Field label="Offending: held under (seconds)" hint="Blank for none">{text("review_min_hold_seconds", "none")}</Field>
            <Field label="Offending: one trade over % of cycle profit" hint="Blank for none">{text("review_max_trade_percent", "none")}</Field>
            <Field label="Offending: breached the open-loss cap">{choice("review_open_loss", [["no", "No"], ["yes", "Yes"]])}</Field>
            <Field label="Ladder" hint="Comma-separated: warning, forfeit, cap; the last repeats">{text("review_ladder")}</Field>
            <Field label="Cap step's payout maximum" hint="Needed when the ladder caps">{text("review_cap", "none")}</Field>
          </>}
          <Field label="Floor after a payout">{choice("after_payout", [["keep", "Stays as it is"], ["reset", "Account resets to a share of its start"], ["starting_balance", "Moves up to the starting balance"]])}</Field>
          {form.after_payout === "reset" && <Field label="Resets to % of the starting balance">{text("reset_percent")}</Field>}
        </>}
      </div>
    </fieldset>
  )
}
