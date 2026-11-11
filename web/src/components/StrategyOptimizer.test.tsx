// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import type { StrategySearch } from "../api/types"
import { waitForRender } from "../test/render"
import { expiry } from "../test/trading-fixtures"
import { StrategyOptimizer } from "./StrategyOptimizer"

const search: StrategySearch = {
  symbol: "SPX", as_of: "2026-09-23T15:30:00Z", spot: 7000, target_price: 7000, target_date: "2026-10-16", target_time: "2026-10-16T20:00:00Z",
  budget: 1000, vol_points: 0, rank: "expected", price: "natural", structures: ["vertical"], expiries: [expiry.id], skipped: [],
  considered: 12, evaluated: 12, unbounded: 0, over_budget: 3, within_budget: 1, truncated: false, max_candidates: 5000, variance_fallback: false,
  label: "Derived estimates", model: "Fixture model",
  candidates: [{ name: "Bull call 7050/7100 Oct 16", structure: "vertical", shares: 0, net: 10.5, max_loss: 1050, max_profit: 3950, breakevens: [7060.5],
    pop: { risk_neutral: 0.41, user: 0.47 }, expected: { risk_neutral: -50, user: 25 }, return_on_risk: 0.024, quantity: 1, valued_at: "2026-10-16T20:00:00Z",
    sigma: { risk_neutral: 0.05, user: 0.05 },
    legs: [{ symbol: "SPXW  261016C07050000", side: "buy", ratio: 1, type: "call", strike: 7050, expiry: expiry.id, bid: 50, ask: 51, mid: 50.5, iv: .2 },
      { symbol: "SPXW  261016C07100000", side: "sell", ratio: 1, type: "call", strike: 7100, expiry: expiry.id, bid: 40, ask: 41, mid: 40.5, iv: .2 }] }],
}

let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity } } })
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})
const draw = (symbol: string, spot: number) => act(async () => root.render(<QueryClientProvider client={client}>
  <StrategyOptimizer symbol={symbol} spot={spot} expiries={[expiry]} onAnalyze={() => {}} onTicket={() => {}} />
</QueryClientProvider>))
const searchButton = () => [...host.querySelectorAll<HTMLButtonElement>('button[type="submit"]')][0]!

it("searches again on the same terms and says which quotes the results used", async () => {
  const optimizer = vi.spyOn(api, "optimizer").mockResolvedValue(search)
  await draw("SPX", 7000)
  await act(async () => searchButton().click())
  await waitForRender(() => expect(host.textContent).toContain("Bull call 7050/7100 Oct 16"))
  expect(host.textContent).toContain("quotes as of 11:30:00 ET")
  await waitForRender(() => expect(searchButton().disabled).toBe(false))
  await act(async () => searchButton().click())
  await waitForRender(() => expect(optimizer).toHaveBeenCalledTimes(2))
  expect(optimizer.mock.calls[1]![1]).toEqual(optimizer.mock.calls[0]![1])
})

it("starts a new form for another symbol instead of searching it with the old target", async () => {
  const optimizer = vi.spyOn(api, "optimizer").mockResolvedValue(search)
  await draw("SPX", 7000)
  const covered = () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Covered calls")!
  expect(covered().getAttribute("aria-pressed")).toBe("false")
  await act(async () => searchButton().click())
  await waitForRender(() => expect(host.textContent).toContain("Bull call 7050/7100 Oct 16"))
  await draw("SPY", 600)
  expect(host.querySelector<HTMLInputElement>('[aria-label="Target price"]')!.value).toBe("600.00")
  expect(covered().getAttribute("aria-pressed")).toBe("true")
  expect(host.textContent).not.toContain("Bull call 7050/7100 Oct 16")
  expect(optimizer).toHaveBeenCalledTimes(1)
  expect(optimizer.mock.calls[0]![0]).toBe("SPX")
})
