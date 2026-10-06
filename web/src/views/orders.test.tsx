import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { ClosePositionsResponse, FlattenPreview, Order, Position, StockHolding, WhatIfAccount } from "../api/trading-types"
import { CancelAllDialog, EditOrderDialog, FlattenDialog, FlattenDryRun, FlattenOutcome } from "../components/OrderActions"
import { OrderDetailDialog } from "../components/OrderDetail"
import { ExerciseDialog } from "../components/StockActions"
import { account, fill, order, portfolio, risk, status, trading } from "../test/trading-fixtures"
import { TradeDetail } from "./JournalView"
import { trades } from "../test/trading-fixtures"
import { OrdersView } from "./OrdersView"
import { PositionsView } from "./PositionsView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const clients: QueryClient[] = []
function render(node: ReactNode, orders: Order[] = [order]) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(client)
  const queries = tradingQueries(0, "17", true)
  client.setQueryData(queries.portfolio.queryKey, portfolio)
  client.setQueryData(queries.orders.queryKey, { account_version: "17", orders: orders.filter((o) => o.status !== "filled") })
  client.setQueryData(queries.allOrders.queryKey, { account_version: "17", orders })
  client.setQueryData(queries.fills.queryKey, { account_version: "17", fills: [fill] })
  client.setQueryData(queries.risk.queryKey, risk)
  client.setQueryData(queries.account.queryKey, account)
  return renderToStaticMarkup(<QueryClientProvider client={client}>{node}</QueryClientProvider>)
}
beforeEach(() => vi.mocked(useLive).mockReturnValue(liveState(status, null, "open")))
afterEach(() => { clients.splice(0).forEach((client) => client.clear()); vi.clearAllMocks() })

const stop: Order = { ...order, id: "9", status: "armed", role: "stop_loss", side: "sell", type: "market", time_in_force: "ioc",
  limit_price: null, quantity: 2, filled_quantity: 0, remaining_quantity: 2, parent: "order-1",
  trigger: { source: "option", direction: "at_or_below", level: "3.50" } }

describe("shares from exercise and assignment", () => {
  const spyCall: Position = { ...portfolio.positions[0]!, symbol: "SPY   261022C00500000", underlying: "SPY", strike: 500, type: "call",
    quantity: 2, mark: "21.10" }
  const shares: StockHolding = { symbol: "SPY", shares: -100, average_price: "510.00", basis: "-51000.00", mark: "505.00",
    mark_time: "2026-09-23T15:00:00.000Z", market_value: "-50500.00", unrealised: "500.00", realised: "0.00", fees: "0.00",
    fresh: true, attribution: { delta: 500, gamma: 0, vega: 0, theta: 0, other: 0, costs: 0, total: 500 } }
  it("lists the shares, and offers exercise on equity options but not index ones", () => {
    const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
    clients.push(client)
    const queries = tradingQueries(0, "17", true)
    client.setQueryData(queries.portfolio.queryKey, { ...portfolio, positions: [portfolio.positions[0]!, spyCall], stocks: [shares] })
    client.setQueryData(queries.orders.queryKey, { account_version: "17", orders: [] })
    client.setQueryData(queries.allOrders.queryKey, { account_version: "17", orders: [] })
    client.setQueryData(queries.risk.queryKey, risk)
    client.setQueryData(queries.account.queryKey, account)
    const html = renderToStaticMarkup(<QueryClientProvider client={client}><PositionsView /></QueryClientProvider>)
    expect(html).toContain("Shares · 1")
    expect(html).toContain("-100 short")
    expect(html).toContain("$510.00")
    expect(html).toContain('aria-label="Close SPY shares"')
    expect(html).toContain('aria-label="Exercise SPY   261022C00500000"')
    expect(html).not.toContain(`aria-label="Exercise ${portfolio.positions[0]!.symbol}"`)
  })
  it("explains an exercise: intrinsic value, the shares and the time value given up", () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!, symbol: "SPY", spot: 520 }] }, null, "open"))
    const html = render(<ExerciseDialog position={spyCall} trading={trading} onClose={() => {}} />)
    expect(html).toContain("close at intrinsic value, $20.00 with SPY at 520.00")
    expect(html).toContain("buy 200 SPY shares at that price: together, the $500.00 strike")
    expect(html).toContain("gives up about $220.00 of time value")
    expect(html).toContain(">Exercise 2</button>")
  })
})

