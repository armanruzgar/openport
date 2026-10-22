import type { AccountRules, OpenLossCap, PayoutRules, ReviewStep, TiltFlag } from "../api/trading-types"
import { compareMoney, ruleMoney, validMoney } from "./trading"

/** The prop rules' part of the custom plan form, as typed; choices as their API words. */
export interface PropRulesForm {
  drawdown_basis: "equity" | "balance"
  open_loss_basis: "off" | OpenLossCap["basis"]
  /** Dollars for the amount basis, a whole percent for the others. */
  open_loss_value: string
  open_loss_action: OpenLossCap["action"]
  minute_volume_percent: string
  best_position_percent: string
  review: "yes" | "no"
  review_min_hold_seconds: string
  review_max_trade_percent: string
  review_open_loss: "yes" | "no"
  /** Comma-separated steps: warning, forfeit, cap. */
  review_ladder: string
  review_cap: string
  after_payout: "keep" | "reset" | "starting_balance"
  reset_percent: string
}

export function propForm(r: AccountRules): PropRulesForm {
  const cap = r.open_loss_cap
  const review = r.payouts?.review
  return {
    drawdown_basis: r.drawdown_basis ?? "equity",
    open_loss_basis: cap?.basis ?? "off",
    open_loss_value: cap ? (cap.basis === "amount" ? cap.amount ?? "" : String(cap.percent)) : "",
    open_loss_action: cap?.action ?? "flatten",
    minute_volume_percent: r.minute_volume_percent ? String(r.minute_volume_percent) : "",
    best_position_percent: r.best_position_percent ? String(r.best_position_percent) : "",
    review: review ? "yes" : "no",
    review_min_hold_seconds: review?.min_hold_seconds ? String(review.min_hold_seconds) : "",
    review_max_trade_percent: review?.max_trade_percent ? String(review.max_trade_percent) : "",
    review_open_loss: review?.open_loss ? "yes" : "no",
    review_ladder: (review?.ladder ?? ["warning", "forfeit", "cap"]).join(", "),
    review_cap: review?.cap ?? "",
    after_payout: r.payouts?.after_payout ?? "keep",
    reset_percent: String(r.payouts?.reset_percent ?? 100),
  }
}

const whole = (text: string) => text.trim() === "" ? 0 : Number(text.trim())
const steps: ReviewStep[] = ["warning", "forfeit", "cap"]

/** The rules the prop fields set, checked as the server checks them; or the first problem. */
export function propRules(form: PropRulesForm, phase: AccountRules["phase"], target: string | null, drawdown: string | null,
                          drawdownMode: AccountRules["drawdown_mode"]):
    { rules: Pick<AccountRules, "drawdown_basis" | "open_loss_cap" | "minute_volume_percent" | "best_position_percent">;
      payouts: Pick<PayoutRules, "review" | "after_payout" | "reset_percent"> } | { error: string } {
  let open_loss_cap: OpenLossCap | null = null
  if (form.open_loss_basis !== "off") {
    const value = form.open_loss_value.trim()
    if (form.open_loss_basis === "amount") {
      if (!validMoney(value) || compareMoney(value, "0") !== 1) return { error: "The open-loss cap must be a positive dollar amount" }
      open_loss_cap = { basis: "amount", amount: value, percent: 0, action: form.open_loss_action }
    } else {
      const percent = whole(value)
      if (!Number.isInteger(percent) || percent < 1 || percent > 100) return { error: "The open-loss cap percentage must be a whole number from 1 to 100" }
      if (form.open_loss_basis === "drawdown_room" && !drawdown) return { error: "An open-loss cap on the drawdown room needs a drawdown floor" }
      open_loss_cap = { basis: form.open_loss_basis, amount: null, percent, action: form.open_loss_action }
    }
  }
  const minute = whole(form.minute_volume_percent)
  if (!Number.isInteger(minute) || minute < 0 || minute > 1000) return { error: "The minute-volume cap must be a whole percent from 0 to 1000" }
  const best = phase === "funded" ? 0 : whole(form.best_position_percent)
  if (!Number.isInteger(best) || best < 0 || best > 100) return { error: "The best-position cap must be a whole percent from 0 to 100" }
  if (best > 0 && !target) return { error: "The best-position cap needs a profit target" }
  let review: PayoutRules["review"] = null
  if (phase === "funded" && form.review === "yes") {
    const hold = whole(form.review_min_hold_seconds), share = whole(form.review_max_trade_percent)
    if (!Number.isInteger(hold) || hold < 0 || hold > 3600) return { error: "Review hold seconds must be a whole number from 0 to 3600" }
    if (!Number.isInteger(share) || share < 0 || share > 100) return { error: "The reviewed trade share must be a whole percent from 0 to 100" }
    if (!hold && !share && form.review_open_loss !== "yes") return { error: "A payout review needs at least one offence to look for" }
    if (form.review_open_loss === "yes" && !open_loss_cap) return { error: "Reviewing open-loss breaches needs an open-loss cap" }
    const ladder = form.review_ladder.split(",").map((s) => s.trim()).filter(Boolean)
    if (!ladder.length || ladder.length > 16 || ladder.some((s) => !steps.includes(s as ReviewStep)))
      return { error: "The review ladder is 1 to 16 comma-separated steps: warning, forfeit or cap" }
    const capped = ladder.includes("cap")
    const cap = form.review_cap.trim()
    if (capped && (!validMoney(cap) || compareMoney(cap, "0") !== 1)) return { error: "A ladder with a cap step needs a positive payout cap" }
    review = { min_hold_seconds: hold, max_trade_percent: share, open_loss: form.review_open_loss === "yes",
      ladder: ladder as ReviewStep[], cap: capped ? cap : null }
  }
  const after = phase === "funded" ? form.after_payout : "keep"
  const reset = after === "reset" ? whole(form.reset_percent) : 100
  if (!Number.isInteger(reset) || reset < 1 || reset > 100) return { error: "The reset balance must be 1 to 100% of the starting balance" }
  if (after !== "keep" && !drawdown) return { error: "Moving the floor after a payout needs a drawdown floor" }
  if (after === "starting_balance" && drawdownMode === "static") return { error: "A static floor cannot move to the starting balance" }
  return {
    rules: { drawdown_basis: form.drawdown_basis, open_loss_cap, minute_volume_percent: minute, best_position_percent: best },
    payouts: { review, after_payout: after, reset_percent: reset },
  }
}

