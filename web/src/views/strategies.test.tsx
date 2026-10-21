import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { Order, Position } from "../api/trading-types"
import { account, fill, order, portfolio, risk, status, trades } from "../test/trading-fixtures"
import { JournalView } from "./JournalView"
import { PositionsView } from "./PositionsView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const clients: QueryClient[] = []
function render(node: ReactNode, positions: Position[], orders: Order[]) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(client)
  const queries = tradingQueries(0, "17", true)
  client.setQueryData(queries.portfolio.queryKey, { ...portfolio, positions })
  client.setQueryData(queries.orders.queryKey, { account_version: "17", orders: [] })
  client.setQueryData(queries.allOrders.queryKey, { account_version: "17", orders })
  client.setQueryData(queries.fills.queryKey, { account_version: "17", fills: [fill] })
  client.setQueryData(queries.risk.queryKey, risk)
  client.setQueryData(queries.account.queryKey, account)
  client.setQueryData(queries.trades("current").queryKey, { account_version: "17", attempt: 1, trades })
  return renderToStaticMarkup(<QueryClientProvider client={client}>{node}</QueryClientProvider>)
}
beforeEach(() => vi.mocked(useLive).mockReturnValue(liveState(status, null, "open")))
afterEach(() => { clients.splice(0).forEach((client) => client.clear()); vi.clearAllMocks() })

const base = portfolio.positions[0]!
const put = (strike: number, quantity: number, mark: string): Position =>
  ({ ...base, symbol: `SPXW  261016P0${strike}000`, type: "put", strike, quantity, mark })
const spread: Order = { ...order, id: "7", symbol: null, side: null, status: "filled", quantity: 2, filled_quantity: 2, remaining_quantity: 0,
  limit_price: "-1.20", average_fill_price: "-1.20", accepted_at: "2026-09-23T15:00:00Z",
  legs: [{ symbol: "SPXW  261016P06900000", side: "sell", ratio: 1 }, { symbol: "SPXW  261016P06890000", side: "buy", ratio: 1 }] }

describe("strategies on the positions page", () => {
  it("show a spread as one row with its P&L, close and roll", () => {
    const html = render(<PositionsView />, [put(6900, -2, "3.00"), put(6890, 2, "2.50"), base], [spread])
    expect(html).toContain("Strategies · 1")
    expect(html).toContain("Bull put spread")
    expect(html).toContain("SPX Oct 16 6900/6890 P · #7")
    expect(html).toContain("$1.20 cr")
    expect(html).toContain("$0.50 cr")
    expect(html).toContain("+$140.00")
    expect(html).toContain('aria-label="Close Bull put spread SPX Oct 16 6900/6890 P"')
    expect(html).toContain('aria-label="Roll Bull put spread SPX Oct 16 6900/6890 P"')
    // Each leg notes the strategy it belongs to; the call stays on its own.
    expect(html.match(/In bull put spread #7/g)?.length).toBe(2)
  })

  it("stay out of the way without multi-leg orders", () => {
    expect(render(<PositionsView />, [base], [order])).not.toContain("Strategies ·")
  })
})

describe("strategies in the journal", () => {
  it("can group the trade history by strategy", () => {
    const html = render(<JournalView />, [], [spread])
    expect(html).toContain('aria-label="Group trades"')
    expect(html).toContain(">Strategies</button>")
  })
})

describe("notes and tags in the journal", () => {
  it("shows each trade's tags, filters by tag and reports by tag", () => {
    const tagged = trades.map((t) => t.id === "2" ? { ...t, tags: ["breakout"], note: "Chased the open" } : t)
    const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
    clients.push(client)
    const queries = tradingQueries(0, "17", true)
    client.setQueryData(queries.fills.queryKey, { account_version: "17", fills: [fill] })
    client.setQueryData(queries.allOrders.queryKey, { account_version: "17", orders: [] })
    client.setQueryData(queries.trades("current").queryKey, { account_version: "17", attempt: 1, trades: tagged })
    const html = renderToStaticMarkup(<QueryClientProvider client={client}><JournalView /></QueryClientProvider>)
    expect(html).toContain('aria-label="Tag"')
    expect(html).toContain('<option value="breakout">breakout</option>')
    expect(html).toContain(">breakout</span>")
    expect(html).toContain('title="Chased the open">note</span>')
    expect(html).toContain('<option value="tag">Tag</option>')
    expect(html).toContain("Mistake cost comparison")
  })
})
