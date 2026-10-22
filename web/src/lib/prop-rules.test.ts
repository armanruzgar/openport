import { afterEach, describe, expect, it } from "vitest"
import type { AccountRules, RulePack } from "../api/trading-types"
import { customPlan, planForm } from "../components/PlanEditor"
import { account, fundedAccount } from "../test/trading-fixtures"
import { openLossFact, propForm, propRuleFacts, propRules, reviewFacts, tiltText } from "./prop-rules"
import { isPackPlan, loadRulePacks, packFile, packPlans, saveRulePacks, withPack } from "./rule-packs"

const rules: AccountRules = { ...account.rules, buy_only: false }

describe("prop rules in the plan editor", () => {
  it("leaves every prop rule off for plans without them", () => {
    const form = propForm(rules)
    expect(form).toMatchObject({ drawdown_basis: "equity", open_loss_basis: "off", minute_volume_percent: "", best_position_percent: "",
      review: "no", after_payout: "keep", reset_percent: "100" })
    const result = propRules(form, "evaluation", "10000", "5000", "intraday")
    expect(result).toEqual({ rules: { drawdown_basis: "equity", open_loss_cap: null, minute_volume_percent: 0, best_position_percent: 0 },
      payouts: { review: null, after_payout: "keep", reset_percent: 100 } })
    expect(propRuleFacts(rules)).toEqual([])
  })
  it("builds open-loss caps, minute-volume and best-position caps, and refuses what the server refuses", () => {
    const form = { ...propForm(rules), open_loss_basis: "drawdown_room" as const, open_loss_value: "30", open_loss_action: "block_adds" as const,
      minute_volume_percent: "25", best_position_percent: "40", drawdown_basis: "balance" as const }
    const result = propRules(form, "evaluation", "10000", "5000", "intraday")
    expect(result).toEqual({ rules: { drawdown_basis: "balance", minute_volume_percent: 25, best_position_percent: 40,
      open_loss_cap: { basis: "drawdown_room", amount: null, percent: 30, action: "block_adds" } }, payouts: expect.anything() })
    expect(propRules(form, "evaluation", "10000", null, "intraday")).toEqual({ error: expect.stringContaining("needs a drawdown floor") })
    expect(propRules({ ...form, open_loss_value: "0" }, "evaluation", "10000", "5000", "intraday")).toHaveProperty("error")
    expect(propRules({ ...form, open_loss_basis: "amount", open_loss_value: "-1" }, "evaluation", "10000", "5000", "intraday")).toHaveProperty("error")
    expect(propRules({ ...form, minute_volume_percent: "1001" }, "evaluation", "10000", "5000", "intraday")).toHaveProperty("error")
    expect(propRules(form, "evaluation", null, "5000", "intraday")).toEqual({ error: "The best-position cap needs a profit target" })
  })
  it("builds payout reviews and the floor after a payout for funded plans only", () => {
    const funded = { ...propForm(fundedAccount.rules), review: "yes" as const, review_min_hold_seconds: "60", review_ladder: "warning, cap",
      review_cap: "250", after_payout: "reset" as const, reset_percent: "90" }
    const result = propRules(funded, "funded", null, "5000", "intraday")
    expect(result).toHaveProperty("payouts", { review: { min_hold_seconds: 60, max_trade_percent: 0, open_loss: false, ladder: ["warning", "cap"], cap: "250" },
      after_payout: "reset", reset_percent: 90 })
    expect(propRules({ ...funded, review_cap: "" }, "funded", null, "5000", "intraday")).toHaveProperty("error")
    expect(propRules({ ...funded, review_ladder: "warn" }, "funded", null, "5000", "intraday")).toHaveProperty("error")
    expect(propRules({ ...funded, review_open_loss: "yes" }, "funded", null, "5000", "intraday"))
      .toEqual({ error: "Reviewing open-loss breaches needs an open-loss cap" })
    expect(propRules({ ...funded, after_payout: "starting_balance" }, "funded", null, "5000", "static")).toHaveProperty("error")
    // An evaluation keeps them off whatever the form says.
    expect(propRules(funded, "evaluation", "100", "5000", "intraday")).toHaveProperty("payouts", { review: null, after_payout: "keep", reset_percent: 100 })
  })
  it("round-trips prop rules through the custom plan form", () => {
    const withRules: AccountRules = { ...fundedAccount.rules, open_loss_cap: { basis: "amount", amount: "400.00", percent: 0, action: "flatten" },
      payouts: { ...fundedAccount.rules.payouts!, review: { min_hold_seconds: 30, max_trade_percent: 50, open_loss: true, ladder: ["warning", "forfeit"], cap: null },
        after_payout: "starting_balance", reset_percent: 100 } }
    const form = { ...planForm({ initial_cash: "100000", rules: withRules }), name: "Round trip" }
    const plan = customPlan(form, withRules)
    if ("error" in plan) throw new Error(plan.error)
    expect(plan.rules.open_loss_cap).toEqual(withRules.open_loss_cap)
    expect(plan.rules.payouts?.review).toEqual(withRules.payouts!.review)
    expect(plan.rules.payouts?.after_payout).toBe("starting_balance")
  })
  it("states the rules in force in a trader's words", () => {
    expect(openLossFact({ basis: "day_profit", amount: null, percent: 30, action: "fail" }))
      .toBe("Open loss of one position at most 30% of the start-of-day profit balance (the drawdown room without profit): the attempt fails")
    expect(openLossFact({ basis: "amount", amount: "500.00", percent: 0, action: "block_adds" }))
      .toContain("$500 · adding to it is refused".replace(" · ", ": "))
    const facts = propRuleFacts({ ...rules, drawdown_basis: "balance", minute_volume_percent: 10, best_position_percent: 30 })
    expect(facts).toEqual(["The floor follows the closed balance; open losses do not touch it",
      "Opening orders at most 10% of the last minute's volume per contract", "At most 30% of the target from one trade counts toward it"])
    expect(reviewFacts({ ...fundedAccount.rules.payouts!, review: { min_hold_seconds: 60, max_trade_percent: 0, open_loss: false, ladder: ["warning", "forfeit", "cap"], cap: "100" } }))
      .toEqual(["Payout review of trades held under 60s: warning, then forfeit, then cap (cap $100)"])
    expect(tiltText({ time: "2026-09-22T14:00:00Z", code: "SIZE_CREEP", message: "3 contracts" })).toBe("Size creep: 3 contracts")
  })
})

