// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { Position } from "../api/trading-types"
import { account, portfolio, risk, status } from "../test/trading-fixtures"
import { PositionsView } from "./PositionsView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
let root: Root, host: HTMLDivElement, client: QueryClient
const queries = tradingQueries(0, "17", true)
const base = portfolio.positions[0]!
const put = (strike: number, quantity: number): Position => ({ ...base, symbol: `SPXW  261016P0${strike}000`, type: "put", strike, quantity })
const legs = [put(6900, -2), put(6890, 2), base]
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.stubGlobal("fetch", vi.fn(() => new Promise(() => {})))
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  client.setQueryData(queries.portfolio.queryKey, { ...portfolio, positions: legs })
  client.setQueryData(queries.orders.queryKey, { account_version: "17", orders: [] })
  client.setQueryData(queries.allOrders.queryKey, { account_version: "17", orders: [] })
  client.setQueryData(queries.risk.queryKey, risk)
  client.setQueryData(queries.account.queryKey, account)
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); client.clear(); vi.restoreAllMocks(); vi.unstubAllGlobals() })

it("drops picked positions that close or await settlement", async () => {
  await act(async () => root.render(<QueryClientProvider client={client}><PositionsView /></QueryClientProvider>))
  const boxes = () => [...host.querySelectorAll<HTMLInputElement>('input[aria-label^="Pick "]')]
  const together = () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Close together")!
  for (const box of boxes().slice(0, 2)) await act(async () => box.click())
  expect(host.textContent).toContain("2 picked")
  expect(together().disabled).toBe(false)
  // The query cache notifies observers on a timer.
  await act(async () => {
    client.setQueryData(queries.portfolio.queryKey, { ...portfolio, positions: [{ ...legs[0]!, awaiting_settlement: true }, legs[2]!] })
    await new Promise((resolve) => setTimeout(resolve, 0))
  })
  expect(host.textContent).not.toContain("1 picked")
  expect(host.textContent).not.toContain("2 picked")
  expect(together().disabled).toBe(true)
  expect(boxes().some((box) => box.checked)).toBe(false)
})
