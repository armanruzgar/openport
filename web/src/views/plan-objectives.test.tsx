// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, type ReactNode } from "react"
import { createRoot, type Root } from "react-dom/client"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { Account, Plan } from "../api/trading-types"
import { PlanEditor, planForm, type PlanForm } from "../components/PlanEditor"
import { ResetDialog, planFacts } from "../components/ResetDialog"
import { evaluationBadge } from "../components/Sidebar"
import { ruleAlerts } from "../lib/rule-alerts"
import { account, plans, portfolio, risk, status, trades, twoStepPlans } from "../test/trading-fixtures"
import { DashboardView } from "./DashboardView"
import { SizeScalingProgress } from "./PayoutsView"
import { RulesView, ruleText } from "./RulesView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const features = vi.hoisted(() => ({ showFundedAccounts: false }))
vi.mock("../lib/features", () => features)

/** A static floor on the closed balance, with a plan daily loss limit, minimum days and consistency. */
const planned: Account = {
  ...account,
  rules: { ...account.rules, plan: "Custom plan", buy_only: false, drawdown_mode: "static", lock_at_start: false, profit_basis: "balance",
    daily_loss_limit: "2000.00", daily_loss_basis: "balance", daily_loss_action: "lock", consistency_percent: 40, consistency_basis: "total",
    min_trading_days: 3, min_profitable_days: 0, profitable_day_profit: null, day_end: "18:00" },
  evaluation: { ...account.evaluation, floor: "95000.00", floor_locked: true, drawdown_buffer: "5267.50", balance: "100150.00", profit_basis: "balance",
    target_remaining: "9850.00", decision_code: null,
    objectives: [
      { code: "PROFIT_TARGET", met: false, actual: 150, required: 10000, message: "$9850.00 to go to the target $110000.00 on the closed balance" },
      { code: "MIN_TRADING_DAYS", met: false, actual: 2, required: 3, message: "2 of 3 days with a trade; a day counts once one of your own orders, or a share trade, executes on it" },
      { code: "MICROSCALPING", met: false, actual: 60, required: 25, message: "Net positive profit $90.00 from round trips held under 30 seconds" },
      { code: "MIN_TRADES", met: false, actual: 2, required: 10, message: "2 of 10 closed whole trades" },
      { code: "TRADE_CONSISTENCY", met: false, actual: 75, required: 40, message: "Best trade 17, net $112.50; at most 40% of attempt profit $150.00" },
      { code: "CONSISTENCY", met: false, actual: 66.7, required: 40, message: "The best day, $100.00 on 2026-09-22, is 67% of the total profit $150.00" },
    ],
    trading_days: 2, profitable_days: 1, best_day: { day: "2026-09-22", profit: "100.00" }, consistency_target: "250.00",
    daily_loss: { limit: "2000.00", basis: "balance", action: "lock", reference: "100150.00", level: "98150.00", room: "2117.50" },
    day_lock: null, day_locked_at: null, exit_equity: "100200.00", exit_cost: "67.50", liquidated_equity: null, liquidation_cost: null },
}
const locked: Account = { ...planned, evaluation: { ...planned.evaluation, day_lock: "DAILY_LOSS_LIMIT", day_locked_at: "2026-09-23T15:10:00Z",
  daily_loss: { ...planned.evaluation.daily_loss!, room: "0.00" } } }

const clients: QueryClient[] = []
function client(value: Account) {
  const c = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(c)
  const queries = tradingQueries(0, "17", true)
  c.setQueryData(queries.account.queryKey, value)
  c.setQueryData(queries.portfolio.queryKey, portfolio)
  c.setQueryData(queries.risk.queryKey, risk)
  c.setQueryData(queries.trades("current").queryKey, { account_version: "17", attempt: 2, trades })
  c.setQueryData(["plans"], { plans })
  return c
}
const render = (node: ReactNode, value: Account) => renderToStaticMarkup(<QueryClientProvider client={client(value)}>{node}</QueryClientProvider>)
let host: HTMLDivElement
let root: Root
beforeEach(() => {
  features.showFundedAccounts = false
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
})
afterEach(async () => {
  if (root) await act(async () => root.unmount())
  host?.remove()
  clients.splice(0).forEach((c) => c.clear())
  vi.restoreAllMocks(); vi.clearAllMocks(); vi.unstubAllGlobals()
})

