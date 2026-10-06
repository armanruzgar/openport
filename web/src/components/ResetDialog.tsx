import { useRef, useState } from "react"
import { api } from "../api/client"
import { useAccount, useRefreshTrading, usePlans, useRisk, useTradingSession } from "../api/trading"
import type { FeeModel, FillModel, Plan, TradingStatus } from "../api/trading-types"
import { FillModelPicker } from "./FillModelPicker"
import { FeeModelPicker } from "./FeeModelPicker"
import { lockReason, offeredPlans, payoutRuleFacts } from "../lib/payouts"
import { resetQuote } from "../lib/program-costs"
import { phaseFact, sizeScalingFact, scalingFact, dailyLossFact, dayEndFact, drawdownFact, objectiveFacts, targetFact, timeRuleFacts, tradeRuleFacts } from "../lib/plan-rules"
import { compareMoney, ruleMoney, formatMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { customPlan, PlanEditor, planForm, type PlanForm } from "./PlanEditor"
import { marginFacts, marginRequest, MarginSettings, planMargin, type MarginChoice } from "./MarginSettings"
import { TradingError, WriteAccess, writeBlocked } from "./TradingControls"
import { Badge } from "./ui"

export function planFacts(plan: Pick<Plan, "initial_cash" | "rules">): string[] {
  const r = plan.rules
  const p = r.payouts
  const dailyLoss = dailyLossFact(r), dayEnd = dayEndFact(r)
  return [
    phaseFact(r),
    `${ruleMoney(plan.initial_cash)} starting balance`,
    ...(([r.evaluation_fee, r.reset_fee, r.activation_fee].some((fee) => compareMoney(fee, "0") === 1) || r.max_resets) ? [
      `Purchase ${formatMoney(r.evaluation_fee ?? "0")}, reset ${formatMoney(r.reset_fee ?? "0")}, activation ${formatMoney(r.activation_fee ?? "0")}`,
      r.max_resets ? `${r.max_resets} resets per plan` : "Unlimited resets",
    ] : []),
    targetFact(r),
    drawdownFact(r, plan.initial_cash),
    ...(dailyLoss ? [dailyLoss] : []),
    ...objectiveFacts(r),
    ...tradeRuleFacts(r),
    ...timeRuleFacts(r),
    ...(sizeScalingFact(r) ? [sizeScalingFact(r)!] : []),
    ...(scalingFact(r) ? [scalingFact(r)!] : []),
    r.buy_only ? "Buy-only, single leg" : r.defined_risk ? "Defined risk only" : "Any strategy",
    ...(r.buying_power ? ["Buying power enforced"] : []),
    ...marginFacts(r),
    ...(r.slippage_ticks ? [`${r.slippage_ticks} ${r.slippage_ticks === 1 ? "tick" : "ticks"} of slippage`] : []),
    ...(r.fill_latency_ms ? [`${r.fill_latency_ms} ms fill latency on market time`] : []),
    ...(r.inside_fill_percent ? [`Inside fills at ${r.inside_fill_percent}% across the spread`] : []),
    ...(r.impact_ticks ? [`${r.impact_ticks} extra ${r.impact_ticks === 1 ? "tick" : "ticks"} per displayed-size block`] : []),
    ...(r.expiry_cutoff_seconds > 0 ? [`Auto-close ${Math.round(r.expiry_cutoff_seconds / 60)} min before expiry`] : []),
    ...(dayEnd ? [dayEnd] : []),
    ...(p ? [`Payout every ${p.qualifying_days} days of ${ruleMoney(p.qualifying_profit)}+ net profit`,
      `Up to ${p.withdrawal_percent}% of profit per payout, ${p.split_percent}% to you`, ...payoutRuleFacts(p, plan.initial_cash)] : []),
  ]
}

/** The choice that opens the custom plan editor. */
const CUSTOM = "custom"

/** Starting an attempt closes every position at its last mark; confirm with a plan choice. */
export function ResetDialog({ trading, attempt, initial, onClose }: { trading: TradingStatus; attempt: number; initial?: string; onClose: () => void }) {
  const plans = usePlans()
  const account = useAccount().data
  const risk = useRisk()
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const [choice, setChoice] = useState(initial ?? "")
  const [fillModel, setFillModel] = useState<FillModel>("as_displayed")
  const [feeModel, setFeeModel] = useState<FeeModel>("flat")
  const [margin, setMargin] = useState<MarginChoice>()
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<unknown>()
  const busy = useRef(false)
  const list = offeredPlans(plans.data?.plans ?? [], account)
  const selected = list.find((p) => p.id === choice && lockReason(p, list, account) == null) ?? null
  // Custom rules can use either phase without taking the preset's name.
  const bases = list
  const [baseId, setBaseId] = useState("")
  const [form, setForm] = useState<PlanForm | null>(null)
  const base = bases.find((p) => p.id === baseId) ?? bases.find((p) => p.rules.profit_target) ?? bases[0] ?? null
  const custom = choice === CUSTOM && base && form ? customPlan(form, base.rules) : null
  const terms = custom && !("error" in custom) ? custom : selected
  const quote = account && terms ? resetQuote(terms, account, selected?.id) : null
  const ready = !!risk.data && !!account && (custom ? !("error" in custom) : selected != null) && !quote?.blocked
  const balance = custom && !("error" in custom) ? custom.initial_cash : selected?.initial_cash
  const softFloor = (risk.data?.pending_guardrails ?? risk.data?.guardrails)?.soft_floor
  const floorWarning = compareMoney(softFloor, "0") === 1 && balance != null &&
    (compareMoney(balance, softFloor) === -1 || compareMoney(balance, softFloor) === 0)
  const planDefaults = planMargin(custom && !("error" in custom) ? custom.rules : selected?.rules)
  function chooseCustom() {
    setChoice(CUSTOM)
    if (form || !base) return
    const start = bases.find((p) => p.id === choice) ?? base
    setBaseId(start.id)
    setForm(planForm(start))
  }
  function startFrom(id: string) {
    const plan = bases.find((p) => p.id === id)
    if (!plan) return
    setBaseId(id)
    setForm(planForm(plan))
  }
  async function submit() {
    if (!ready || busy.current || writeBlocked(trading, token)) return
    busy.current = true
    setPending(true)
    setError(undefined)
    try {
      const fill = {
        ...(fillModel !== "as_displayed" ? { fill_model: fillModel } : {}),
        ...(feeModel === "itemized" ? { fee_model: feeModel } : {}),
      }
      const marginSettings = marginRequest(margin ?? planDefaults, planDefaults)
      if (custom && !("error" in custom)) await api.resetAccount({ ...custom, reason: `Start ${custom.rules.plan}`, ...fill, ...marginSettings }, trading.write)
      else if (selected) await api.resetAccount({ plan: selected.id, reason: `Start ${selected.name}`, ...fill, ...marginSettings }, trading.write)
      if (sameSession()) onClose()
    } catch (failure) {
      if (sameSession()) setError(failure)
    } finally {
      busy.current = false
      if (sameSession()) setPending(false)
      void refresh()
    }
  }
  const groups = [
    { title: "Evaluations", plans: list.filter((p) => p.rules.phase === "evaluation") },
    { title: "Verification", plans: list.filter((p) => p.rules.phase === "verification") },
    { title: "Funded accounts", plans: list.filter((p) => p.rules.phase === "funded") },
  ].filter((g) => g.plans.length)
  return (
    <Dialog title="Start a new attempt" onClose={onClose}>
      <p className="text-sm text-muted">
        Attempt {attempt + 1} starts fresh: working orders are cancelled, open positions close at their last mark,
        and cash returns to the plan's starting balance. Your trade history is kept.
        Today's profit lock, trade limit, opening-order count and active cooldown carry over.
        Manual and daily-loss latches clear. The soft floor is checked against the new balance.
      </p>
      <WriteAccess trading={trading} />
      <TradingError error={plans.error} />
      <TradingError error={risk.error} />
      <form className="space-y-3" onSubmit={(event) => { event.preventDefault(); void submit() }}>
        {groups.map((group) => (
          <fieldset key={group.title} className="space-y-2" disabled={pending}>
            <legend className="mb-1 text-[11px] font-medium uppercase tracking-wide text-muted">{group.title}</legend>
            {group.plans.map((plan) => {
              const locked = lockReason(plan, list, account)
              return (
                <label key={plan.id} className={`block rounded-md border p-3 ${locked ? "cursor-not-allowed opacity-60" : "cursor-pointer"} ${
                  choice === plan.id && !locked ? "border-accent bg-accent/5" : "border-border hover:border-muted"}`}>
                  <div className="flex items-start gap-2">
                    <input type="radio" name="plan" value={plan.id} checked={choice === plan.id && !locked} disabled={locked != null}
                      onChange={() => setChoice(plan.id)} className="mt-1 accent-[var(--accent)]" />
                    <div className="min-w-0">
                      <div className="flex flex-wrap items-center gap-2 text-sm font-medium">
                        {plan.name}
                        {plan.unlocked_by && (locked ? <Badge>locked</Badge> : <Badge tone="positive">unlocked</Badge>)}
                      </div>
                      <div className="mt-0.5 text-xs text-muted">{locked ?? plan.summary}</div>
                      <div className="mt-1 text-[11px] text-faint">{planFacts(plan).join(" · ")}</div>
                    </div>
                  </div>
                </label>
              )
            })}
          </fieldset>
        ))}
        {base && <div className="space-y-2">
          <label className={`block cursor-pointer rounded-md border p-3 ${choice === CUSTOM ? "border-accent bg-accent/5" : "border-border hover:border-muted"}`}>
            <div className="flex items-start gap-2">
              <input type="radio" name="plan" value={CUSTOM} checked={choice === CUSTOM} disabled={pending} onChange={chooseCustom}
                className="mt-1 accent-[var(--accent)]" />
              <div className="min-w-0">
                <div className="text-sm font-medium">Custom plan</div>
                <div className="mt-0.5 text-xs text-muted">Start from a preset and set your own target, floor, daily loss limit,
                  objectives to pass and trading day, as a prop firm's plan has them.</div>
              </div>
            </div>
          </label>
          {choice === CUSTOM && form && <>
            <label className="block space-y-1 text-xs">
              <span className="text-muted">Start from</span>
              <select className="trade-input w-full" value={base.id} disabled={pending} onChange={(event) => startFrom(event.target.value)}>
                {bases.map((p) => <option key={p.id} value={p.id}>{p.name}</option>)}
              </select>
            </label>
            <PlanEditor form={form} onChange={setForm} disabled={pending} />
            {custom && "error" in custom
              ? <p role="status" className="text-xs text-warn">{custom.error}</p>
              : custom && <p className="text-[11px] text-faint">{planFacts(custom).join(" · ")}</p>}
          </>}
        </div>}
        {quote && <div role="status" className="rounded-md border border-border p-3 text-sm">
          <p>This start charges {formatMoney(quote.fee)} ({quote.kind} fee). Paper cash and rule outcomes are unaffected.</p>
          <p>{quote.left == null ? "Unlimited resets" : `${quote.left} resets left after this start`}</p>
          {quote.blocked && <p className="text-warn">Reset limit reached. Choose a different plan to start a new purchase.</p>}
        </div>}
        <FillModelPicker value={fillModel} onChange={setFillModel} disabled={pending} />
        <FeeModelPicker value={feeModel} onChange={setFeeModel} disabled={pending} flat={trading.fee_per_contract} />
        <MarginSettings value={margin ?? planDefaults} onChange={setMargin} disabled={pending} />
        {plans.isLoading && <p className="text-sm text-muted">Loading plans…</p>}
        {floorWarning && <p role="alert" className="text-sm text-warn">
          Your absolute soft floor of {formatMoney(softFloor)} is at or above this plan's starting balance of {formatMoney(balance)}.
          The soft floor will latch at once and refuse opening orders. Lower it in personal guardrails before resetting;
          a lower setting is pending until rollover, and a reset applies pending settings.
        </p>}
        {risk.isLoading && <p className="text-sm text-muted">Checking personal guardrails…</p>}
        <TradingError error={error} />
        <button type="submit" className="trade-button" disabled={!ready || pending || writeBlocked(trading, token)}>
          {pending ? "Starting…" : custom && !("error" in custom) ? `Start ${custom.rules.plan}` : selected && choice !== CUSTOM ? `Start ${selected.name}` : "Choose a plan"}
        </button>
      </form>
    </Dialog>
  )
}
