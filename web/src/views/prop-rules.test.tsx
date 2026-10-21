import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { Account, RulesCompareResponse } from "../api/trading-types"
import { RulesCompareResult, packTemplate, replayVerdict } from "../components/PropRulesPanels"
import { account, fundedAccount, plans, portfolio, risk, status, trades } from "../test/trading-fixtures"
import { PayoutReviewPanel, PayoutsView } from "./PayoutsView"
import { RulesView } from "./RulesView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../lib/features", () => ({ showFundedAccounts: true }))
const clients: QueryClient[] = []
function render(node: ReactNode, value: Account = account) {
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
beforeEach(() => vi.mocked(useLive).mockReturnValue(liveState(status, null, "open")))
afterEach(() => { clients.splice(0).forEach((client) => client.clear()); vi.clearAllMocks() })

const comparison: RulesCompareResponse = {
  account: "main", attempt: 1, days: ["2026-09-21", "2026-09-22"], basis: "Recorded path", label: "Replay of recorded equity; trades are not re-executed", simulated: true,
  sets: [
    { name: "Mine", plan_id: null, initial_cash: "100000.00", rules: account.rules, status: "active", code: null, message: null, decided_day: null,
      decided_at: null, decided_equity: null, locked_days: ["2026-09-21"], closest_floor: "1200.00", not_replayed: [],
      days: [{ day: "2026-09-21", outcome: "locked", close: "99700.00", floor: "95000.00" }, { day: "2026-09-22", outcome: "active", close: "100300.00", floor: "95300.00" }] },
    { name: "Tight", plan_id: "intraday-100k", initial_cash: "100000.00", rules: account.rules, status: "failed", code: "DRAWDOWN_FLOOR", message: "Equity reached the floor",
      decided_day: "2026-09-21", decided_at: "2026-09-21T15:00:00Z", decided_equity: "99000.00", locked_days: [], closest_floor: "0.00", not_replayed: ["strategies"],
      days: [{ day: "2026-09-21", outcome: "failed", close: "99000.00", floor: "99000.00" }, { day: "2026-09-22", outcome: "after", close: "99600.00", floor: "99000.00" }] },
  ],
}

describe("prop rules in the terminal", () => {
  it("shows where compared rule sets pass, fail or lock, day by day", () => {
    const html = renderToStaticMarkup(<RulesCompareResult result={comparison} />)
    for (const text of ["Mine", "(yours)", "Active · 1 locked day", "Failed 2026-09-21 · drawdown floor", "strategies",
      "Tight 2026-09-21 Failed", "Mine 2026-09-21 Day locked", "trades are not re-executed"]) expect(html).toContain(text)
    expect(replayVerdict(comparison.sets[1]!)).toBe("Failed 2026-09-21 · drawdown floor")
  })
  it("explains the prop rules in force with the account's numbers", () => {
    const value: Account = { ...account, rules: { ...account.rules, drawdown_basis: "balance", minute_volume_percent: 20, best_position_percent: 40,
      open_loss_cap: { basis: "drawdown_room", amount: null, percent: 30, action: "flatten" } },
      evaluation: { ...account.evaluation, open_loss: { limit: "1500.00", positions: ["SPXW  261022C05000000"], trades: ["4"] },
        best_position: { cap: "4000.00", excess: "250.00", counted_profit: "750.00", trade: { id: "4", pnl: "4250.00" } } } }
    const html = render(<RulesView />, value)
    for (const text of ["Floor on the closed balance", "Open-loss cap", "$1,500.00", "SPXW  261022C05000000", "last minute&#x27;s volume",
      "MINUTE_VOLUME", "Best-position cap", "$250.00 is left out", "Compare rules", "Challenge economics", "Rule packs",
      "Built-in plans are illustrative"]) expect(html).toContain(text)
  })
  it("names the payout review step and what it forfeits", () => {
    const review = { step: "forfeit" as const, forfeited: "93.50", trades: [{ id: "7", pnl: "93.50", held_seconds: 1, closed: "2026-09-23T14:00:00Z", offences: ["short_hold" as const] }] }
    const html = renderToStaticMarkup(<PayoutReviewPanel review={review} forfeit="93.50" />)
    expect(html).toContain("Payout review · forfeit")
    expect(html).toContain("$93.50, is forfeited")
    expect(html).toContain("held too briefly")
    const funded: Account = { ...fundedAccount, payout: { ...fundedAccount.payout!, review, forfeit: "93.50", after_payout: "reset", reset_balance: "100000.00" } }
    const page = render(<PayoutsView />, funded)
    expect(page).toContain("Payout review · forfeit")
    expect(page).toContain("the account resets to $100,000.00")
  })
  it("starts a pack template from the account's own rules", () => {
    const template = packTemplate(account, "2026-09-22")
    expect(template).toMatchObject({ format: "openport.rule-pack", version: 1, effective_from: "2026-09-22" })
    expect(template.plans[0]!.rules.plan_id).toBeUndefined()
    expect(template.plans[0]!.initial_cash).toBe(account.evaluation.starting_balance)
  })
})