describe("plan objectives in the terminal", () => {
  it("shows mandatory flat notices, plan facts and no-overnight rules", () => {
    const value: Account = { ...planned, time: "2026-09-23T19:30:00Z",
      rules: { ...planned.rules, flat_time: "15:45", no_overnight: true },
      evaluation: { ...planned.evaluation, flat_time: "15:45", flat_now: false } }
    expect(render(<DashboardView />, value)).toContain("Flat by 15:45 ET, positions will be closed")
    expect(render(<DashboardView />, { ...value, time: "2026-09-23T19:45:00Z",
      evaluation: { ...value.evaluation, flat_now: true } })).toContain("Flat time passed; openings blocked until the day ends")
    const texts = ruleText(value).map((r) => renderToStaticMarkup(<>{r.body}</>)).join(" ")
    expect(texts).toContain("Flat by 15:45 ET")
    expect(texts).toContain("OVERNIGHT_HOLD")
    expect(planFacts({ initial_cash: "100000", rules: value.rules }).join(" ")).toContain("positions awaiting settlement are excluded")
  })
  it("shows evaluation and inactivity deadlines and configured entry restrictions", () => {
    const timed: Account = { ...planned, rules: { ...planned.rules, time_limit_days: 30, inactivity_days: 14,
      underlyings: ["SPX", "XSP"], trading_start: "09:30", trading_end: "16:00",
      scaling: [{ profit: "0.00", contracts: 2 }] }, evaluation: { ...planned.evaluation,
      days_left: 5, deadline: "2026-10-22", inactive_days: 10, inactivity_deadline: "2026-10-21" } }
    const html = render(<DashboardView />, timed)
    expect(html).toContain("5 calendar days left; deadline 2026-10-22")
    expect(html).toContain("4 calendar days left to execute a trade; deadline 2026-10-21")
    const texts = ruleText(timed).map((r) => renderToStaticMarkup(<>{r.body}</>)).join(" ")
    expect(texts).toContain("30 calendar days")
    expect(texts).toContain("Allowed underlyings: SPX, XSP")
    expect(texts).toContain("09:30–16:00 ET")
    expect(texts).toContain("SCALING_LIMIT")
    expect(texts).not.toContain("There is no time limit")
    expect(planFacts({ initial_cash: "100000", rules: timed.rules })).toContain("Evaluation ends after 30 calendar days")
  })
  it("lists what a pass waits for, the daily loss room and what closing now would leave", () => {
    const html = render(<DashboardView />, planned)
    for (const text of ["Objectives to pass", "0 of 6 met", "Microscalping", "60% of 25% max", "Closed trades", "2 of 10", "Trade consistency", "75% of 40% max", "Best trade 17", "Trading days", "2 of 3", "Consistency", "67% of 40% max",
      "Best day $100.00 on 2026-09-22", "needs $250.00 of profit", "Daily loss limit", "$98,150.00", "$2,117.50 room · locks the day",
      "to go on the closed balance", "Closed balance", "$100,150.00", "Equity if every position closed now", "$100,200.00", "· static"])
      expect(html).toContain(text)
    expect(html).not.toContain("Trading locked")
    // The default account shows none of it.
    const plain = render(<DashboardView />, account)
    for (const text of ["Objectives to pass", "Daily loss limit", "Closed balance", "if every position closed"]) expect(plain).not.toContain(text)
  })
  it("announces a locked day and a decision's code and liquidation cost", () => {
    const html = render(<DashboardView />, locked)
    expect(html).toContain("Trading locked until the next trading day")
    expect(html).toContain("6:00 pm ET")
    expect(html).toContain("Reached · locked until the next trading day")
    expect(renderToStaticMarkup(<>{evaluationBadge(locked)}</>)).toContain("locked today")
    const failed: Account = { ...planned, evaluation: { ...planned.evaluation, status: "failed", decision_code: "DAILY_LOSS_LIMIT",
      decided_at: "2026-09-23T15:10:00Z", decided_equity: "98100.00", decision: "Equity $98100.00 reached the daily loss limit",
      liquidated_equity: "98010.00", liquidation_cost: "90.00" },
      attempts: [{ ...account.attempts[0]!, status: "failed", decision_code: "DRAWDOWN_FLOOR" }] }
    const decided = render(<DashboardView />, failed)
    for (const text of ["Evaluation failed", "by the daily loss limit", "left $98,010.00, $90.00 less", "failed · drawdown floor"])
      expect(decided).toContain(text)
  })
  it("explains the plan's rules and offers its presets' objectives", () => {
    const texts = ruleText(planned, "0.65", "5000.00")
    const titles = texts.map((t) => t.title)
    expect(titles.slice(0, 5)).toEqual(["Account type and margin", "Profit target", "Static drawdown", "Plan daily loss limit", "Objectives to pass"])
    const bodies = texts.slice(1).map((t) => renderToStaticMarkup(<>{t.body}</>))
    expect(bodies[0]).toContain("on the closed balance")
    expect(bodies[0]).toContain("The pass also waits for: at least 3 trading days; best day at most 40% of the total profit")
    expect(bodies[1]).toContain("It is static")
    expect(bodies[2]).toContain("$98,150.00")
    expect(bodies[2]).toContain("refuses opening orders until the next trading day")
    expect(bodies[3]).toContain("never fails the attempt")
    expect(bodies.join(" ")).toContain("A trading day ends at 6:00 pm ET")
    const presets: Plan[] = [...plans, { id: "static-50k", name: "Static 50K", summary: "Static floor.", initial_cash: "50000.00", unlocked_by: null,
      rules: { ...planned.rules, plan: "Static 50K", profit_target: "5000.00", max_drawdown: "4000.00", daily_loss_action: "fail" } }]
    const c = client(planned)
    c.setQueryData(["plans"], { plans: presets })
    const html = renderToStaticMarkup(<QueryClientProvider client={c}><RulesView /></QueryClientProvider>)
    for (const text of ["Never (static)", "$2,000 · fails", "Target, closed · At least 3 trading days · Best day at most 40% of the total profit"])
      expect(html).toContain(text)
    expect(planFacts(presets.at(-1)!)).toEqual(expect.arrayContaining(["$5,000 profit target on the closed balance",
      "$4,000 static drawdown: the floor stays at $46,000", "Trading day ends at 6:00 pm ET"]))
  })
  it.each([
    { room: "-53.90", equity: "99696.10", reached: "which equity has reached ($53.90 below it)" },
    { room: "0.00", equity: "99750.00", reached: "which equity has reached" },
  ])("explains a locked day's daily loss level with $room room", ({ room, equity, reached }) => {
    const value: Account = { ...locked,
      rules: { ...locked.rules, daily_loss_limit: "250.00", daily_loss_basis: "peak" },
      evaluation: { ...locked.evaluation, equity,
        daily_loss: { limit: "250.00", basis: "peak", action: "lock", reference: "100000.00", level: "99750.00", room } } }
    const page = document.createElement("div")
    page.innerHTML = render(<RulesView />, value)
    const text = page.textContent!.replace(/\s+/g, " ")
    expect(text).toContain(`Each trading day, equity may not touch $250 below the day's equity high: today $99,750.00, ${reached}.`)
    expect(text).toContain("Today is locked.")
    expect(text).not.toMatch(/−\$[\d,.]+ below current equity/)
    expect(text).not.toContain("below current equity")
  })
  it("keeps the daily loss level below current equity when room is positive", () => {
    const page = document.createElement("div")
    page.innerHTML = render(<RulesView />, planned)
    const text = page.textContent!.replace(/\s+/g, " ")
    expect(text).toContain("Each trading day, equity may not touch $2,000 below the day's opening balance: today $98,150.00, $2,117.50 below current equity.")
    expect(text).not.toContain("which equity has reached")
  })
  it("lowercases each objective after the first in the objectives sentence", () => {
    const value: Account = { ...planned, rules: { ...planned.rules, min_profitable_days: 2, profitable_day_profit: "100.00" } }
    const html = render(<RulesView />, value)
    expect(html).toContain("At least 3 trading days; at least 2 profitable days of $100+; best day at most 40% of the total profit.")
  })
  it("alerts on the plan's daily loss room, a locked day and a target waiting on objectives", () => {
    const near: Account = { ...planned, evaluation: { ...planned.evaluation, daily_loss: { ...planned.evaluation.daily_loss!, room: "400.00" } } }
    expect(ruleAlerts(near, risk).map((a) => a.id)).toEqual(expect.arrayContaining(["plan-loss-50", "plan-loss-25"]))
    expect(ruleAlerts(near, risk).map((a) => a.id)).not.toContain("plan-loss-10")
    expect(ruleAlerts(locked, risk).map((a) => a.id)).toContain("day-lock")
    expect(ruleAlerts(locked, risk).map((a) => a.id)).not.toContain("plan-loss-50")
    const reached: Account = { ...planned, evaluation: { ...planned.evaluation, objectives: [
      { code: "PROFIT_TARGET", met: false, actual: 10100, required: 10000, message: "The closed balance reached the target; close every position to count it" },
      { code: "MIN_TRADING_DAYS", met: true, actual: 3, required: 3, message: "3 of 3 days" }] } }
    const waits = ruleAlerts(reached, risk).find((a) => a.id === "target-waits")
    expect(waits?.body).toContain("close every position to count it")
  })
  it("submits a custom funded plan from the editor with consistency and buffer", async () => {
    Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
    Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
    vi.spyOn(api, "resetAccount").mockResolvedValue(account)
    host = document.createElement("div"); document.body.append(host); root = createRoot(host)
    await act(async () => root.render(<QueryClientProvider client={client(account)}>
      <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} onClose={() => {}} />
    </QueryClientProvider>))
    await act(async () => host.querySelector<HTMLInputElement>('input[value="custom"]')!.click())
    const set = async (name: string, value: string) => {
      const element = [...host.querySelectorAll("label")].find((l) => l.textContent?.startsWith(name))?.querySelector("input, select")
      if (!(element instanceof HTMLInputElement || element instanceof HTMLSelectElement)) throw new Error(`Missing field ${name}`)
      await act(async () => {
        Object.getOwnPropertyDescriptor(element instanceof HTMLInputElement ? HTMLInputElement.prototype : HTMLSelectElement.prototype, "value")!.set!.call(element, value)
        element.dispatchEvent(new Event(element instanceof HTMLInputElement ? "input" : "change", { bubbles: true }))
      })
    }
    await set("Phase", "funded")
    await set("Payout consistency percentages", "20, 25, 30")
    await set("Payout buffer", "2100")
    await set("Buffer payouts", "3")
    expect(host.textContent).toContain("20% / 25% / 30%")
    expect(host.textContent).toContain("first 3 payouts")
    await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
    expect(api.resetAccount).toHaveBeenCalledWith(expect.objectContaining({ rules: expect.objectContaining({ phase: "funded", profit_target: null,
      payouts: expect.objectContaining({ consistency_percents: [20, 25, 30], buffer: "2100", buffer_payouts: 3 }) }) }), "open")
  })
  it("starts a custom plan from a preset with the edited rules", async () => {
    Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
    Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
    vi.spyOn(api, "resetAccount").mockResolvedValue(account)
    host = document.createElement("div"); document.body.append(host); root = createRoot(host)
    await act(async () => root.render(<QueryClientProvider client={client(account)}>
      <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} initial="intraday-100k" onClose={() => {}} />
    </QueryClientProvider>))
    const radio = host.querySelector<HTMLInputElement>('input[value="custom"]')!
    await act(async () => radio.click())
    const set = async (name: string, value: string) => {
      const element = [...host.querySelectorAll("label")].find((l) => l.textContent?.startsWith(name))?.querySelector("input, select")
      if (!(element instanceof HTMLInputElement || element instanceof HTMLSelectElement)) throw new Error(`Missing field ${name}`)
      await act(async () => {
        Object.getOwnPropertyDescriptor(element instanceof HTMLInputElement ? HTMLInputElement.prototype : HTMLSelectElement.prototype, "value")!.set!.call(element, value)
        element.dispatchEvent(new Event(element instanceof HTMLInputElement ? "input" : "change", { bubbles: true }))
      })
    }
    await set("Plan name", "Two-step 100K")
    await set("Floor", "static")
    await set("Daily loss limit", "3000")
    await set("Reaching it", "fail")
    await set("Minimum trading days", "4")
    await set("Trading day ends", "18:00")
    expect(host.textContent).toContain("$3,000 daily loss limit from the day's opening equity: fails the attempt")
    await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
    expect(api.resetAccount).toHaveBeenCalledWith(expect.objectContaining({ initial_cash: "100000.00", reason: "Start Two-step 100K",
      rules: expect.objectContaining({ plan: "Two-step 100K", drawdown_mode: "static", daily_loss_limit: "3000", daily_loss_action: "fail",
        min_trading_days: 4, day_end: "18:00", profit_target: "10000.00", buy_only: true, payouts: null }) }), "open")
    // A bad value is named, and nothing is sent.
    vi.mocked(api.resetAccount).mockClear()
    await set("Minimum trading days", "x")
    expect(host.textContent).toContain("Minimum trading days must be a whole number from 0 to 366")
    await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
    expect(api.resetAccount).not.toHaveBeenCalled()
  })
})