export function openLossFact(cap: OpenLossCap | null | undefined): string | null {
  if (!cap) return null
  const measure = cap.basis === "amount" ? ruleMoney(cap.amount ?? "0")
    : `${cap.percent}% of ${cap.basis === "day_profit" ? "the start-of-day profit balance (the drawdown room without profit)" : "the drawdown room at the day's open"}`
  const action = cap.action === "flatten" ? "the account closes the position" : cap.action === "block_adds" ? "adding to it is refused until it recovers" : "the attempt fails"
  return `Open loss of one position at most ${measure}: ${action}`
}

/** The prop rules in force, in a few words each. */
export function propRuleFacts(r: AccountRules): string[] {
  const facts: string[] = []
  if (r.drawdown_basis === "balance" && r.max_drawdown) facts.push("The floor follows the closed balance; open losses do not touch it")
  const open = openLossFact(r.open_loss_cap)
  if (open) facts.push(open)
  if (r.minute_volume_percent) facts.push(`Opening orders at most ${r.minute_volume_percent}% of the last minute's volume per contract`)
  if (r.phase !== "funded" && r.best_position_percent && r.profit_target)
    facts.push(`At most ${r.best_position_percent}% of the target from one trade counts toward it`)
  facts.push(...reviewFacts(r.payouts))
  return facts
}

export function reviewFacts(p: PayoutRules | null | undefined): string[] {
  if (!p) return []
  const facts: string[] = []
  if (p.review) {
    const offences = [p.review.min_hold_seconds ? `held under ${p.review.min_hold_seconds}s` : "",
      p.review.max_trade_percent ? `over ${p.review.max_trade_percent}% of the cycle's profit` : "",
      p.review.open_loss ? "past the open-loss cap" : ""].filter(Boolean)
    facts.push(`Payout review of trades ${offences.join(", ")}: ${p.review.ladder.join(", then ")}${p.review.cap ? ` (cap ${ruleMoney(p.review.cap)})` : ""}`)
  }
  if (p.after_payout === "reset") facts.push(`After a payout the account resets to ${p.reset_percent ?? 100}% of the starting balance`)
  if (p.after_payout === "starting_balance") facts.push("After a payout the floor moves up to the starting balance")
  return facts
}

export function tiltText(flag: TiltFlag): string {
  return `${flag.code === "LOSS_STREAK" ? "Losing streak" : "Size creep"}: ${flag.message}`
}
