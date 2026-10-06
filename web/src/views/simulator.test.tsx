import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { Account, Plan, SettlementRecord } from "../api/trading-types"
import { account, fill, fundedAccount, plans, portfolio, risk, shareTrades, status, trades, twoStepPlans } from "../test/trading-fixtures"
import { DashboardView, equitySeries } from "./DashboardView"
import { PositionsView } from "./PositionsView"
import { JournalView, SettlementList, SettlementPrint } from "./JournalView"
import { RulesView, ruleText } from "./RulesView"
import { ResetDialog, planFacts } from "../components/ResetDialog"
import { describeAttribution } from "../lib/attribution"
import { exitLabel } from "../lib/journal"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const clients: QueryClient[] = []
function render(node: ReactNode, value: Account = account, available: Plan[] = plans) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(client)
  const queries = tradingQueries(0, "17", true)
  client.setQueryData(queries.account.queryKey, value)
  client.setQueryData(queries.portfolio.queryKey, portfolio)
  client.setQueryData(queries.risk.queryKey, risk)
  client.setQueryData(queries.fills.queryKey, { account_version: "17", fills: [fill] })
  client.setQueryData(queries.trades("current").queryKey, { account_version: "17", attempt: 2, trades })
  client.setQueryData(["plans"], { plans: available })
  return renderToStaticMarkup(<QueryClientProvider client={client}>{node}</QueryClientProvider>)
}
beforeEach(() => vi.mocked(useLive).mockReturnValue(liveState(status, null, "open")))
afterEach(() => { clients.splice(0).forEach((client) => client.clear()); vi.clearAllMocks() })