it("shows enabled trade-entry rules in plan facts and the Rules page", () => {
  const value = { ...planned, rules: { ...planned.rules, max_contracts_held: 5, require_stop_loss: true, max_trade_risk: "250.00", max_trade_risk_percent: 10 } }
  const facts = planFacts({ initial_cash: "10000", rules: value.rules })
  expect(facts).toEqual(expect.arrayContaining(["At most 5 option contracts held or opening", "Stop-loss required on every entry", "Trade risk at most $250.00 before fees", "Trade risk at most 10% of room to the plan floor"]))
  const texts = ruleText(value, "0.65", "5000.00")
  expect(texts.map((t) => t.title)).toEqual(expect.arrayContaining(["Contracts held at once", "Stop-loss required", "Maximum trade risk"]))
  expect(ruleText(account, "0.65", "5000.00").map((t) => t.title)).not.toContain("Maximum trade risk")
  })

it("shows the scaling limit in force and the next session's threshold", () => {
  const scaled: Account = { ...planned, rules: { ...planned.rules, scaling: [{ profit: "0.00", contracts: 2 }, { profit: "1500.00", contracts: 3 }] },
    evaluation: { ...planned.evaluation, scaling: { limit: 2, held: 1, profit: "1600.00", next: { profit: "1500.00", contracts: 3 } } } }
  const dashboard = render(<DashboardView />, scaled)
  for (const text of ["Scaling plan", "1 held / 2 contract limit", "Next: 3 contracts", "$1,500.00 profit or more; applies next session", "Closed-balance profit $1,600.00"])
    expect(dashboard).toContain(text)
  const rules = render(<RulesView />, scaled)
  for (const text of ["Scaling plan", "SCALING_LIMIT", "shares do not", "reducing orders remain allowed"]) expect(rules).toContain(text)
  expect(planFacts({ initial_cash: "50000", rules: scaled.rules }).join(" ")).toContain("2 contracts at $0.00 profit; 3 contracts at $1,500.00 profit")
  expect(render(<DashboardView />, account)).not.toContain("Scaling plan")
})