describe("working orders", () => {
  it("shows current walking prices, next steps and editable terms", () => {
    const walking = { ...order, walk: { step: "0.10", seconds: 10, limit: "5.00" },
      next_walk: { time: "2026-09-22T14:00:10.000Z", limit_price: "4.70" } }
    const table = render(<OrdersView />, [walking])
    expect(table).toContain("Walk · $4.70 at 10:00:10 AM ET")
    const edit = render(<EditOrderDialog order={walking} trading={trading} onClose={() => {}} onDone={() => {}} />)
    expect(edit).toContain("Walk cap ($)")
    expect(edit).toContain('value="5.00"')
    expect(edit).toContain("No changes</button>")
  })
  it("offer an edit for resting orders and one button to cancel them all", () => {
    const html = render(<OrdersView />, [order, stop, { ...order, id: "12", status: "filled", filled_quantity: 5, remaining_quantity: 0 }])
    expect(html).toContain('aria-label="Edit order order-1 for SPX Oct 16 7000C"')
    expect(html).toContain('aria-label="Edit order 9 for SPX Oct 16 7000C"')
    expect(html).not.toContain('aria-label="Edit order 12')
    expect(html).toContain(">Cancel all</button>")
  })

  it("edit a limit order's size and price, keeping what already filled", () => {
    const html = render(<EditOrderDialog order={order} trading={trading} onClose={() => {}} onDone={() => {}} />)
    expect(html).toContain("Edit order #order-1")
    expect(html).toContain("Quantity, including 2 filled")
    expect(html).toContain('value="5"')
    expect(html).toContain("Limit price")
    expect(html).toContain('value="4.60"')
    expect(html).not.toContain("Trigger level")
    // A resting limit switches between DAY and GTC in place.
    expect(html).toContain("Time in force")
    expect(html).toContain("GTC: good until expiry")
    expect(html).toContain("No changes</button>")
  })

  it("move a stop's trigger and its size within the position, but not its time in force", () => {
    const html = render(<EditOrderDialog order={stop} trading={trading} onClose={() => {}} onDone={() => {}} />)
    expect(html).toContain("Trigger level: activates when bid ≤ $3.50")
    expect(html).toContain("A bracket exit closes at most the position it protects")
    expect(html).toContain("Quantity")
    expect(html).not.toContain("Limit price")
    expect(html).not.toContain("Time in force")
  })

  it("warn before cancelling an exit that protects a position", () => {
    const html = render(<CancelAllDialog orders={[order, stop]} trading={trading} onClose={() => {}} onDone={() => {}} />)
    expect(html).toContain("This cancels 2 open orders, armed ones included.")
    expect(html).toContain("1 bracket exit protects a position; the position stays open without it.")
    expect(html).toContain("Cancel 2 orders</button>")
  })
})

