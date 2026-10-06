import type { Account, EvaluationDay, Money, PayoutStatus, PayoutRules, Plan } from "../api/trading-types"
import { showFundedAccounts } from "./features"
import { sameProgram } from "./program-costs"
import { compareMoney, ruleMoney, formatMoney, sumMoney } from "./trading"

/** Verification is always offered; funded plans share the Payouts page's visibility. */
export function offeredPlans(plans: Plan[], account?: Pick<Account, "rules">, showFunded = showFundedAccounts): Plan[] {
  return payoutsVisible(account, showFunded) ? plans : plans.filter((p) => p.rules.phase !== "funded")
}
/** The Payouts page is shown with funded plans, or for an account that is already funded. */
export function payoutsVisible(account: Pick<Account, "rules"> | undefined, showFunded = showFundedAccounts): boolean {
  return showFunded || account?.rules.phase === "funded"
}

/** A payout number's cap: the last cap repeats; null when uncapped. */
export function payoutCap(caps: Money[], number: number): Money | null {
  return caps.length ? caps[Math.min(number, caps.length) - 1] ?? null : null
}

/** Verification and funded plans unlock once the current attempt has passed their prerequisite. */
export function lockReason(plan: Plan, plans: Plan[], account: Account | undefined): string | null {
  if (!plan.unlocked_by) return null
  const name = plans.find((p) => p.id === plan.unlocked_by)?.name ?? plan.unlocked_by
  if (account && sameProgram(plan, account)) return null
  const passed = account?.evaluation.status === "passed" && (account.next_plans
    ? account.next_plans.includes(plan.id)
    : account.rules.plan === name)
  return passed ? null : `Pass ${name} to unlock`
}

/** The next verification or funded step unlocked by the current pass. */
export function unlockedNextPlan(plans: Plan[], account: Account | undefined, showFunded = showFundedAccounts): Plan | null {
  return account?.evaluation.status === "passed"
    ? offeredPlans(plans, account, showFunded).find((p) => p.unlocked_by != null && !sameProgram(p, account) && lockReason(p, plans, account) == null) ?? null : null
}

/** Finished days of the current payout cycle: each day counts toward the cycle in progress when it
 * closes, so the trading day of the last payout request and later days. */
export function cycleDays(account: Account): EvaluationDay[] {
  const last = account.evaluation.payouts.at(-1)
  return last ? account.evaluation.days.filter((d) => d.day >= last.day) : account.evaluation.days
}

export interface PayoutCheck { label: string; ok: boolean; value?: string }
/** Every payout requirement with its current standing, in the server's order. */
export function payoutChecks(status: PayoutStatus): PayoutCheck[] {
  const checks: PayoutCheck[] = [
    { label: "Funded account is active", ok: status.active },
    { label: "No open positions or working orders", ok: status.flat },
    { label: `Qualifying days of ${ruleMoney(status.qualifying_profit)}+ net realised profit`,
      ok: status.qualifying_days >= status.required_days, value: `${status.qualifying_days} of ${status.required_days}` },
  ]
  if (status.consistency_percent != null) checks.push({ label: "Payout consistency",
    ok: (compareMoney(status.cycle_profit, "0") ?? 0) > 0 && compareMoney(status.consistency_needed, "0") === 0,
    value: `${payoutBestDayShare(status)} of ${status.consistency_percent}% max` })
  checks.push({ label: "Available payout meets the minimum",
    ok: (compareMoney(status.maximum, "0") ?? 0) > 0 && (compareMoney(status.maximum, status.minimum) ?? -1) >= 0,
    value: `${formatMoney(status.maximum)} of ${formatMoney(status.minimum)}` })
  return checks
}

/** A whole-cent amount within the accepted range, or why not. */
export function payoutAmountError(amount: string, status: PayoutStatus): string | null {
  if (!/^\d+(\.\d{1,2})?$/.test(amount.trim())) return "Enter an amount in dollars and cents"
  if ((compareMoney(amount.trim(), "0") ?? 0) <= 0) return "Enter an amount above zero"
  if ((compareMoney(amount.trim(), status.minimum) ?? -1) < 0) return `The minimum payout is ${formatMoney(status.minimum)}`
  if ((compareMoney(amount.trim(), status.maximum) ?? 1) > 0) return `The most you can request now is ${formatMoney(status.maximum)}`
  return null
}

/** Extra funded rules, shared by plan facts and the Rules page. */
export function payoutRuleFacts(rules: PayoutRules, startingBalance: Money): string[] {
  const percents = rules.consistency_percents ?? []
  return [
    ...(percents.length ? [`Payout consistency: best day at most ${percents.map((p) => `${p}%`).join(" / ")} of cycle net profit by payout number; last repeats`] : []),
    ...((compareMoney(rules.buffer ?? "0", "0") ?? 0) > 0 ? [`Payout buffer: keep equity at ${formatMoney(sumMoney([startingBalance, rules.buffer!]))} or more (${formatMoney(rules.buffer)} above start), ${rules.buffer_payouts ? `first ${rules.buffer_payouts} payouts` : "every payout"}`] : []),
  ]
}

export function payoutBestDayShare(status: PayoutStatus): string {
  return status.best_day && Number(status.cycle_profit) > 0
    ? `${(Number(status.best_day.profit) / Number(status.cycle_profit) * 100).toFixed(2)}%` : "—"
}