it("adds, edits and removes custom scaling steps", async () => {
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  let form: PlanForm = planForm({ initial_cash: "50000", rules: planned.rules })
  const draw = () => root.render(<PlanEditor form={form} onChange={(next) => { form = next; draw() }} />)
  await act(async () => draw())
  const click = async (label: string) => act(async () => {
    const button = [...host.querySelectorAll("button")].find((b) => b.textContent === label || b.getAttribute("aria-label") === label)!
    button.click()
  })
  await click("Add scaling step")
  expect(form.scaling).toEqual([{ profit: "0", contracts: "2" }])
  await click("Add scaling step")
  const input = [...host.querySelectorAll("label")].find((l) => l.textContent?.startsWith("Step 2 profit"))!.querySelector("input")!
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(input, "1500")
    input.dispatchEvent(new Event("input", { bubbles: true }))
  })
  expect(form.scaling[1]?.profit).toBe("1500")
  await click("Remove scaling step 2")
  await click("Remove scaling step 1")
  expect(form.scaling).toEqual([])
})

it("renders size reviews, history, plan facts and the existing rule notice", () => {
  const size_scaling = { size: "62500.00", original: "50000.00", original_max_drawdown: "5000.00", original_daily_loss_limit: "2000.00", max_balance: "100000.00", period_started: "2026-09-24",
    period_days: 3, days_required: 80, period_profit: "1200.50", profit_required: "6250.00", period_payouts: 1, payouts_required: 2,
    next_size: "75000.00", history: [{ day: "2026-09-24", old: "50000.00", size: "62500.00" }] }
  const scaled: Account = { ...planned, rules: { ...planned.rules, phase: "funded", size_scaling: {
    profit_percent: 10, payouts: 2, days: 80, increase_percent: 25, max_balance: "100000.00" },
    time_limit_days: 0, inactivity_days: 14, underlyings: ["SPX"], trading_start: "09:30", trading_end: "16:00",
    flat_time: "15:45", no_overnight: true,
    evaluation_fee: "100.000001", reset_fee: "25.000002", activation_fee: "50.000003", max_resets: 2,
    scaling: [{ profit: "0.00", contracts: 2 }] },
    evaluation: { ...planned.evaluation, day: "2026-09-24", size_scaling } }
  const text = renderToStaticMarkup(<SizeScalingProgress status={size_scaling} />)
  for (const value of ["Account size scaling", "$62,500.00", "$75,000.00", "3 / 80", "$1,200.50 / $6,250.00", "1 / 2", "2026-09-24", "Account size history"])
    expect(text).toContain(value)
  expect(planFacts({ initial_cash: "50000", rules: scaled.rules }).join(" ")).toContain("Add 25% of the original size")
  expect(render(<RulesView />, scaled)).toContain("Account size scaling")
  const facts = planFacts({ initial_cash: "50000", rules: scaled.rules }).join(" ")
  const allRules = render(<RulesView />, scaled)
  for (const text of ["Scaling plan", "Inactivity limit", "Allowed underlyings", "Opening hours", "Mandatory flat time", "No overnight holds"])
    expect(allRules).toContain(text)
  for (const text of ["Scaling", "Inactivity limit", "Allowed underlyings", "Opening hours", "Flat by 15:45", "OVERNIGHT_HOLD"])
    expect(facts).toContain(text)
  for (const text of ["Purchase $100.00", "reset $25.00", "activation $50.00", "2 resets per plan"])
    expect(facts).toContain(text)
  expect(ruleAlerts(scaled, risk).map((a) => a.title)).toContain("Account size increased")
  expect(ruleAlerts({ ...scaled, evaluation: { ...scaled.evaluation, day: "2026-09-25" } }, risk).map((a) => a.title)).not.toContain("Account size increased")
})

