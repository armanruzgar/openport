import { describe, expect, it } from "vitest"
import { account, risk } from "../test/trading-fixtures"
import { createRuleAlertStore, ruleAlerts } from "./rule-alerts"
const shown = (room: string, loss: string, remaining = "1000") => ruleAlerts({ ...account,
  rules: { ...account.rules, max_drawdown: "1000", profit_target: "1000" },
  evaluation: { ...account.evaluation, marked: true, valuation_complete: true, drawdown_buffer: room, target_remaining: remaining },
}, { ...risk, daily_loss: loss, limits: { ...risk.limits, max_daily_loss: "100" } }).map((alert) => alert.id)
describe("rule alerts", () => {
  it("fires exact daily loss thresholds and strictly-below room thresholds", () => {
    expect(shown("500", "49")).toEqual([])
    expect(shown("499", "50")).toEqual(["floor-50", "loss-50"])
    expect(shown("249", "75")).toEqual(["floor-50", "floor-25", "loss-50", "loss-75"])
    expect(shown("99", "90", "100")).toEqual(["floor-50", "floor-25", "floor-10", "loss-50", "loss-75", "loss-90", "target-near"])
  })
  it("announces latches and pending activation from polled state", () => {
    const alerts = ruleAlerts({ ...account, evaluation: { ...account.evaluation, day: "2026-09-22" } }, { ...risk,
      guardrail_state: { opening_trades: 3, latched: ["TRADE_LIMIT", "COOLDOWN"], cooldown_until: null, cooldown_seconds: 90, soft_floor: null },
      pending_applied_day: "2026-09-22",
    })
    expect(alerts.map((alert) => alert.id)).toEqual(expect.arrayContaining(["guardrail-TRADE_LIMIT", "guardrail-COOLDOWN", "pending-applied"]))
    expect(alerts.find((a) => a.id === "guardrail-COOLDOWN")?.body).toContain("2 market minutes")
  })
  it("claims once per account and trading day across reloads", () => {
    let text: string | null = null
    const storage = () => ({ getItem: () => text, setItem: (_key: string, value: string) => { text = value } })
    const first = createRuleAlertStore(storage)
    const alerts = [{ id: "floor-50", title: "Floor", body: "room" }]
    expect(first.fresh("main", 1, "2026-09-22", alerts)).toHaveLength(1)
    expect(first.fresh("main", 1, "2026-09-22", alerts)).toEqual([])
    const reloaded = createRuleAlertStore(storage)
    expect(reloaded.fresh("main", 1, "2026-09-22", alerts)).toEqual([])
    expect(reloaded.fresh("second", 1, "2026-09-22", alerts)).toHaveLength(1)
    expect(reloaded.fresh("main", 1, "2026-09-23", alerts)).toHaveLength(1)
    expect(reloaded.fresh("main", 1, "2026-09-23", alerts)).toEqual([])
  })
  it("drops an account's claims from earlier days and attempts", () => {
    let text: string | null = JSON.stringify(["{", JSON.stringify(["second", 1, "2026-09-20", "floor-50"])])
    const store = createRuleAlertStore(() => ({ getItem: () => text, setItem: (_key: string, value: string) => { text = value } }))
    const alerts = [{ id: "floor-50", title: "Floor", body: "room" }]
    store.fresh("main", 1, "2026-09-22", alerts)
    store.fresh("main", 1, "2026-09-23", alerts)
    store.fresh("main", 2, "2026-09-23", alerts)
    expect(JSON.parse(text!)).toEqual(["{", JSON.stringify(["second", 1, "2026-09-20", "floor-50"]), JSON.stringify(["main", 2, "2026-09-23", "floor-50"])])
  })
  it("alerts again for a new attempt started on the same day", () => {
    const store = createRuleAlertStore(() => { throw new Error("denied") })
    const alerts = [{ id: "floor-25", title: "Floor", body: "room" }]
    expect(store.fresh("main", 1, "2026-09-22", alerts)).toHaveLength(1)
    expect(store.fresh("main", 2, "2026-09-22", alerts)).toHaveLength(1)
    expect(store.fresh("main", 2, "2026-09-22", alerts)).toEqual([])
  })
  it("survives denied and corrupt storage with in-memory deduplication", () => {
    const alerts = [{ id: "profit", title: "Target", body: "near" }]
    const store = createRuleAlertStore(() => { throw new Error("denied") })
    expect(store.fresh("main", 1, "today", alerts)).toHaveLength(1)
    expect(store.fresh("main", 1, "today", alerts)).toEqual([])
    const corrupt = createRuleAlertStore(() => ({ getItem: () => "{", setItem: () => {} }))
    expect(corrupt.fresh("main", 1, "today", alerts)).toHaveLength(1)
  })
  it("does not infer numbers from missing marks or disabled limits", () => {
    const alerts = ruleAlerts({ ...account, rules: { ...account.rules, max_drawdown: null, profit_target: null }, evaluation: { ...account.evaluation, marked: false, valuation_complete: false } }, { ...risk, limits: { ...risk.limits, max_daily_loss: "0" } })
    expect(alerts).toEqual([])
    expect(ruleAlerts({ ...account, rules: { ...account.rules, max_drawdown: "1000", profit_target: "1000" },
      evaluation: { ...account.evaluation, marked: true, valuation_complete: false, drawdown_buffer: "1", target_remaining: "1" },
    }, { ...risk, daily_loss: "10000" })).toEqual([])
  })
  it("stays quiet about the target once the attempt is decided", () => {
    // B37: a passed attempt has nothing left to go, even when it sold below the target.
    for (const remaining of ["0.00", "1.30"])
      expect(ruleAlerts({ ...account, rules: { ...account.rules, profit_target: "1000" },
        evaluation: { ...account.evaluation, status: "passed", marked: true, valuation_complete: true, target_remaining: remaining },
      }, risk).map((alert) => alert.id)).not.toContain("target-near")
  })
})

it("records the server's evaluation failure without inferring a breach from prices", () => {
  const alerts = ruleAlerts({ ...account, evaluation: { ...account.evaluation, status: "failed", decision: "Maximum drawdown reached", decided_at: "2026-10-16T15:30:00Z" } }, risk)
  expect(alerts).toContainEqual(expect.objectContaining({ title: "Evaluation rule breached", body: "Maximum drawdown reached" }))
  expect(ruleAlerts(account, risk).some((a) => a.title === "Evaluation rule breached")).toBe(false)
})
