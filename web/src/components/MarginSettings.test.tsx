import { describe, expect, it } from "vitest"
import { marginFacts, marginRequest, planMargin } from "./MarginSettings"

describe("margin settings", () => {
  it("sends only what differs from the plan", () => {
    const plan = planMargin({ margin: "strategy" })
    expect(marginRequest(plan, plan)).toEqual({})
    expect(marginRequest({ ...plan, account_type: "ira", margin: "portfolio", pm_vol_shock: 5 }, plan)).toEqual({ account_type: "ira" })
    expect(marginRequest({ ...plan, margin: "portfolio", pm_vol_shock: 5 }, plan)).toEqual({ margin: "portfolio", pm_vol_shock: 5 })
    // Back to the plan's portfolio margin from a cash choice sends strategy explicitly.
    const portfolio = planMargin({ margin: "portfolio", pm_vol_shock: 3 })
    expect(marginRequest({ ...portfolio, account_type: "cash" }, portfolio)).toEqual({ account_type: "cash", margin: "strategy" })
  })
  it("states the account's margin in plain facts", () => {
    expect(marginFacts({ margin: "strategy" })).toEqual([])
    expect(marginFacts({ account_type: "ira", margin: "strategy", house_margin_percent: 20 })).toEqual(["IRA (limited margin)", "20% house margin"])
    expect(marginFacts({ margin: "portfolio", pm_vol_shock: 5 })).toEqual(["Portfolio margin with ±5 vol points"])
  })
})

it("keeps financing and pin risk opt-in and carries their overrides", () => {
  const defaults = planMargin()
  expect(defaults.pin_risk).toBe(false)
  expect(defaults.cash_credit_rate_bps).toBe(0)
  expect(marginRequest({ ...defaults, pin_risk: true, pin_risk_seed: 42, cash_credit_rate_bps: 365,
    margin_debit_rate_bps: 1200, stock_borrow_rate_bps: 500 }, defaults)).toEqual({ pin_risk: true,
    pin_risk_seed: 42, cash_credit_rate_bps: 365, margin_debit_rate_bps: 1200, stock_borrow_rate_bps: 500 })
})