it("enables and edits account size scaling only in the funded form", async () => {
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  let form = planForm({ initial_cash: "50000", rules: planned.rules })
  const draw = () => root.render(<PlanEditor form={form} onChange={(next) => { form = next; draw() }} />)
  await act(async () => draw())
  expect(host.textContent).not.toContain("Account size scaling")
  form = { ...form, phase: "funded" }
  await act(async () => draw())
  const checkbox = host.querySelector<HTMLInputElement>('input[type="checkbox"]')!
  await act(async () => checkbox.click())
  expect(form.size_scaling_enabled).toBe("yes")
  const input = [...host.querySelectorAll("label")].find((l) => l.textContent?.startsWith("Maximum account size"))!.querySelector("input")!
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(input, "200000")
    input.dispatchEvent(new Event("input", { bubbles: true }))
  })
  expect(form.size_max_balance).toBe("200000")
  await act(async () => checkbox.click())
  expect(form.size_scaling_enabled).toBe("no")
  expect(host.textContent).not.toContain("Review profit percent")
})

it("shows trade objectives and protective hold exceptions on Rules and the editor", () => {
  const value: Account = { ...planned, rules: { ...planned.rules, min_trades: 10, trade_consistency_percent: 40,
    min_hold_seconds: 60, microscalp_seconds: 30, microscalp_percent: 25 } }
  const html = render(<RulesView />, value)
  for (const text of ["at least 10 closed whole trades", "best closed whole trade at most 40%", "Minimum hold time",
    "60 seconds of market time", "OCO", "system exits still execute", "losses do not offset it"])
    expect(html).toContain(text)
  const editor = renderToStaticMarkup(<PlanEditor form={planForm({ initial_cash: "100000", rules: value.rules })} onChange={() => {}} />)
  for (const label of ["Minimum closed trades", "Trade consistency: best trade at most %", "Minimum hold (seconds)",
    "Microscalp threshold (seconds)", "Microscalp profit at most %"])
    expect(editor).toContain(label)
})