describe("simulator pages", () => {
  it("shows exact settlement references and provenance in the Journal and handles older journals", () => {
    const record: SettlementRecord = { symbol: "SPX   260918C06000000", underlying: "SPX", expiry: "2026-09-18", settlement: "AM",
      value: "6011.123456", time: "2026-09-18T13:30:00Z", quantity: 1, cash: "1112.3456", realised: "692.3456", fee: "0.00",
      source: { kind: "scenario_opening_print", provider: "replay (demo)", symbol: "SPX", quote_time: "2026-09-18T13:30:00Z" } }
    const settled = { ...trades[0]!, status: "closed" as const, closed: record.time, quantity: 0, closure: "settlement" as const, settlement_value: record.value, settlement_source: record.source!.kind }
    const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
    clients.push(client)
    const queries = tradingQueries(0, "17", true)
    client.setQueryData(queries.trades("current").queryKey, { account_version: "17", attempt: 2, trades: [settled] })
    client.setQueryData(queries.settlements.queryKey, { account_version: "17", settlements: [record] })
    const html = renderToStaticMarkup(<QueryClientProvider client={client}><JournalView /></QueryClientProvider>)
    for (const text of ["Settlements · all attempts", "Settlement reference 6011.123456", "scenario opening print (AM approximation)", "replay (demo)", "+$1,112.35", "+$692.35"])
      expect(html).toContain(text)
    expect(renderToStaticMarkup(<SettlementList records={[{ ...record, source: null }]} />)).toContain("Source unavailable")
    expect(renderToStaticMarkup(<SettlementPrint trade={{ ...settled, settlement_value: null, settlement_source: null }} />)).toContain("reference unavailable")
    expect(renderToStaticMarkup(<SettlementList records={[]} />)).toContain("No settlements yet")
  })
  it("labels awaiting AM opening prints and permits a manual import before they arrive", () => {
    const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
    clients.push(client)
    const queries = tradingQueries(0, "17", true)
    const position = { ...portfolio.positions[0]!, awaiting_settlement: true, settle_by: "opening_print" as const }
    client.setQueryData(queries.portfolio.queryKey, { ...portfolio, positions: [position] })
    const html = renderToStaticMarkup(<QueryClientProvider client={client}><PositionsView /></QueryClientProvider>)
    expect(html).toContain("Waiting for opening print · AM approximation")
    expect(html).toContain(`aria-label="Settle ${position.symbol}"`)
  })

  it("offers disposal only for longs and labels abandoned trades", () => {
    const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
    clients.push(client)
    const queries = tradingQueries(0, "17", true)
    client.setQueryData(queries.portfolio.queryKey, { ...portfolio, positions: portfolio.positions.map((p) => ({ ...p, no_bid: true, do_not_exercise: p.quantity > 0 })) })
    const html = renderToStaticMarkup(<QueryClientProvider client={client}><PositionsView /></QueryClientProvider>)
    for (const p of portfolio.positions) {
      expect(html.includes(`aria-label="Abandon ${p.symbol}"`)).toBe(p.quantity > 0)
      expect(html.includes(`aria-label="Exercise instruction for ${p.symbol}"`)).toBe(p.quantity > 0)
    }
    expect(html).toContain("Not to be exercised")
    expect(exitLabel({ ...trades[0]!, closure: "abandon" })).toBe("abandon")
  })
  it("plots equity by finished day with the trailing floor and the target line", () => {
    const { series, references } = equitySeries(account)
    expect(series[0]!.points.map((p) => p.y)).toEqual([100000, 100100, 100267.5])
    expect(series[1]!.points.map((p) => p.y)).toEqual([95000, 95300, 95300])
    expect(references).toEqual([{ y: 110000, label: "Target", color: "var(--bullish)" }])
    const practice = equitySeries({ ...account, rules: { ...account.rules, max_drawdown: null, profit_target: null },
      evaluation: { ...account.evaluation, floor: null, target_equity: null } })
    expect(practice.series).toHaveLength(1)
    expect(practice.references).toEqual([])
  })
  it("renders the dashboard tiles, progress checklist, rules and attempts", () => {
    const html = render(<DashboardView />)
    for (const text of ["Dashboard", "Intraday 100K · Evaluation · attempt 2", "New attempt", "+$267.50", "+0.27% of starting balance", "$110,000.00",
      "$9,732.50 to go · 2.7%", "Peak $100,300.00", "$95,300.00", "$4,967.50 buffer", "How am I doing?", "Remaining to target",
      "Drawdown left", "Buy-only, single leg", "Auto-close 5 min before expiry", "Underlyings: SPX", "#1 · Practice", "SPX Oct 16 7000C"])
      expect(html).toContain(text)
    expect(html).not.toContain("Evaluation failed")
    expect(html).not.toContain("NaN")
  })
  it.each(["passed", "failed"] as const)("explains leftovers after an attempt %s and shows archived decision provenance", (status) => {
    const decided: Account = { ...account, evaluation: { ...account.evaluation, status },
      attempts: [{ ...account.attempts[0]!, status, decided_at: "2026-09-22T14:00:00Z", decided_equity: "98123.45",
        rules: { ...account.rules, slippage_ticks: 2, fill_latency_ms: 1000 } }] }
    for (const view of [<DashboardView />, <PositionsView />]) {
      const html = render(view, decided)
      for (const text of ["Positions remain open", "system close keeps retrying", "closing liquidity", "place closing orders", "new attempt"])
        expect(html.toLowerCase()).toContain(text.toLowerCase())
    }
    const positions = render(<PositionsView />, decided)
    expect(positions).toContain("Close all")
    const history = render(<DashboardView />, decided)
    for (const text of ["Decided", "$98,123.45", "2 ticks of slippage", "1000 ms fill latency", "profit target", "drawdown"])
      expect(history).toContain(text)
    expect(render(<DashboardView />)).toContain("Rules were not recorded for this attempt.")
  })
  it("explains today's P&L by Greek on the dashboard and each position", () => {
    const attribution = { delta: 120, gamma: 4.5, vega: -30, theta: -12.25, other: 1.75, costs: -10.65, total: 73.35 }
    const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
    clients.push(client)
    const queries = tradingQueries(0, "17", true)
    client.setQueryData(queries.account.queryKey, account)
    client.setQueryData(queries.portfolio.queryKey, { ...portfolio, attribution,
      positions: portfolio.positions.map((p, i) => i === 0 ? { ...p, attribution } : p) })
    client.setQueryData(queries.trades("current").queryKey, { account_version: "17", attempt: 2, trades })
    client.setQueryData(["plans"], { plans })
    const html = renderToStaticMarkup(<QueryClientProvider client={client}><DashboardView /></QueryClientProvider>)
    expect(html).toContain("Today&#x27;s P&amp;L by Greek")
    for (const text of ["Delta", "Gamma", "Vega", "Theta", "Other", "Costs", "+$120.00", "−$30.00", "−$10.65", "+$73.35"]) expect(html).toContain(text)
    expect(describeAttribution(attribution)).toBe("Delta +$120.00 · Gamma +$4.50 · Vega −$30.00 · Theta −$12.25 · Other +$1.75 · Costs −$10.65")
    // Older servers send no attribution, and the panel stays away.
    expect(render(<DashboardView />)).not.toContain("by Greek")
  })
  it("announces a decided evaluation and a practice account", () => {
    const failed = render(<DashboardView />, { ...account, evaluation: { ...account.evaluation, status: "failed",
      decided_at: "2026-09-23T15:00:00Z", decided_equity: "95300.00", decision: "Equity $95300.00 reached the drawdown floor $95300.00" } })
    expect(failed).toContain("Evaluation failed")
    expect(failed).toContain("reached the drawdown floor")
    expect(failed).toContain("Start a new attempt")
    const practice = render(<DashboardView />, { ...account, rules: { ...account.rules, plan: "Practice", profit_target: null, max_drawdown: null },
      evaluation: { ...account.evaluation, enabled: false, floor: null, target_equity: null, target_remaining: null, drawdown_buffer: null } })
    expect(practice).toContain("No evaluation running")
    expect(practice).toContain("Start an evaluation")
  })
  it("shows a passed attempt's target as reached although the liquidation sold below it", () => {
    // B37: liquidation sells at the bid, so equity can end just under the target that passed.
    const passed = render(<DashboardView />, { ...account, evaluation: { ...account.evaluation, status: "passed",
      decided_at: "2026-09-23T15:00:00Z", decided_equity: "110001.30", equity: "109998.70", profit: "9998.70", target_remaining: "1.30",
      decision: "Equity $110001.30 reached the profit target $110000.00" } })
    expect(passed).toContain("Reached at $110,001.30")
    expect(passed).not.toContain("to go")
    expect(passed).not.toContain("$1.30")
  })
  it("names an end-of-day peak for the closes it follows", () => {
    // B50: the peak is the high-water mark the floor follows, which end-of-day plans take from closes.
    const eod = render(<DashboardView />, { ...account, rules: { ...account.rules, drawdown_mode: "end_of_day" } })
    expect(eod).toContain("Peak close $100,300.00")
    expect(eod).toContain("Peak closing equity")
    const intraday = render(<DashboardView />, { ...account, rules: { ...account.rules, drawdown_mode: "intraday" } })
    expect(intraday).toContain("Peak $100,300.00")
    expect(intraday).toContain("Peak equity")
  })
  it("renders journal statistics, the month calendar, reports and history", () => {
    const html = render(<JournalView />)
    for (const text of ["Journal", "2 closed trades · attempt 2", "+$167.20", "50.0%", "1 of 2 decided", "2.65", "+$268.50", "−$101.30",
      "September 2026", "Week 4", "Net P&amp;L by hold time", "Trades by hold time", "Win rate by hold time", "Trade history",
      "4m 49s", "+12.6%", "$4.25", "$4.80"])
      expect(html).toContain(text)
    expect(html).not.toContain("NaN")
  })
  it("names a replay's run and marks the trades a liquidation or a stop closed", () => {
    const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
    clients.push(client)
    const queries = tradingQueries(0, "17", true)
    client.setQueryData(queries.fills.queryKey, { account_version: "17", fills: [fill] })
    client.setQueryData(queries.trades("current").queryKey, { account_version: "17", attempt: 2,
      run: { id: "reversal-2026-09-22-7", scenario: "reversal", seed: "7", recording: null, date: "2026-09-22" },
      trades: trades.map((t) => t.id === "2" ? { ...t, closed_by: "system" as const, system_reason: "drawdown" }
        : t.status === "closed" ? { ...t, closed_by: "stop_loss" as const } : t) })
    const html = renderToStaticMarkup(<QueryClientProvider client={client}><JournalView /></QueryClientProvider>)
    for (const text of ["2 closed trades · attempt 2 · replay reversal seed 7", "(liquidated: drawdown)", "(stop)"]) expect(html).toContain(text)
    expect(exitLabel({ ...trades[1]!, closed_by: "system", system_reason: "expiry" })).toBe("auto-closed before expiry")
    expect(exitLabel({ ...trades[1]!, closed_by: "order" })).toBeNull()
    expect(exitLabel({ ...trades[1]!, closure: "settlement" })).toBe("settlement")
    expect(render(<JournalView />)).not.toContain("replay")
  })
  it("puts shares from exercise and assignment in the journal", () => {
    const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
    clients.push(client)
    const queries = tradingQueries(0, "17", true)
    client.setQueryData(queries.fills.queryKey, { account_version: "17", fills: [fill] })
    client.setQueryData(queries.trades("current").queryKey, { account_version: "17", attempt: 2, trades,
      share_trades: shareTrades.map((t) => t.id === "s1" ? { ...t, tags: ["income"], note: "held for the dividend" } : t) })
    const html = renderToStaticMarkup(<QueryClientProvider client={client}><JournalView /></QueryClientProvider>)
    for (const text of ["3 closed trades · attempt 2", "+$487.20", "10 contracts · 100 shares", "SPY 100 shares", "Share trades",
      "Exercised SPY Sep 22 500C at expiry", "Sold", "Assigned QQQ Sep 23 480P", "$504.20", "+$50.00", "+$320.00", "−$300.00",
      ">income</span>", 'title="held for the dividend"'])
      expect(html).toContain(text)
    // Older servers send no shares, and the panel stays away.
    expect(render(<JournalView />)).not.toContain("Share trades")
  })
  it("explains the active rules with the account's numbers and lists presets", () => {
    const texts = Object.fromEntries(ruleText(account, "0.65", "5000.00").map((rule) => [rule.title, renderToStaticMarkup(<>{rule.body}</>)] as const))
    expect(texts["Profit target"]).toContain("$110,000.00")
    expect(texts["Trailing drawdown"]).toContain("$95,300.00")
    expect(texts["Trailing drawdown"]).toContain("rises with every new equity high")
    expect(texts["Strategies"]).toContain("Buy-only")
    expect(texts["Expiring positions"]).toContain("last trade, working orders on it are cancelled")
    expect(texts["Expiring positions"]).toContain("4:15 pm")
    expect(texts["Daily loss limit"]).toContain("$5,000.00")
    expect(texts["Exercise and assignment"]).toContain("can be assigned overnight, in part and at random")
    const defined = Object.fromEntries(ruleText({ ...account, rules: { ...account.rules, buy_only: false, defined_risk: true } }, "0.65", "5000.00")
      .map((rule) => [rule.title, renderToStaticMarkup(<>{rule.body}</>)] as const))
    expect(defined["Strategies"]).toContain("Defined risk only: each short option needs a long of the same type")
    expect(texts["Exercise and assignment"]).toContain("Dividends come from a dividend file, from Massive or, in the demo feed and scenario replays, from the simulated calendar and the scenario&#x27;s own dividend entries. At the day&#x27;s rollover into the ex-date, shares held then receive the dividend and short shares pay it.")
    const html = render(<RulesView />)
    for (const text of ["Rules", "Profit target", "Trailing drawdown", "Evaluation plans", "Intraday 100K", "Every new high", "Buy only", "5 min before", "Start"])
      expect(html).toContain(text)
  })
  it("explains optional simulated fills and defaults older accounts to displayed fills", () => {
    const custom = { ...account, rules: { ...account.rules, fill_latency_ms: 1000, impact_ticks: 1, slippage_ticks: 1 } }
    const html = render(<RulesView />, custom)
    for (const text of ["Latency is 1000 ms on market time", "stops start waiting when triggered", "Simulated impact adds 1 extra tick",
      "Each combo leg uses its own size", "Automatic closes also wait", "queue position", "hidden liquidity", "whether the market would have traded at all"])
      expect(html).toContain(text)
    const facts = planFacts({ initial_cash: "100000", rules: custom.rules })
    expect(facts).toContain("1000 ms fill latency on market time")
    expect(facts).toContain("1 extra tick per displayed-size block")
    const legacy = render(<RulesView />)
    expect(legacy).toContain("No fill latency")
    expect(legacy).toContain("Fills use up to the displayed size")
    const dialog = render(<ResetDialog trading={status.trading!} attempt={2} initial="intraday-100k" onClose={() => {}} />)
    expect(dialog).toContain("As displayed")
    expect(dialog).toContain("Conservative")
    expect(dialog).toContain("this account’s new attempt")
  })
  it("shows slippage and portfolio margin and defaults older rules to strategy margin", () => {
    const custom = { ...account, rules: { ...account.rules, slippage_ticks: 2, margin: "portfolio" as const } }
    const html = render(<RulesView />, custom)
    for (const text of ["Portfolio margin", "11 price shocks", "−8% to +6%", "−15% to +15%", "$37.50", "long or short", "count as collateral",
      "Slippage is 2 ticks", "bracket exits and automatic closes", "slipped net exceeds the net limit"])
      expect(html).toContain(text)
    const facts = planFacts({ initial_cash: "100000", rules: custom.rules })
    expect(facts).toContain("Portfolio margin")
    expect(facts).toContain("2 ticks of slippage")
    const legacy = { ...account, rules: { ...account.rules, margin: undefined, slippage_ticks: undefined } }
    const defaults = render(<RulesView />, legacy)
    expect(defaults).toContain("Strategy margin")
    expect(defaults).toContain("Slippage is 0 ticks")
    // A plan's facts list only what differs from the defaults.
    expect(planFacts({ initial_cash: "100000", rules: legacy.rules }).join(" ")).not.toMatch(/margin|slippage/)
    const disabled = render(<RulesView />, { ...custom, rules: { ...custom.rules, buying_power: false } })
    expect(disabled).toContain("Buying power is not enforced")
    expect(disabled).toContain("Portfolio margin")
  })
  it("always displays the active account type and margin settings, including defaults", () => {
    const defaults = render(<RulesView />)
    expect(defaults).toContain("Margin account. Strategy margin (Reg T). House margin: 0%. Portfolio IV shock: ±0 points (inactive under strategy margin).")
    for (const type of ["cash", "ira"] as const) {
      const html = render(<RulesView />, { ...account, rules: { ...account.rules, account_type: type, house_margin_percent: 25 } })
      expect(html).toContain(type === "cash" ? "Cash account. Strategy margin" : "IRA (limited margin). Strategy margin")
      expect(html).toContain("House margin: 25%")
    }
    const html = render(<RulesView />, { ...account, rules: { ...account.rules, margin: "portfolio", house_margin_percent: 30, pm_vol_shock: 5 } })
    expect(html).toContain("Margin account. Portfolio margin. House margin: 30%. Portfolio IV shock: ±5 points.")
  })
  it("hides funded steps and their unlock in the simulator", () => {
    const passed: Account = { ...account, evaluation: { ...account.evaluation, status: "passed", decided_at: "2026-09-23T15:00:00Z",
      decided_equity: "110000.00", decision: "Equity $110000.00 reached the profit target $110000.00" } }
    const dialog = render(<ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} onClose={() => {}} />, passed)
    expect(dialog).toContain("Intraday 100K")
    expect(dialog).not.toContain("Funded")
    const rules = render(<RulesView />, passed)
    expect(rules).toContain("Evaluation plans")
    expect(rules).not.toContain("Funded accounts")
    const dashboard = render(<DashboardView />, passed)
    expect(dashboard).toContain("Evaluation passed")
    expect(dashboard).not.toContain("Start funded account")
  })
  it("keeps verification visible and hides the funded destination and its costs after verification passes", () => {
    const [challenge, verification, funded] = twoStepPlans
    const current: Account = { ...account, rules: challenge.rules }
    const rules = render(<RulesView />, current, twoStepPlans)
    expect(rules).toContain(verification.name)
    expect(rules).toContain(`Pass ${challenge.name} to unlock`)
    expect(rules).not.toContain(funded.name)
    expect(render(<DashboardView />, current, twoStepPlans)).not.toContain("Start verification")
    const passed: Account = { ...current, rules: verification.rules, next_plans: [funded.id],
      evaluation: { ...account.evaluation, status: "passed" } }
    const dashboard = render(<DashboardView />, passed, twoStepPlans)
    expect(dashboard).toContain("Verification passed")
    expect(dashboard).not.toContain("Start funded account")
    expect(dashboard).not.toContain(funded.name)
    const dialog = render(<ResetDialog trading={status.trading!} attempt={2} initial={funded.id} onClose={() => {}} />, passed, twoStepPlans)
    expect(dialog).toContain(verification.name)
    expect(dialog).not.toContain(funded.id)
    expect(dialog).not.toContain("activation $200.00")
    expect(dialog).not.toContain("This start charges")
  })
  it("shows funded plans, costs and payouts for an already-funded account", () => {
    const funded = twoStepPlans[2]
    const current: Account = { ...fundedAccount, rules: funded.rules }
    const dialog = render(<ResetDialog trading={status.trading!} attempt={3} initial={funded.id} onClose={() => {}} />, current, twoStepPlans)
    expect(dialog).toContain(funded.name)
    expect(dialog).toContain("activation $200.00")
    expect(dialog).toContain("This start charges $200.00 (reset fee)")
    expect(render(<RulesView />, current, twoStepPlans)).toContain("Funded accounts")
    expect(render(<DashboardView />, current, twoStepPlans)).toContain("Next payout")
  })
})
