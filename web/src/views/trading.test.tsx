import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { ApiError } from "../api/client"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import { Header } from "../components/Header"
import { KillSwitch, killChangeText } from "../components/KillSwitch"
import { LimitsEditor } from "../components/LimitsEditor"
import { FlattenDialog } from "../components/OrderActions"
import { OrderResult, OrderTicket } from "../components/OrderTicket"
import { LiquidityWarning } from "../components/LiquidityWarning"
import { PersonalRules } from "../components/PersonalRules"
import { ScenarioGrid, ScenarioPanel } from "../components/ScenarioGrid"
import { Sidebar } from "../components/Sidebar"
import { formatRoute, navigableViews, parseRoute, primaryViews } from "../lib/route"
import { account, chain, fill, order, portfolio, quote, risk, selection, status, summary, trading } from "../test/trading-fixtures"
import { ChainView } from "./ChainView"
import { OrdersView } from "./OrdersView"
import { PositionsView } from "./PositionsView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../lib/theme", () => ({ useTheme: () => ({ theme: "dark", toggleTheme: vi.fn() }) }))
const clients: QueryClient[] = []
function render(node: ReactNode, seed?: (client: QueryClient) => void) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(client)
  const queries = tradingQueries(0, "17", true)
  client.setQueryData(queries.portfolio.queryKey, portfolio)
  client.setQueryData(queries.orders.queryKey, { account_version: "17", orders: [order] })
  client.setQueryData(queries.fills.queryKey, { account_version: "17", fills: [fill] })
  client.setQueryData(queries.risk.queryKey, risk)
  client.setQueryData(queries.account.queryKey, account)
  client.setQueryData(queries.allOrders.queryKey, { account_version: "17", orders: [order] })
  client.setQueryData(["summary", "SPX", 1], summary)
  client.setQueryData(["chain", "SPX", selection.expiry.id, .05, 1], chain)
  seed?.(client)
  return renderToStaticMarkup(<QueryClientProvider client={client}>{node}</QueryClientProvider>)
}
beforeEach(() => vi.mocked(useLive).mockReturnValue(liveState(status, null, "open")))
afterEach(() => { clients.splice(0).forEach((client) => client.clear()); vi.clearAllMocks() })