it("explains direction bans and fill rechecks in Rules and the editor", () => {
  const value: Account = { ...planned, rules: { ...planned.rules, no_hedging: true, no_counter_positions: true } }
  const html = render(<RulesView />, value)
  for (const text of ["No hedging", "No counter positions", "Only the ordering account", "Pure reductions", "sandbox", "working opening orders separately", "COUNTER_POSITION at fill"])
    expect(html).toContain(text)
  const editor = renderToStaticMarkup(<PlanEditor form={planForm({ initial_cash: "100000", rules: value.rules })} onChange={() => {}} />)
  expect(editor).toContain("No hedging")
  expect(editor).toContain("No counter positions")
})

it("explains the strict volume gate and exposes its editor field", () => {
  const value: Account = { ...planned, rules: { ...planned.rules, max_volume_percent: 25 } }
  const html = render(<RulesView />, value)
  for (const text of ["Maximum share of option volume", "25%", "Unknown or stale volume refuses", "Scenario backtests support this rule with generated opening volume", "Recording days need current-date option volume", "HTTP 400", "cancel with MAX_VOLUME_SHARE"])
    expect(html).toContain(text)
  const editor = renderToStaticMarkup(<PlanEditor form={planForm({ initial_cash: "100000", rules: value.rules })} onChange={() => {}} />)
  expect(editor).toContain("Maximum volume %")
  expect(editor).toContain('value="25"')
})

