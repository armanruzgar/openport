// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import { loadChartPrefs } from "../lib/candles"
import { expiry, portfolio, status } from "../test/trading-fixtures"
import { PriceChart } from "./PriceChart"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../charts/useSize", () => ({ useSize: () => [{ current: null }, { width: 700, height: 250 }] }))
const open = Date.UTC(2026, 8, 22, 13, 30) / 1000
const bars = Array.from({ length: 5 }, (_, i) => ({ t: open + i * 300, o: 7000 + i, h: 7001 + i, l: 6999 + i, c: 7000.5 + i }))
let host: HTMLDivElement, root: Root, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.stubGlobal("fetch", vi.fn(() => new Promise(() => {})))
  localStorage.clear()
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity } } })
  client.setQueryData(tradingQueries(0, "17", true).portfolio.queryKey, portfolio)
  client.setQueryData(tradingQueries(0, "17", true).orders.queryKey, { account_version: "17", orders: [] })
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear(); localStorage.clear()
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})
async function render(candles: { t: number; o: number; h: number; l: number; c: number; v?: number }[]) {
  client.setQueryData(["candles", "SPX", "5m", 1], { symbol: "SPX", interval: "5m", bars: candles })
  await act(async () => root.render(<QueryClientProvider client={client}><PriceChart symbol="SPX" spot={7002} expiry={expiry} /></QueryClientProvider>))
}
const studyNames = () => [...host.querySelectorAll("label")].filter((label) => label.querySelector('input[type="checkbox"]') && !label.textContent?.includes("Brief"))
  .map((label) => label.textContent)
const period = () => host.querySelector<HTMLInputElement>('input[aria-label="Study period"]')!
async function typePeriod(value: string) {
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(period(), value)
    period().dispatchEvent(new Event("input", { bubbles: true }))
  })
}

it("offers VWAP only once the bars carry volume", async () => {
  await render(bars)
  expect(studyNames()).toEqual(["SMA", "EMA", "Bollinger ±2σ"])
  expect(host.textContent).not.toContain("VWAP")
  await render(bars.map((bar) => ({ ...bar, v: 1000 })))
  expect(studyNames()).toEqual(["SMA", "EMA", "Bollinger ±2σ", "VWAP"])
})

it("keeps the study period's keystrokes until they make a whole 2–200 period", async () => {
  await render(bars)
  expect(period().value).toBe("20")
  await typePeriod("")
  expect(period().value).toBe("")
  await typePeriod("1")
  expect(period().value).toBe("1")
  expect(period().getAttribute("aria-invalid")).toBe("true")
  expect(loadChartPrefs().studies?.period ?? 20).toBe(20)
  await typePeriod("14")
  expect(period().value).toBe("14")
  expect(period().getAttribute("aria-invalid")).toBe("false")
  expect(loadChartPrefs().studies?.period).toBe(14)
  await typePeriod("1")
  await act(async () => period().dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true })))
  expect(period().value).toBe("14")
})
