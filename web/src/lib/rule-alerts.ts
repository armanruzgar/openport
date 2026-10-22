import type { Account, Risk } from "../api/trading-types"
import { formatMoney } from "./trading"

export interface RuleAlert { id: string; title: string; body: string }
export function ruleAlerts(account: Account, risk: Risk): RuleAlert[] {
  const result: RuleAlert[] = []
  const e = account.evaluation
  if (e.status === "failed")
    result.push({ id: `evaluation-failed-${e.attempt}-${e.decided_at ?? e.day}`, title: "Evaluation rule breached",
      body: e.decision ?? "The server marked this paper evaluation as failed." })
  const scaled = e.size_scaling?.history.at(-1)
  if (scaled?.day === e.day)
    result.push({ id: `account-scaled-${e.attempt}-${scaled.day}`, title: "Account size increased",
      body: `${formatMoney(scaled.old)} to ${formatMoney(scaled.size)}. A new review period has started; the capital credit is not profit.` })
  const drawdown = Number(account.rules.max_drawdown)
  const room = e.drawdown_buffer == null ? null : Number(e.drawdown_buffer)
  if (e.valuation_complete && room != null && Number.isFinite(room) && drawdown > 0) {
    for (const percent of [50, 25, 10]) if (room < drawdown * percent / 100)
      result.push({ id: `floor-${percent}`, title: `Floor room below ${percent}%`, body: `${formatMoney(e.drawdown_buffer)} remains above the drawdown floor.` })
  }
  const loss = Number(risk.daily_loss), limit = Number(risk.limits.max_daily_loss)
  if (e.valuation_complete && Number.isFinite(loss) && limit > 0) {
    for (const percent of [50, 75, 90]) if (loss >= limit * percent / 100)
      result.push({ id: `loss-${percent}`, title: `Daily loss reached ${percent}%`, body: `${formatMoney(risk.daily_loss)} of the ${formatMoney(risk.limits.max_daily_loss)} daily loss limit.` })
  }
  for (const reason of risk.guardrail_state?.latched ?? [])
    result.push({ id: `guardrail-${reason}`, title: `Guardrail: ${reason}`, body: reason === "COOLDOWN"
      ? `Opening orders pause for ${Math.ceil((risk.guardrail_state?.cooldown_seconds ?? 0) / 60)} market minutes. Closing orders and exits still work.`
      : "Reduce-only until the next trading day. Closing orders and exits still work." })
  if (risk.pending_applied_day === e.day)
    result.push({ id: "pending-applied", title: "Pending limits applied", body: "Your pending limits and guardrails are now in effect." })
  const target = Number(account.rules.profit_target), remaining = Number(e.target_remaining)
  if (e.status === "active" && e.valuation_complete && target > 0 && e.target_remaining != null && remaining > 0 && remaining <= target * 0.1)
    result.push({ id: "target-near", title: "Profit target within 10%", body: `${formatMoney(e.target_remaining)} remains to the profit target.` })
  // The plan's own daily loss limit, apart from the personal one above.
  const plan = e.daily_loss
  if (e.status === "active" && e.valuation_complete && plan && !e.day_lock) {
    const room = Number(plan.room), limit = Number(plan.limit)
    if (Number.isFinite(room) && limit > 0)
      for (const percent of [50, 25, 10]) if (room < limit * percent / 100)
        result.push({ id: `plan-loss-${percent}`, title: `Plan daily loss room below ${percent}%`,
          body: `${formatMoney(plan.room)} remains above the plan's daily loss limit at ${formatMoney(plan.level)}; reaching it ${plan.action === "fail" ? "fails the attempt" : "locks the day"}.` })
  }
  if (e.status === "active" && e.day_lock)
    result.push({ id: "day-lock", title: "Trading locked for the day", body: "The plan's daily loss limit closed your positions. Only closing orders are accepted until the next trading day." })
  // A target reached while other objectives, or open positions on a closed-balance plan, hold the pass back.
  const objectives = e.objectives ?? []
  const goal = objectives.find((o) => o.code === "PROFIT_TARGET")
  const waiting = objectives.filter((o) => !o.met)
  if (e.status === "active" && goal && goal.actual != null && goal.actual >= goal.required && waiting.length)
    result.push({ id: "target-waits", title: "Target reached: the pass waits", body: waiting.map((o) => o.message).join(". ") })
  return result
}

/**
 * Claim before notifying. Reloads and account switches share the same durable keys;
 * a reset starts a new attempt, whose thresholds alert again on the same day. An
 * account's claims for earlier days and attempts are dropped, so the list stays small.
 */
function claimOf(key: string): unknown[] {
  try { const value: unknown = JSON.parse(key); return Array.isArray(value) ? value : [] } catch { return [] }
}
export function createRuleAlertStore(storage: () => Pick<Storage, "getItem" | "setItem">) {
  const memory = new Set<string>()
  return {
    fresh(account: string, attempt: number, day: string, alerts: readonly RuleAlert[]) {
      let saved: string[] = []
      try {
        const value: unknown = JSON.parse(storage().getItem("openport.rule-alerts") ?? "[]")
        if (Array.isArray(value)) saved = value.filter((item): item is string => typeof item === "string")
      } catch { /* Storage can be denied; keep this page's deduplication. */ }
      saved.forEach((key) => memory.add(key))
      for (const key of memory) {
        const [owner, claimed, on] = claimOf(key)
        if (owner === account && (claimed !== attempt || on !== day)) memory.delete(key)
      }
      const fresh = alerts.filter((alert) => {
        const key = JSON.stringify([account, attempt, day, alert.id])
        if (memory.has(key)) return false
        memory.add(key)
        return true
      })
      try { storage().setItem("openport.rule-alerts", JSON.stringify([...memory])) } catch { /* Keep the in-memory claim. */ }
      return fresh
    },
  }
}
export const ruleAlertStore = createRuleAlertStore(() => window.localStorage)
