import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { Account } from "../api/trading-types"
import { ResetDialog, planFacts } from "../components/ResetDialog"
import { account, fundedAccount, plans, portfolio, risk, status, trades } from "../test/trading-fixtures"
import { DashboardView } from "./DashboardView"
import { PayoutsView } from "./PayoutsView"
import { RulesView, ruleText } from "./RulesView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
// The terminal hides funded plans by default; these tests cover the kept code with them shown.
vi.mock("../lib/features", () => ({ showFundedAccounts: true }))
const clients: QueryClient[] = []
function render(node: ReactNode, value: Account) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(client)
  const queries = tradingQueries(0, "17", true)
  client.setQueryData(queries.account.queryKey, value)
  client.setQueryData(queries.portfolio.queryKey, portfolio)
  client.setQueryData(queries.risk.queryKey, risk)
  client.setQueryData(queries.trades("current").queryKey, { account_version: "17", attempt: value.evaluation.attempt, trades })
  client.setQueryData(["plans"], { plans })
  return renderToStaticMarkup(<QueryClientProvider client={client}>{node}</QueryClientProvider>)
}
const passed: Account = { ...account, evaluation: { ...account.evaluation, status: "passed", decided_at: "2026-09-23T15:00:00Z",
  decided_equity: "110000.00", decision: "Equity $110000.00 reached the profit target $110000.00" } }
const trading = { ...status.trading!, enabled: true, write: "open" as const }
beforeEach(() => vi.mocked(useLive).mockReturnValue(liveState(status, null, "open")))
afterEach(() => { clients.splice(0).forEach((client) => client.clear()); vi.clearAllMocks() })