describe("paper trading fixtures", () => {
  it("shows pending personal rules, exposure limits and market-time cooldown", () => {
    const guardrails = { soft_floor: "99000", soft_floor_percent: 0, max_opening_trades: 5, cooldown_loss: "100", cooldown_minutes: 5, profit_lock: "500" }
    const html = render(<PersonalRules risk={{ ...risk, guardrails, pending_guardrails: { ...guardrails, max_opening_trades: 10 },
      pending_limits: { ...risk.limits, aggregate: { dollar_delta: 10000, vega: 250 } },
      guardrail_state: { opening_trades: 2, cooldown_until: null, cooldown_seconds: 90, latched: ["COOLDOWN"], soft_floor: "99000" },
    }} />)
    expect(html).toContain("10 next trading day")
    expect(html).toContain("dollar delta: 10000 · vega: 250")
    expect(html).toContain("Cooldown 2 market minutes left")
    expect(html).toContain("$99,000.00")
  })
  it("leaves buying power to the server for portfolio margin or slippage", () => {
    for (const optional of [{ margin: "portfolio" as const }, { slippage_ticks: 2 }]) {
      const html = render(<OrderTicket selection={selection} quote={quote} trading={trading} onClose={() => {}} />, (client) => {
        client.setQueryData(tradingQueries(0, "17", true).account.queryKey,
          { ...account, rules: { ...account.rules, ...optional }, buying_power: { ...account.buying_power, available: "0" } })
      })
      expect(html).toContain("Simulated order preview")
      expect(html).not.toContain("Buying power effect")
      expect(html).not.toContain("the server will reject it")
      if (optional.slippage_ticks) expect(html).toContain("the server preview includes it")
    }
  })
  it("keeps market size labels factual for optional fill models", () => {
    const legs = [{ label: "Call", quote, side: "buy" as const, quantity: 20 }]
    const modeled = render(<LiquidityWarning market modeled legs={legs} />)
    expect(modeled).toContain("inputs to the account’s simulated latency and impact model")
    expect(modeled).not.toContain("displayed quote and size only")
    expect(render(<LiquidityWarning market legs={legs} />)).toContain("displayed quote and size only")
  })
  it("labels ticket prices as estimates when latency or simulated impact is enabled", () => {
    for (const optional of [{ fill_latency_ms: 1000 }, { impact_ticks: 1 }]) {
      const html = render(<OrderTicket selection={selection} quote={quote} trading={trading} onClose={() => {}} />, (client) => {
        client.setQueryData(tradingQueries(0, "17", true).account.queryKey,
          { ...account, rules: { ...account.rules, ...optional } })
      })
      expect(html).toContain("Simulated fills use the account’s latency and size impact")
      expect(html).toContain("cannot predict the later quote")
      expect(html).not.toContain("Marketable: fills now")
    }
  })
  it("renders a labelled buy ticket with quotes, sizes, exact premium and its own Greeks", () => {
    const html = render(<OrderTicket selection={selection} quote={quote} trading={trading} onClose={() => {}} />)
    expect(html).toContain("<dialog")
    // Buying the 7000 call at $4.60 pays off above 7004.60 by expiry, about even odds at a 7,010 forward.
    expect(html).toContain("Breakeven at expiry</dt><dd class=\"text-right tabular\">7004.60")
    expect(html).toContain("≈ 50%")
    expect(html).toContain("aria-labelledby=")
    expect(html).toContain("2026-10-16 PM")
    expect(html).toContain("7000 call")
    expect(html).toContain('aria-checked="true" tabindex="0" class="rounded px-2 py-0.5 text-xs transition-colors bg-raised text-foreground">Buy</button>')
    expect(html).toContain("Long Call")
    expect(html).toContain("Simulated order preview")
    expect(html).toContain("4.50 × 10")
    expect(html).toContain("4.60 × 3")
    expect(html).toContain("4.55")
    expect(html).toContain("$460.00")
    expect(html).toContain("50.00")
    expect(html).toContain("0.2000")
    expect(html).toContain("1225.00")
    expect(html).toContain("-85.00")
    expect(html).toContain("Not provided by server")
    expect(html).toContain('aria-label="Submit order"')
    expect(html).toContain("Buy 1 Long Call @ $4.60")
  })
  it("allows closing tickets and Flatten while latched and blocks opening tickets", () => {
    const latched = { ...trading, kill_latched: true }
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, trading: latched }, null, "open"))
    const close = render(<OrderTicket selection={{ ...selection, cell: "bid", price: "4.50" }} quote={quote} trading={latched} onClose={() => {}} />)
    expect(close).toContain("reduce-only: closing orders and exits still work")
    expect(close).not.toMatch(/aria-label="Submit order"[^>]*disabled=""/)
    const open = render(<OrderTicket selection={selection} quote={quote} trading={latched} onClose={() => {}} />)
    expect(open).toMatch(/aria-label="Submit order"[^>]*disabled=""/)
    const flatten = render(<FlattenDialog positions={[portfolio.positions[0]!]} orders={[]} trading={latched} onClose={() => {}} />)
    expect(flatten).toContain("Close 1 position</button>")
    expect(flatten).not.toMatch(/disabled=""[^>]*>Close 1 position/)
    const positions = render(<PositionsView />)
    expect(positions).not.toMatch(/disabled=""[^>]*>Close all/)
  })
  it("shows the selected expiry's auto-close countdown on the chain", () => {
    const auto_close = "2026-09-23T15:42:34Z"
    const html = render(<ChainView symbol="SPX" expiry={null} onExpiry={() => {}} />, (client) => {
      client.setQueryData(["summary", "SPX", 1], { ...summary, expiries: [{ ...selection.expiry, auto_close }] })
    })
    expect(html).toContain("auto-close in 12:34")
  })
  it("renders sells, null quotes and token/kill write gates", () => {
    const html = render(<OrderTicket selection={{ ...selection, cell: "bid", price: "4.50" }} quote={{ ...quote, bid: null, ask: null, mid: null, bid_size: null, ask_size: null, delta: null }} trading={{ ...trading, write: "token", kill_latched: true }} onClose={() => {}} />)
    expect(html).toContain('aria-checked="true" tabindex="0" class="rounded px-2 py-0.5 text-xs transition-colors bg-raised text-foreground">Sell</button>')
    expect(html).toContain("— × —")
    expect(html).toContain("Estimated premium · credit")
    expect(html).toContain("Enter write token")
    expect(html).toContain("Kill switch latched")
    expect(html).toMatch(/aria-label="Submit order" disabled=""/)
    expect(html).not.toContain("NaN")
  })
  it.each(["0.65", "0.00"])("uses the server fee %s and removes the manual override", (fee) => {
    const html = render(<OrderTicket selection={{ ...selection, price: "15.8" }} quote={quote} trading={{ ...trading, fee_per_contract: fee, initial_cash: "100000.00" }} onClose={() => {}} />)
    expect(html).toContain(`$${fee}`)
    expect(html).not.toContain("Not provided by server")
    expect(html).not.toContain("Fee / contract ($, estimate)")
    expect(html).toContain('value="15.80"')
    expect(html).toContain("Increase limit price one tick")
    expect(html).toContain("Decrease limit price one tick")
  })
  it("blocks the ticket and notes Portfolio while the session is closed", () => {
    const session = { name: "closed" as const, open: false, note: "closed (weekend)" }
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!, session }] }, null, "open"))
    const html = render(<OrderTicket selection={selection} quote={quote} trading={trading} onClose={() => {}} />)
    expect(html).toContain("SPX options are not trading now (closed (weekend)); paper orders wait for the next session.")
    expect(html).toMatch(/aria-label="Submit order" disabled=""/)
    expect(render(<PositionsView />)).toContain("SPX options are not trading now")
  })
  it.each(["global", "curb"] as const)("takes limit orders only in the %s session", (name) => {
    const session = { name, open: true, note: "Product session" }
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!, session }] }, null, "open"))
    const html = render(<OrderTicket selection={selection} quote={quote} trading={trading} onClose={() => {}} />)
    expect(html).toContain(name === "global" ? "SPX is in its overnight session (8:15 pm to 9:25 am ET)" : "SPX is in its curb session (4:15 to 5:00 pm ET)")
    expect(html).toContain("limit orders only. EXTO and GTC + EXTO conditions and exits work here; GTC and GTD wait for regular hours.")
    expect(html).not.toContain('aria-label="Condition"')
    expect(html).not.toContain('aria-label="Bracket"')
    expect(html).not.toContain(">Market</button>")
    expect(html).not.toMatch(/aria-label="Submit order" disabled=""/)
    expect(render(<PositionsView />)).not.toContain("not trading now")
  })
  it.each(["status", "tick"] as const)("uses the paper gate from %s for ticket and Portfolio notices", (source) => {
    const message = "SPX quotes are 10h 20m behind the market; the feed appears to have stalled"
    const underlyings = [{ ...status.underlyings[0]!,
      session: { name: "regular" as const, open: true, note: "Regular" },
      paper: { accepting: false, reason: "FEED_STALLED", message },
    }]
    vi.mocked(useLive).mockReturnValue(source === "status"
      ? liveState({ ...status, underlyings }, null, "open")
      : liveState(status, { type: "tick", engine: status.engine, feed: status.feed, underlyings }, "open"))
    const html = render(<OrderTicket selection={selection} quote={quote} trading={trading} onClose={() => {}} />)
    expect(html).toContain(message)
    expect(html).toMatch(/aria-label="Submit order" disabled=""/)
    expect(render(<PositionsView />)).toContain(message)
  })
  it("uses paper acceptance over wall-clock session in the ticket and Portfolio", () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!,
      session: { name: "closed", open: false, note: "Closed" },
      paper: { accepting: true, reason: null, message: null },
    }] }, null, "open"))
    const html = render(<OrderTicket selection={selection} quote={quote} trading={trading} onClose={() => {}} />)
    expect(html).not.toContain("not trading now")
    expect(html).not.toMatch(/aria-label="Submit order" disabled=""/)
    expect(render(<PositionsView />)).not.toContain("not trading now")
  })
  it("limits Portfolio session notices to tradable underlyings, positions, or open orders", () => {
    const session = { name: "closed" as const, open: false, note: "Closed" }
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [
      { ...status.underlyings[0]!, session },
      ...["SPY", "QQQ", "OEX", "HELD", "ORDER", "DONE", "FLAT"].map((symbol) => ({ ...status.underlyings[0]!, symbol, session, has_tradable_contracts: false })),
    ] }, null, "open"))
    const html = render(<PositionsView />, (client) => {
      const queries = tradingQueries(0, "17", true)
      client.setQueryData(queries.portfolio.queryKey, { ...portfolio, positions: [
        { ...portfolio.positions[0]!, underlying: "HELD" }, { ...portfolio.positions[0]!, symbol: "flat", underlying: "FLAT", quantity: 0 },
      ] })
      client.setQueryData(queries.orders.queryKey, { account_version: "17", orders: [
        { ...order, underlying: "ORDER" }, { ...order, id: "done", underlying: "DONE", status: "cancelled" },
      ] })
    })
    for (const symbol of ["SPX", "HELD", "ORDER"]) expect(html).toContain(`${symbol} options are not trading now`)
    for (const symbol of ["SPY", "QQQ", "OEX", "DONE", "FLAT"]) expect(html).not.toContain(`${symbol} options are not trading now`)
  })
  it("uses the selected underlying's regular session, even if another market is closed", () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [
      { ...status.underlyings[0]!, symbol: "SPY", session: { name: "closed", open: false, note: "Closed" } },
      { ...status.underlyings[0]!, session: { name: "regular", open: true, note: "Regular" } },
    ] }, null, "open"))
    const html = render(<OrderTicket selection={selection} quote={quote} trading={trading} onClose={() => {}} />)
    expect(html).not.toContain("not trading now")
    expect(html).not.toMatch(/aria-label="Submit order" disabled=""/)
  })
  it.each(["working", "partially_filled", "filled", "rejected", "cancelled"] as const)("renders inline order status %s and typed risk rejections", (orderStatus) => {
    const html = render(<OrderResult order={{ ...order, status: orderStatus, reason: { code: "QUOTE_STALE", message: "Quote is too old" } }} error={new ApiError(422, "Delta cap exceeded", "DELTA_LIMIT", 200, 100, "aggregate")} />)
    expect(html).toContain(orderStatus === "partially_filled" ? "Partial fill" : orderStatus)
    expect(html).toContain("QUOTE_STALE")
    expect(html).toContain("DELTA_LIMIT")
    expect(html).toContain("Actual 200.00 · Limit 100.00 · aggregate")
  })
  it("renders positions, awaiting settlement, buying power and risk ranges", () => {
    const html = render(<PositionsView />)
    for (const text of ["Positions", "$99,988.70", "−$11.30", "Valuation incomplete", "Awaiting settlement", "Age unavailable", "$4.55", "2 s old", "+2 long", "−$10.00", "−1.1%", "$99,078.70", "fees $1.30", "Close SPXW  261016C07000000", "Reachable including open orders", "1750000.00", "87.5%", "Spot × volatility scenarios", "Armed", "Edit limits"]) expect(html).toContain(text)
    // Awaiting settlement cannot be closed with an order.
    expect(html).not.toContain("Close SPX   260918P06800000")
    expect(html).toContain("overflow-x-auto")
    expect(html).not.toContain("NaN")
    expect(html).not.toContain("undefined")
  })
  it("renders complete and empty position and order fixtures", () => {
    const empty = (client: QueryClient) => {
      const queries = tradingQueries(0, "17", true)
      client.setQueryData(queries.portfolio.queryKey, { ...portfolio, valuation_complete: true, quality_flags: [], positions: [] })
      client.setQueryData(queries.allOrders.queryKey, { account_version: "17", orders: [] })
      client.setQueryData(queries.fills.queryKey, { account_version: "17", fills: [] })
    }
    const html = render(<PositionsView />, empty)
    expect(html).toContain("No open positions")
    expect(html).not.toContain("Valuation incomplete")
    expect(render(<OrdersView />, empty)).toContain("No working orders.")
  })
  it("groups working orders by trading day with status, reason and cancel", () => {
    const html = render(<OrdersView />, (client) => client.setQueryData(tradingQueries(0, "17", true).allOrders.queryKey, { account_version: "17", orders: [
      order, { ...order, id: "order-2", status: "cancelled", reason: { code: "USER_CANCEL", message: "Cancelled by caller" } },
      { ...order, id: "order-3", status: "filled", origin: "system", client_order_id: "system:drawdown:3" }] }))
    for (const text of ["Working · 1", "All · 3", "Wed, Sep 23, 2026", "SPX Oct 16 7000C", "BUY", "2 / 5", "$4.60", "Partial", "Cancel order order-1"]) expect(html).toContain(text)
    // The default Working tab hides terminal orders.
    expect(html).not.toContain("Cancelled by you")
    expect(html).not.toContain("NaN")
  })
  it("keeps null heatmap cells distinct from the exact unchanged-spot/vol zero", () => {
    const html = render(<ScenarioGrid scenarios={risk.scenarios} />)
    expect(html).toContain('title="0% spot, 0 vol points: 0 · unchanged spot and vol"')
    expect(html).toContain("$0.00")
    expect(html).toContain("unavailable · volatility clamped")
    expect(html).toContain("Scenario valuation incomplete")
    expect(html).toContain("Loss")
    expect(html).toContain("Gain")
  })
  it("offers smile dynamics over the account's sticky-strike grid", () => {
    const html = render(<ScenarioPanel scenarios={{ ...risk.scenarios, dynamics: "sticky_strike", surface_fallback: [] }} />)
    expect(html).toContain('aria-label="Smile dynamics"')
    for (const text of [">Sticky strike</option>", ">Sticky moneyness</option>", ">Sticky delta</option>", "as the account&#x27;s rules value it"])
      expect(html).toContain(text)
    expect(html).toContain('title="0% spot, 0 vol points: 0 · unchanged spot and vol"')
  })
  it("renders a revision-based limits editor and a clear latched kill state", () => {
    const html = render(<LimitsEditor initial={risk} trading={trading} onClose={() => {}} />)
    expect(html).toContain("Editing revision 3")
    expect(html).toContain('value="5000.00"')
    expect(html).toContain("Relative price band (ratio)")
    const killHtml = render(<KillSwitch kill={{ latched: true, reason: "Daily loss reached" }} trading={trading} />)
    expect(killHtml).toContain("LATCHED · reduce-only: closing orders and exits still work")
    expect(killHtml).toContain("Daily loss reached")
    expect(killHtml).toContain("Reset kill switch…")
    const history = render(<KillSwitch kill={{
      latched: true, reason: "stepping away",
      reset_blocked: { code: "DAILY_LOSS", message: "A reset trips again while the marked loss $620.00 exceeds the limit of $100.00", actual: 620, limit: 100, scope: "aggregate" },
      history: [
        { time: "2026-09-22T14:00:01Z", action: "trip", reason: "DAILY_LOSS", previous: null, actor: "system" },
        { time: "2026-09-22T14:00:02Z", action: "trip", reason: "stepping away", previous: "DAILY_LOSS", actor: "trader" },
      ],
    }} trading={trading} />)
    expect(history).toContain("Kill-switch reset now would not clear it: A reset trips again while the marked loss $620.00 exceeds the limit of $100.00")
    expect(history).toContain("History · 2")
    expect(history).toContain("Tripped: stepping away (replaced DAILY_LOSS)")
    expect(killChangeText({ time: "", action: "reset", reason: "back", previous: "pause", actor: "trader" })).toBe("Reset: back (cleared pause)")
    expect(killChangeText({ time: "", action: "release", reason: null, previous: "COOLDOWN", actor: "system" })).toBe("Released: COOLDOWN expired")
    const daily = render(<KillSwitch kill={{ latched: true, reason: "TRADE_LIMIT",
      reset_blocked: { code: "TRADE_LIMIT", message: "Neither a kill-switch reset nor an account reset clears it" },
      history: [{ time: "2026-09-22T14:00:01Z", action: "trip", reason: "TRADE_LIMIT", previous: null, actor: "system" }],
    }} trading={trading} />)
    expect(daily).toContain("Neither a kill-switch reset nor an account reset clears it")
    expect(daily).toContain("The opening order that reached the limit may finish its partial fills")
    expect(daily).not.toContain("cleared TRADE_LIMIT")
  })
  it("exposes call and put bid/ask buttons and an untradable reason", () => {
    const html = render(<ChainView symbol="SPX" expiry={selection.expiry.id} onExpiry={() => {}} />)
    for (const label of ["sell 7000 call at bid", "buy 7000 call at ask", "sell 7000 put at bid", "buy 7000 put at ask"]) expect(html).toContain(label)
    const untradable = render(<ChainView symbol="SPX" expiry={selection.expiry.id} onExpiry={() => {}} />, (client) => client.setQueryData(["chain", "SPX", selection.expiry.id, .05, 1], { ...chain, strikes: [{ ...chain.strikes[0], call: { ...quote, tradable: false, untradable_reason: "AMERICAN_UNSUPPORTED" } }] }))
    expect(untradable).toContain("AMERICAN_UNSUPPORTED")
  })
  it("hides every trading entry point on older servers, even with cached trading payloads", () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, trading: undefined }, null, "open"))
    const header = render(<Header symbol="SPX" onSymbol={() => {}} />)
    expect(header).not.toContain("Positions")
    const sidebar = render(<Sidebar view="chain" onView={() => {}} open={false} onClose={() => {}} />)
    for (const hidden of ["Dashboard", "Positions", "Orders", "Journal", "Rules", "Account summary"]) expect(sidebar).not.toContain(hidden)
    expect(sidebar).toContain("Trade")
    expect(render(<PositionsView />)).toBe("")
    const html = render(<ChainView symbol="SPX" expiry={selection.expiry.id} onExpiry={() => {}} />)
    expect(html).not.toContain("paper order")
    expect(html).not.toContain("at bid")
  })
  it("routes the simulator pages, keeps old Portfolio links and numbers the sidebar", () => {
    expect(primaryViews).toEqual(["dashboard", "chain", "positions", "orders", "journal", "rules", "playbooks", "brief", "payouts"])
    expect(parseRoute("#/SPX/portfolio").view).toBe("positions")
    expect(formatRoute({ view: "positions", symbol: "SPX", expiry: null })).toBe("#/SPX/positions")
    expect(navigableViews(true, false)).toEqual(["dashboard", "chain", "positions", "orders", "journal", "rules", "playbooks", "brief"])
    expect(navigableViews(false, true)).toEqual(["chain", "brief"])
    const sidebar = render(<Sidebar view="positions" onView={() => {}} open={false} onClose={() => {}} />)
    expect(sidebar).toContain('title="Positions (3)"')
    // The simulator funds no one: Payouts appears only for an account that is already funded.
    expect(sidebar).not.toContain("Payouts")
    const funded = render(<Sidebar view="positions" onView={() => {}} open={false} onClose={() => {}} />, (client) =>
      client.setQueryData(tradingQueries(0, "17", true).account.queryKey, { ...account, rules: { ...account.rules, phase: "funded" } }))
    expect(funded).toContain('title="Payouts (9)"')
    expect(sidebar).toContain('aria-current="page"')
    for (const text of ["Intraday 100K", ">active<", "$100,267.50", "+$267.50", "(+0.27%)", "$99,078.70", "Progress to profit target"]) expect(sidebar).toContain(text)
  })
  it("picks positions to close together in one order", () => {
    const html = render(<PositionsView />)
    expect(html).toContain(">Pick</th>")
    expect(html).toContain("Pick SPX Oct 16 7000C to close together")
    expect(html).toContain("Pick positions to close them in one order")
    expect(html).toMatch(/<button type="button" class="trade-button" disabled="">Close together<\/button>/)
  })
})