describe("closing positions", () => {
  const short: Position = { ...portfolio.positions[0]!, symbol: "SPXW  261016P06900000", type: "put", strike: 6900, quantity: -1 }

  it("is one button on the positions page", () => {
    expect(render(<PositionsView />)).toContain(">Close all</button>")
  })

  it("lists what closes, short positions first, and what is cancelled", () => {
    const html = render(<FlattenDialog positions={[...portfolio.positions, short]} orders={[order, stop]} trading={trading} onClose={() => {}} />)
    expect(html).toContain("Close all positions")
    const buy = html.indexOf("Buy 1 at market")
    const sell = html.indexOf("Sell 2 at market")
    expect(buy).toBeGreaterThan(0)
    expect(sell).toBeGreaterThan(buy)
    expect(html).toContain("1 working order is cancelled first.")
    expect(html).toContain("1 bracket exit stays until the position it protects is flat.")
    expect(html).toContain("1 expired position waits for settlement.")
    expect(html).toContain("Close 2 positions</button>")
  })

  it("can start on one underlying", () => {
    const html = render(<FlattenDialog positions={[portfolio.positions[0]!]} orders={[]} trading={trading} initial="SPX" onClose={() => {}} />)
    expect(html).toContain("Flatten SPX")
    expect(html).toContain("Close 1 position</button>")
  })

  it("closes shares with the options", () => {
    const shares: StockHolding = { symbol: "SPY", shares: 200, average_price: "520.00", basis: "104000.00", mark: "525.00",
      mark_time: "2026-09-23T15:00:00.000Z", market_value: "105000.00", unrealised: "1000.00", realised: "0.00", fees: "0.00",
      fresh: true, attribution: null }
    const html = render(<FlattenDialog positions={[portfolio.positions[0]!]} stocks={[shares]} orders={[]} trading={trading} onClose={() => {}} />)
    expect(html).toContain("200 SPY shares")
    expect(html).toContain("Sell at the price")
    expect(html).toContain("Close 2 positions</button>")
  })

  it("reports the shares it closed and the shares it left", () => {
    const closed = render(<FlattenOutcome done={{ account_version: "18", cancelled_orders: [], orders: [], fills: [],
      stock_fills: [{ id: "3", symbol: "SPY", shares: -100, price: "599.34", time: "2026-09-16T19:50:00.000Z", source: "trade", option: null }],
      kept_stocks: [] }} />)
    expect(closed).toContain("Sell 100 SPY shares")
    expect(closed).toContain("Filled at $599.34")
    expect(closed).not.toContain("No position needed closing.")
    const kept = render(<FlattenOutcome done={{ account_version: "18", cancelled_orders: [], orders: [], fills: [], stock_fills: [],
      kept_stocks: [{ symbol: "SPY", shares: 100, reason: { code: "SESSION_CLOSED", message: "Stock trades in the regular session" } }] }} />)
    expect(kept).toContain("100 SPY shares stay open: Stock trades in the regular session.")
    expect(kept).not.toContain("No position needed closing.")
    // Older servers send neither field.
    expect(render(<FlattenOutcome done={{ account_version: "18", cancelled_orders: [], orders: [], fills: [] }} />))
      .toContain("No position needed closing.")
  })

  it("says which longs it left covering a short", () => {
    const put = { ...portfolio.positions[0]!, type: "put" as const }
    const long: Position = { ...put, symbol: "SPXW  261016P06950000", strike: 6950, quantity: 5 }
    const shortPut: Position = { ...put, symbol: "SPXW  261016P06960000", strike: 6960, quantity: -5 }
    const buyBack: Order = { ...order, id: "20", symbol: shortPut.symbol, side: "buy", type: "market", time_in_force: "ioc", limit_price: null,
      quantity: 5, filled_quantity: 2, remaining_quantity: 0, average_fill_price: "5.10", status: "cancelled",
      reason: { code: "IOC_REMAINDER", message: "IOC exhausted available displayed liquidity" } }
    const done = { account_version: "18", cancelled_orders: [], fills: [], stock_fills: [], kept_stocks: [] }
    // Two shorts bought back free two longs; the other three still cover shorts.
    const partial = render(<FlattenOutcome done={{ ...done, orders: [buyBack, { ...buyBack, id: "21", symbol: long.symbol, side: "sell", quantity: 2,
      filled_quantity: 2, status: "filled", reason: null }] }} closing={[shortPut, long]} />)
    expect(partial).toContain("3 SPX Oct 16 6950P stay open: they cover a short that is still held or being bought back.")
    // With fill latency the buy-back waits for a later quote, so no long sells yet.
    const waiting = render(<FlattenOutcome done={{ ...done, orders: [{ ...buyBack, status: "working", filled_quantity: 0, reason: null }] }}
      closing={[shortPut, long]} />)
    expect(waiting).toContain("5 SPX Oct 16 6950P stay open")
    // A long closed in full leaves nothing to report.
    expect(render(<FlattenOutcome done={{ ...done, orders: [{ ...buyBack, id: "21", symbol: long.symbol, side: "sell", status: "filled" }] }}
      closing={[long]} />)).not.toContain("stay open")
  })

  it("lists the positions still open: what is being worked, and why the rest stay", () => {
    const done: ClosePositionsResponse = { account_version: "18", cancelled_orders: [], fills: [], stock_fills: [], kept_stocks: [],
      orders: [{ ...order, id: "30", symbol: null, side: null, type: "market", time_in_force: "day", limit_price: null, reduce_only: true,
        legs: [{ symbol: "SPXW  261016P06900000", side: "buy", ratio: 1 }, { symbol: "SPXW  261016P06890000", side: "sell", ratio: 1 }],
        quantity: 5, filled_quantity: 2, average_fill_price: "1.10", status: "partially_filled", reason: null }],
      residuals: [
        { symbol: "SPXW  261016P06900000", underlying: "SPX", quantity: -3, working: 3, reason: null },
        { symbol: "SPXW  261016C07000000", underlying: "SPX", quantity: 2, working: 0,
          reason: { code: "AWAITING_SETTLEMENT", message: "The contract has expired; it closes at its settlement" } },
      ] }
    const html = render(<FlattenOutcome done={done} />)
    expect(html).toContain("Close 5")
    expect(html).toContain("Filled 2 of 5 at $1.10; working the rest on later quotes")
    expect(html).toContain("still open: working on later quotes until filled or the session ends.")
    expect(html).toContain("stay open: The contract has expired; it closes at its settlement.")
    expect(html).toContain("Working closes show on the Orders page")
    expect(html).not.toContain("No position needed closing.")
    expect(html).not.toContain("they cover a short")
  })

  it("leaves an underlying whose orders are refused alone", () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!,
      paper: { accepting: false, reason: "FEED_STALLED", message: "SPX quotes are 5m behind the market; the feed appears to have stalled" } }] }, null, "open"))
    const html = render(<FlattenDialog positions={[portfolio.positions[0]!]} orders={[order, stop]} trading={trading} onClose={() => {}} />)
    expect(html).toContain("the feed appears to have stalled. Its positions and orders stay as they are.")
    expect(html).not.toContain("cancelled first")
    expect(html).toMatch(/disabled="">Close 1 position<\/button>/)
  })

  it("waits for the regular session, since it sends market orders", () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!,
      session: { name: "global", open: true, note: "overnight session" } }] }, null, "open"))
    const html = render(<FlattenDialog positions={[portfolio.positions[0]!]} orders={[]} trading={trading} initial="SPX" onClose={() => {}} />)
    expect(html).toContain("SPX is outside the regular session, which takes limit orders only.")
    expect(html).toMatch(/disabled="">Close 1 position<\/button>/)
  })
})