describe("funded accounts and payouts", () => {
  it("shows a funded account's payout standing, cycle and history", () => {
    const html = render(<PayoutsView />, fundedAccount)
    for (const text of [">funded<", "Request payout #2", "Not enough qualifying days in this payout cycle", "Qualifying days", "3 / 8",
      "+$6,050.00", "50% withdrawable: $3,025.00", "$1,600.00", "1 payout · $2,000.00 withdrawn", "3 of 8", ">Tue, Sep 22</td>",
      "$3,000.00 of $1,000.00", "Wed, Sep 23", "Thu, Sep 24", "+$300.00", "+$250.00", "Payout history", "Your share (80%)", "$107,500.00",
      "Cap this payout", "Locked at $100,000.00", "Not eligible yet"])
      expect(html).toContain(text)
    expect(html).toContain("payout cycle since Tue, Sep 22, 2026, 14:00 ET")
    expect(html).not.toContain("NaN")
    expect(html).toMatch(/<button type="submit" class="trade-button" disabled="">Request payout/)
  })
  it("shows consistency standing and the active buffer with facts on the Rules page", () => {
    const custom: Account = { ...fundedAccount,
      rules: { ...fundedAccount.rules, payouts: { ...fundedAccount.rules.payouts!, consistency_percents: [20, 25, 30], buffer: "2100.00", buffer_payouts: 3 } },
      payout: { ...fundedAccount.payout!, consistency_percent: 25, cycle_profit: "500.00", best_day: { day: "2026-09-24", profit: "200.00" },
        consistency_needed: "300.00", buffer_balance: "102100.00" } }
    const html = render(<PayoutsView />, custom)
    for (const text of ["Cycle net realised profit", "+$500.00", "$200.00 on 2026-09-24", "40.00% · limit 25%",
      "Profit still needed", "$300.00", "Payout buffer balance", "$102,100.00 must remain after withdrawal"])
      expect(html).toContain(text)
    const facts = planFacts({ initial_cash: "100000.00", rules: custom.rules })
    expect(facts.join(" ")).toContain("20% / 25% / 30%")
    expect(facts.join(" ")).toContain("$102,100.00 or more ($2,100.00 above start), first 3 payouts")
    const bodies = ruleText(custom, "0.65").map((r) => renderToStaticMarkup(<>{r.body}</>)).join(" ")
    for (const text of ["20% / 25% / 30%", "first 3 payouts", "Consistency includes losses and the day in progress"])
      expect(bodies).toContain(text)
    const lifted = render(<PayoutsView />, { ...custom, payout: { ...custom.payout!, buffer_balance: null } })
    expect(lifted).not.toContain("Payout buffer balance")
    expect(render(<PayoutsView />, fundedAccount)).not.toContain("Profit still needed")
  })
  it("prefills the maximum and previews the trader's share when eligible", () => {
    const eligible: Account = { ...fundedAccount, payout: { ...fundedAccount.payout!, eligible: true, blocked: null, qualifying_days: 8 } }
    const html = render(<PayoutsView />, eligible)
    expect(html).toContain('value="3000.00"')
    expect(html).toContain("Between $1,000.00 and $3,000.00. You receive $2,400.00 (80%).")
    expect(html).toContain("$2,400.00 to you (80%)")
    expect(html).toMatch(/<button type="submit" class="trade-button">Request payout/)
  })
  it("explains the path to a payout before the funded phase and offers an unlocked plan", () => {
    const html = render(<PayoutsView />, account)
    for (const text of ["Payouts are available on funded accounts", "You are on Intraday 100K (active)", "How payouts work", "Funded plans",
      "Funded Intraday 100K", "8 × $200+", "Pass Intraday 100K to unlock", "$2,000 / $3,000 / $4,000 / $6,000"])
      expect(html).toContain(text)
    const unlocked = render(<PayoutsView />, passed)
    expect(unlocked).toContain("Funded account unlocked")
    expect(unlocked).toContain("Start Funded Intraday 100K")
  })
  it("adds payout progress to the dashboard and the unlock to a passed evaluation", () => {
    const html = render(<DashboardView />, fundedAccount)
    for (const text of ["Next payout", "3 / 8 days", "Qualifying days · $200+ net each", "Qualifying days this cycle", "Next payout, once eligible", "up to $3,000.00",
      "Funded: no profit target", "locks at $100,000", "Payout every 8 days of $200+ net profit", "Up to 50% of profit per payout, 80% to you",
      "Drawdown floor (locked)", "buffer · locked"])
      expect(html).toContain(text)
    expect(html).not.toContain("Remaining to target")
    const closed = render(<DashboardView />, { ...fundedAccount, evaluation: { ...fundedAccount.evaluation, status: "failed" } })
    expect(closed).toContain("Funded account closed")
    const pass = render(<DashboardView />, passed)
    expect(pass).toContain("Evaluation passed")
    expect(pass).toContain("Start funded account")
  })
  it("preserves cents in starting balances and payout rule amounts", () => {
    const custom: Account = { ...fundedAccount, rules: { ...fundedAccount.rules,
      payouts: { ...fundedAccount.rules.payouts!, qualifying_profit: "200.25", caps: ["2000.50"] } } }
    const facts = planFacts({ initial_cash: "50000.50", rules: custom.rules })
    expect(facts).toContain("$50,000.50 starting balance")
    expect(facts).toContain("Payout every 8 days of $200.25+ net profit")
    const payout = ruleText(custom).find((rule) => rule.title === "Payouts")!
    const text = renderToStaticMarkup(<>{payout.body}</>)
    expect(text).toContain("$200.25")
    expect(text).toContain("$2,000.50 for payout 1")
    expect(text).toContain("capped at $2,000.50")
  })
  it("states the funded rules and locks funded plans until their evaluation passes", () => {
    const texts = ruleText(fundedAccount, "0.65", "5000.00")
    expect(texts.map((t) => t.title).slice(0, 4)).toEqual(["Account type and margin", "Funded account", "Trailing drawdown", "Payouts"])
    const [, drawdown, payout] = texts.slice(1).map((rule) => renderToStaticMarkup(<>{rule.body}</>))
    expect(drawdown).toContain("It has locked at $100,000 and no longer trails.")
    expect(drawdown).toContain("closes the funded account")
    for (const text of ["8 qualifying days", "$200", "up to 50%", "$2,000 for payout 1", "$6,000 for payout 4 and later", "you keep 80%",
      "Each finished day counts once", "Your next payout is number 2, capped at $3,000."])
      expect(payout).toContain(text)
    const rules = render(<RulesView />, account)
    expect(rules).toContain("Pass Intraday 100K to unlock")
    expect(rules).toContain(">Locked</button>")
    for (const text of ["Evaluation plans", "Funded accounts", "Floor locks at", "8 days of $200+", "$5,000 intraday"]) expect(rules).toContain(text)
  })
  it("groups plans and enables the funded one only after a pass", () => {
    const locked = render(<ResetDialog trading={trading} attempt={2} onClose={() => {}} />, account)
    expect(locked).toContain("Evaluations")
    expect(locked).toContain("Funded accounts")
    expect(locked).toContain("Pass Intraday 100K to unlock")
    const radio = (html: string) => html.match(/<input[^>]*value="funded-intraday-100k"[^>]*>/)?.[0] ?? ""
    expect(radio(locked)).toContain('disabled=""')
    const open = render(<ResetDialog trading={trading} attempt={2} initial="funded-intraday-100k" onClose={() => {}} />, passed)
    expect(open).toContain(">unlocked<")
    expect(radio(open)).toContain('checked=""')
    expect(radio(open)).not.toContain("disabled")
    expect(open).toContain("Start Funded Intraday 100K")
  })
})

it.each(["evaluation", "verification"] as const)("explains all trade objectives and restrictions in %s", (phase) => {
  const r = { ...account.rules, phase, min_trades: 2, trade_consistency_percent: 40,
    min_hold_seconds: 60, microscalp_seconds: 30, microscalp_percent: 25,
    no_hedging: true, no_counter_positions: true, max_volume_percent: 25 }
  const value = { ...account, rules: r }
  const facts = planFacts({ ...plans[0]!, rules: r }).join(" ")
  const html = renderToStaticMarkup(<>{ruleText(value).map((rule) => <section key={rule.title}>{rule.body}</section>)}</>)
  for (const text of ["2 closed whole trades", "Best closed whole trade at most 40%", "round trips under 30s at most 25%"])
    expect(facts).toContain(text)
  for (const text of ["Whole option trades count only", "Trade consistency can hold", "Microscalping sums", "60", "25%"])
    expect(html).toContain(text)
  expect(facts).toContain("No opening hedges")
  expect(facts).toContain("No counter positions")
  expect(facts).toContain("Held plus opening contracts at most 25%")
  const funded = { ...value, rules: { ...r, phase: "funded" as const } }
  const fundedText = renderToStaticMarkup(<>{ruleText(funded).map((rule) => <section key={rule.title}>{rule.body}</section>)}</>)
  expect(fundedText).not.toContain("Microscalping sums")
  expect(fundedText).not.toContain("Trade consistency can hold")
})