it("shows liquidity columns on each side, volume/OI on hover and volume coverage", () => {
  const html = render(<ChainView symbol="SPX" expiry={selection.expiry.id} onExpiry={() => {}} />, (client) => {
    client.setQueryData(["chain", "SPX", selection.expiry.id, .05, 1], { ...chain,
      expiry: { ...chain.expiry, coverage: { options: 2, quoted: 2, priced: 2, open_interest: 2, volume: 1 } },
      strikes: [{ ...chain.strikes[0], call: { ...quote, volume: 125, oi: 500 }, put: { ...quote, bid: 1, ask: 3, volume: null } }],
    })
  })
  expect(html.match(/>Vol<\/th>/g)).toHaveLength(2)
  expect(html.match(/>Spr %<\/th>/g)).toHaveLength(2)
  expect(html.match(/>Liq<\/th>/g)).toHaveLength(2)
  expect(html).toContain('title="Volume / OI: 0.25"')
  expect(html).toContain("Vol 1/2")
  expect(html).toContain("Volume unknown")
  expect(html).toContain("Liquidity columns")
})

it("labels generated volume as simulated on the chain", () => {
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, provider: { ...status.provider, simulated: true } }, null, "open"))
  expect(render(<ChainView symbol="SPX" expiry={selection.expiry.id} onExpiry={() => {}} />)).toContain("simulated prices and volume")
})

it("labels the demo feed simulated and ages it on its market clock", () => {
  const simulated = { ...status, provider: { ...status.provider, name: "demo", simulated: true },
    underlyings: status.underlyings.map(underlying => ({ ...underlying, as_of: "2026-09-18T13:30:00Z",
      session: { name: "regular" as const, open: true, note: "Regular session" } })) }
  vi.mocked(useLive).mockReturnValue(liveState(simulated, null, "open"))
  const html = render(<Header symbol="SPX" onSymbol={() => {}} />)
  expect(html).toContain("simulated prices")
  expect(html).toContain("&lt;1 min old")
  expect(html).not.toContain(">stale<")
})