it("shows the originating actor and marks older orders unknown", () => {
  expect(render(<OrdersView />, [{ ...order, actor: "research-agent" }])).toContain("research-agent")
  expect(render(<OrdersView />, [{ ...order, actor: undefined }])).toContain("unknown")
})

describe("order details", () => {
  it("say what a working order waits for and offer details from the list", () => {
    const waiting: Order = { ...order, waiting: { code: "LIMIT", message: "The ask 4.70 is above the limit 4.60" }, changes: [] }
    const html = render(<OrderDetailDialog order={waiting} onClose={() => {}} />)
    expect(html).toContain("Waiting for")
    expect(html).toContain("The ask 4.70 is above the limit 4.60")
    expect(html).toContain("Accepted")
    expect(html).toContain("Lasts until")
    expect(render(<OrdersView />, [waiting])).toContain('aria-label="Details of order order-1')
  })
  it("show a cancel's numbers", () => {
    const cancelled: Order = { ...order, status: "cancelled", ended_at: "2026-09-22T15:00:00Z",
      reason: { code: "RISK_CHANGED", message: "DELTA_LIMIT: Worst reachable absolute dollar delta exceeds limit", actual: 878617, limit: 700000, scope: "SPX" } }
    const html = render(<OrderDetailDialog order={cancelled} onClose={() => {}} />)
    expect(html).toContain("878,617 dollar delta against a limit of 700,000 dollar delta · SPX")
    expect(html).toContain("Cancelled (RISK_CHANGED)")
  })
})