it.each([false, true])("shows program costs and phase history and opens verification confirmation after a pass (funded=%s)", async (showFunded) => {
  features.showFundedAccounts = showFunded
  vi.stubGlobal("ResizeObserver", class { observe() {} disconnect() {} })
  const challenge: Account = { ...account, rules: { ...account.rules, plan: "Two-step Challenge 100K", plan_id: "two-step-100k" },
    next_plans: ["two-step-verify-100k"], evaluation: { ...account.evaluation, status: "passed" },
    costs: { evaluation: "400", reset: "200", activation: "0", total: "600", resets_used: 1, resets_left: 1,
      payouts_received: "0", net: "-600", fee_charged: "200", fee_kind: "reset" },
    attempts: [{ ...account.attempts[0]!, rules: { ...account.rules, phase: "verification" }, fee_charged: "200.00", fee_kind: "reset" }] }
  const verification: Plan = { ...plans[1]!, id: "two-step-verify-100k", name: "Two-step Verification 100K", unlocked_by: "two-step-100k",
    rules: { ...account.rules, phase: "verification", activation_fee: "0", profit_target: "5000.00" } }
  const c = client(challenge); c.setQueryData(["plans"], { plans: [...plans, verification] })
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  vi.spyOn(api, "resetAccount").mockResolvedValue(challenge)
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  await act(async () => root.render(<QueryClientProvider client={c}><DashboardView /></QueryClientProvider>))
  expect(host.textContent).toContain("Step 1 of 2: challenge")
  expect(host.textContent).toContain("Step 2 of 2: verification")
  expect(host.textContent).toContain("Program costs so far: $600.00")
  expect(host.textContent).toContain("net after payouts −$600.00")
  await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Start verification")!.click())
  expect(host.querySelector<HTMLDialogElement>("dialog")!.open).toBe(true)
  expect(host.querySelector<HTMLInputElement>('input[value="two-step-verify-100k"]')!.checked).toBe(true)
  expect(api.resetAccount).not.toHaveBeenCalled()
})

it.each([false, true])("gates the Dashboard's funded start after a verification pass (funded=%s)", async (showFunded) => {
  features.showFundedAccounts = showFunded
  vi.stubGlobal("ResizeObserver", class { observe() {} disconnect() {} })
  const [, verification, funded] = twoStepPlans
  const passed: Account = { ...account, rules: verification.rules, next_plans: [funded.id],
    evaluation: { ...account.evaluation, status: "passed" } }
  const c = client(passed); c.setQueryData(["plans"], { plans: twoStepPlans })
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  vi.spyOn(api, "resetAccount").mockResolvedValue(passed)
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  await act(async () => root.render(<QueryClientProvider client={c}><DashboardView /></QueryClientProvider>))
  expect(host.textContent).toContain("Verification passed")
  const start = [...host.querySelectorAll("button")].find((b) => b.textContent === "Start funded account")
  if (showFunded) {
    expect(start).toBeDefined()
    await act(async () => start!.click())
    expect(host.querySelector<HTMLDialogElement>("dialog")!.open).toBe(true)
    expect(host.querySelector<HTMLInputElement>(`input[value="${funded.id}"]`)!.checked).toBe(true)
    expect(host.textContent).toContain("This start charges $200.00 (activation fee)")
  } else {
    expect(start).toBeUndefined()
    expect(host.textContent).not.toContain(funded.name)
    expect(host.textContent).not.toContain("activation $200.00")
  }
  expect(api.resetAccount).not.toHaveBeenCalled()
})
