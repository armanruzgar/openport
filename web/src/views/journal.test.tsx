// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, type ReactNode } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import { coachFlags, orderLabel, osiLabel, sizeMedians, tradeBuckets } from "../lib/journal"
import { tradeGroups } from "../lib/positions"
import { fill, order, status, trades } from "../test/trading-fixtures"
import { JournalView } from "./JournalView"
import { OrdersView } from "./OrdersView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../lib/journal", async (original) => {
  const actual = await original<typeof import("../lib/journal")>()
  return { ...actual, tradeBuckets: vi.fn(actual.tradeBuckets), orderLabel: vi.fn(actual.orderLabel), osiLabel: vi.fn(actual.osiLabel) }
})
vi.mock("../lib/positions", async (original) => {
  const actual = await original<typeof import("../lib/positions")>()
  return { ...actual, tradeGroups: vi.fn(actual.tradeGroups) }
})

let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.stubGlobal("fetch", vi.fn(() => new Promise(() => {})))
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  const queries = tradingQueries(0, "17", true)
  client.setQueryData(queries.trades("current").queryKey, { account_version: "17", attempt: 2, trades })
  client.setQueryData(queries.fills.queryKey, { account_version: "17", fills: [fill] })
  client.setQueryData(queries.allOrders.queryKey, { account_version: "17", orders: [order, { ...order, id: "order-2", status: "filled" as const }] })
  client.setQueryData(queries.settlements.queryKey, { account_version: "17", settlements: [] })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); client.clear(); vi.clearAllMocks(); vi.unstubAllGlobals() })

const render = (node: ReactNode) => act(async () => root.render(<QueryClientProvider client={client}>{node}</QueryClientProvider>))
/** What a live tick does to these pages: a new live value with the same account in it. */
const tick = async (node: ReactNode) => {
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  await render(node)
}

describe("journal and orders on live ticks", () => {
  it("groups, filters and reports the trades once per change, not once per tick", async () => {
    await render(<JournalView />)
    const select = host.querySelector<HTMLSelectElement>('select[aria-label="Group by"]')!
    await act(async () => { select.value = "coach"; select.dispatchEvent(new Event("change", { bubbles: true })) })
    expect(host.textContent).toContain("Net P&L by coach flag")
    const reports = vi.mocked(tradeBuckets).mock.calls.length
    const groups = vi.mocked(tradeGroups).mock.calls.length
    expect(reports).toBeGreaterThan(0)
    for (let i = 0; i < 5; i++) await tick(<JournalView />)
    expect(vi.mocked(tradeBuckets).mock.calls.length).toBe(reports)
    expect(vi.mocked(tradeGroups).mock.calls.length).toBe(groups)
  })

  it("searches the orders and fills once per change, not once per tick", async () => {
    await render(<OrdersView />)
    const search = host.querySelector<HTMLInputElement>('input[aria-label="Search orders"]')!
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(search, "nothing like this")
      search.dispatchEvent(new Event("input", { bubbles: true }))
    })
    // Nothing matches, so no row renders a label: the searches alone ask for them.
    expect(host.querySelector('[role="region"][aria-label="Orders"]')).toBeNull()
    const orders = vi.mocked(orderLabel).mock.calls.length, fills = vi.mocked(osiLabel).mock.calls.length
    expect(orders).toBeGreaterThan(0)
    expect(fills).toBeGreaterThan(0)
    for (let i = 0; i < 5; i++) await tick(<OrdersView />)
    expect(vi.mocked(orderLabel).mock.calls.length).toBe(orders)
    expect(vi.mocked(osiLabel).mock.calls.length).toBe(fills)
  })

  it("flags oversized trades from one pass over the peers", () => {
    const closed = trades.filter((t) => t.status === "closed")
    const big = { ...closed[0]!, id: "big", max_quantity: 50 }
    const peers = [big, ...closed, ...closed.map((t) => ({ ...t, id: `${t.id}-copy` }))]
    const medians = sizeMedians(peers)
    for (const trade of peers) expect(coachFlags(trade, peers, {}, medians)).toEqual(coachFlags(trade, peers))
    expect(coachFlags(big, peers, {}, medians)).toContain("Size >2× median")
  })
})