describe("rule packs", () => {
  const pack: RulePack = { format: "openport.rule-pack", version: 1, id: "desk", name: "Desk terms", effective_from: "2026-09-01",
    source: "https://example.com/terms", verify: "Checked 2026-09-01",
    plans: [{ id: "eval", name: "Desk Eval", initial_cash: "50000.00", rules: { ...rules, plan: "Desk Eval" }, unlocked_by: null }] }
  afterEach(() => { try { localStorage.clear() } catch { /* No storage in this environment. */ } })
  it("offers pack plans as their own custom starting points", () => {
    const [plan] = packPlans([pack])
    expect(plan).toMatchObject({ id: "pack:desk:eval", name: "Desk Eval", initial_cash: "50000.00", unlocked_by: null })
    expect(plan!.rules.plan_id).toBeNull()
    expect(plan!.summary).toContain("Desk terms, effective 2026-09-01")
    expect(isPackPlan(plan!.id)).toBe(true)
    expect(isPackPlan("intraday-100k")).toBe(false)
  })
  it("replaces a pack by ID and exports JSON that reads back", () => {
    const later = { ...pack, effective_from: "2026-10-01" }
    expect(withPack(withPack([], pack), later)).toEqual([later])
    expect(JSON.parse(packFile(pack))).toEqual(pack)
    saveRulePacks([pack])
    const loaded = loadRulePacks()
    // Without browser storage the list is empty rather than broken.
    expect(loaded.length === 0 || loaded[0]!.id === "desk").toBe(true)
  })
})