describe("flatten dry run", () => {
  const symbol = portfolio.positions[0]!.symbol
  const account = (equity: string): WhatIfAccount => ({ equity, buying_power: "9978.70", exposure: null, max_loss: null, equity_at_max_loss: null,
    breaches_floor: null, breaches_soft_floor: null, scenarios: { spot_percent: [], vol_points: [], pnl: [], complete: false },
    breach: { room: null, soft_room: null, complete: true, model: "reflection estimate", underlyings: [] } })
  const preview: FlattenPreview = { account_version: "17", decision: "ok", reason: null, cancelled_orders: ["3"],
    orders: [{ symbol, underlying: "SPX", side: "sell", legs: null, quantity: 2, filled_quantity: 2, average_fill_price: "4.00", status: "filled", reason: null },
      { symbol: "SPX   261022P04900000", underlying: "SPX", side: "buy", legs: null, quantity: 1, filled_quantity: 0, average_fill_price: null, status: "working", reason: null }],
    fills: [], stock_fills: [], kept_stocks: [], remaining: [{ symbol: "SPX   261022P04900000", underlying: "SPX", quantity: -1 }], remaining_shares: [],
    current: account("10000.00"), after: account("9978.70"), simulated: true }
  it("shows what fills now, what waits, what stays and the account after it", () => {
    const html = render(<FlattenDryRun preview={{ data: preview, error: null, isFetching: false }} />)
    expect(html).toContain("Simulated dry run at the current quotes")
    expect(html).toContain("Sell 2 SPX")
    expect(html).toContain("at $4.00")
    expect(html).toContain("1 close waits for a later quote")
    expect(html).toContain("$9,978.70")
    expect(html).toContain("−$21.30")
    expect(html).toContain("Still held: -1 SPX")
  })
  it.each(["3.80", "-3.80"])("names a spread's legs and signed net average %s", (average) => {
    const combo: FlattenPreview["orders"][number] = { ...preview.orders[0]!, symbol: null, side: null,
      legs: [{ symbol: "SPXW  260917P06000000", side: "buy", ratio: 1 },
        { symbol: "SPXW  260917P05990000", side: "sell", ratio: 1 }], average_fill_price: average }
    const html = render(<FlattenDryRun preview={{ data: { ...preview, orders: [combo] }, error: null, isFetching: false }} />)
    expect(html).toContain(`Close 2 SPX Sep 17 +6000P −5990P at ${average.startsWith("-") ? "−" : ""}$3.80 net`)
    const refused = render(<FlattenDryRun preview={{ data: { ...preview, orders: [{ ...combo,
      filled_quantity: 0, average_fill_price: null, status: "rejected",
      reason: { code: "FEED_STALLED", message: "Feed stalled", actual: null, limit: null, scope: "SPX" } }] }, error: null, isFetching: false }} />)
    expect(refused).toContain("SPX Sep 17 +6000P −5990P: Feed stalled")
  })
  it("names a refusal, and says when the dry run is unavailable", () => {
    const refused = render(<FlattenDryRun preview={{ data: { ...preview, decision: "FEED_STALLED", reason: { code: "FEED_STALLED", message: "Feed stalled", actual: null, limit: null, scope: "SPX" } },
      error: null, isFetching: false }} />)
    expect(refused).toContain("The flatten would be refused: Feed stalled.")
    expect(render(<FlattenDryRun preview={{ error: new Error("x"), isFetching: false }} />)).toContain("The dry run failed")
  })
})

it("shows playbook time-stop cancellations and the automatic close note", () => {
  const cancelled: Order = { ...stop, status: "cancelled", parent: fill.order_id,
    reason: { code: "PLAYBOOK_TIME_STOP", message: "Playbook time stop", actual: null, limit: null, scope: null } }
  const close: Order = { ...order, id: fill.order_id, note: "Playbook automatic time stop; entry 1" }
  const detail = render(<OrderDetailDialog order={cancelled} onClose={() => {}} />)
  expect(detail).toContain("PLAYBOOK_TIME_STOP")
  expect(detail).toContain("Playbook time stop")
  expect(detail).not.toContain("Cancelled by you")
  expect(render(<OrdersView />, [close])).toContain(close.note)
  const journal = render(<TradeDetail trade={trades[0]!} fills={[fill]} trading={trading} orders={[cancelled, close]} />)
  expect(journal).toContain("PLAYBOOK_TIME_STOP")
  expect(journal).toContain(close.note)
})

it.each(["TRAILING_STOP", "DTE_STOP", "DAYS_IN_TRADE_STOP"])("shows new playbook cancellation %s", code => {
  const cancelled: Order = { ...stop, status: "cancelled", parent: fill.order_id,
    reason: { code: `PLAYBOOK_${code}`, message: "Playbook exit", actual: null, limit: null, scope: null } }
  const journal = render(<TradeDetail trade={trades[0]!} fills={[fill]} trading={trading} orders={[cancelled]} />)
  expect(journal).toContain(`PLAYBOOK_${code}`)
  expect(journal).toContain("Playbook exit")
})
