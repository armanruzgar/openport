// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api, ApiError } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { UnderlyingSubscription } from "../api/types"
import { barDay } from "../lib/candles"
import { status, summary } from "../test/trading-fixtures"
import { subscribableSymbol, SymbolSearch, Watchlist } from "./Watchlist"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
let host: HTMLDivElement, root: Root, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  HTMLDialogElement.prototype.showModal = function () { this.open = true }
  HTMLDialogElement.prototype.close = function () { this.open = false }
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  delete (HTMLElement.prototype as { scrollIntoView?: unknown }).scrollIntoView
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})
const spx = status.underlyings[0]!
const at = (version: number) => vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...spx, version }] }, null, "open"))

it("keeps a card's figures while the next data version loads, and asks for the prior close once a day", async () => {
  const summaries = vi.spyOn(api, "summary").mockReturnValue(new Promise<never>(() => {}))
  const volatility = vi.spyOn(api, "volatility").mockReturnValue(new Promise<never>(() => {}))
  const closes = vi.spyOn(api, "candles").mockReturnValue(new Promise<never>(() => {}))
  client.setQueryData(["summary", "SPX", 1], summary)
  client.setQueryData(["volatility", "SPX", 1], { symbol: "SPX", iv_rank: .25, proxy: { used: false, name: null } })
  client.setQueryData(["watch-close", "SPX", barDay(Date.parse(spx.as_of!) / 1000)],
    { symbol: "SPX", interval: "1d", bars: [{ t: Date.parse("2026-09-22T20:00:00Z") / 1000, o: 6900, h: 6900, l: 6900, c: 6900 }] })
  const draw = () => act(async () => root.render(<QueryClientProvider client={client}><Watchlist onSymbol={() => {}} /></QueryClientProvider>))
  at(1)
  await draw()
  await act(async () => host.querySelector("button")!.click())
  const figures = ["+1.45%", "25.0%", "351.44"]
  for (const text of figures) expect(host.textContent).toContain(text)
  at(2)
  await draw()
  expect(summaries).toHaveBeenCalledTimes(1)
  expect(volatility).toHaveBeenCalledTimes(1)
  // The prior day's close is the same all day: no request for it on a version bump.
  expect(closes).not.toHaveBeenCalled()
  for (const text of figures) expect(host.textContent).toContain(text)
})

it("scrolls the highlighted symbol into view and keeps the highlight on a row when the list shrinks", async () => {
  const scrolled: string[] = []
  Object.defineProperty(HTMLElement.prototype, "scrollIntoView", { configurable: true, value(this: HTMLElement) { scrolled.push(this.id) } })
  const symbols = Array.from({ length: 20 }, (_, i) => `S${String(i).padStart(2, "0")}`)
  const subscribe = (list: string[]) => vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: list.map((symbol) => ({ ...spx, symbol })) }, null, "open"))
  const chosen = vi.fn()
  const draw = () => act(async () => root.render(<SymbolSearch onSymbol={chosen} onClose={() => {}} />))
  subscribe(symbols)
  await draw()
  const input = host.querySelector("input")!
  const press = (key: string) => act(async () => { input.dispatchEvent(new KeyboardEvent("keydown", { key, bubbles: true, cancelable: true })) })
  for (let i = 0; i < 19; i++) await press("ArrowDown")
  expect(input.getAttribute("aria-activedescendant")).toBe("symbol-result-19")
  expect(scrolled.at(-1)).toBe("symbol-result-19")
  subscribe(symbols.slice(0, 5))
  await draw()
  expect(input.getAttribute("aria-activedescendant")).toBe("symbol-result-4")
  expect(host.querySelector('[aria-selected="true"]')?.id).toBe("symbol-result-4")
  await press("Enter")
  expect(chosen).toHaveBeenCalledWith("S04")
})

// G-11: a symbol the server does not carry can be subscribed while it runs.
it("subscribes to a new symbol from search and explains a refused removal or a fixed source", async () => {
  expect(subscribableSymbol(" nvda ")).toBe("NVDA")
  expect(subscribableSymbol("BRK.B")).toBe("BRK.B")
  for (const bad of ["", ".X", "NV DA", "SPX$", "A".repeat(17)]) expect(subscribableSymbol(bad)).toBeNull()
  const carried: UnderlyingSubscription = { underlyings: ["SPX", "AAPL"], configured: ["SPX"], added: ["AAPL"], live: true, reason: null, limit: 32 }
  vi.spyOn(api, "underlyingSubscription").mockResolvedValue(carried)
  const subscribe = vi.spyOn(api, "subscribeUnderlying").mockResolvedValue({ ...carried, underlyings: ["SPX", "AAPL", "NVDA"], added: ["AAPL", "NVDA"] })
  const unsubscribe = vi.spyOn(api, "unsubscribeUnderlying").mockRejectedValue(
    new ApiError(409, "AAPL has positions, working orders or alerts in main; close, cancel or delete them first", "UNDERLYING_IN_USE"))
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  const chosen = vi.fn(), closed = vi.fn()
  await act(async () => root.render(<SymbolSearch onSymbol={chosen} onClose={closed} />))
  const input = host.querySelector("input")!
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(input, "nvda")
    input.dispatchEvent(new Event("input", { bubbles: true }))
  })
  expect(host.textContent).toContain("No subscribed symbol matches.")
  expect([...host.querySelectorAll("button")].some((b) => b.textContent === "Subscribe to NVDA")).toBe(true)
  await act(async () => { input.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true, cancelable: true })) })
  expect(subscribe).toHaveBeenCalledWith("NVDA", "open")
  expect(chosen).toHaveBeenCalledWith("NVDA")
  expect(closed).toHaveBeenCalledOnce()

  await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Unsubscribe from AAPL"]')!.click())
  expect(unsubscribe).toHaveBeenCalledWith("AAPL", "open")
  expect(host.querySelector('[role="alert"]')?.textContent).toContain("AAPL has positions")

  // A replay or the demo market plays fixed underlyings: no offer, and the reason.
  await act(async () => root.unmount())
  root = createRoot(host)
  vi.mocked(api.underlyingSubscription).mockResolvedValue({ ...carried, added: [], live: false,
    reason: "The demo market generates its own underlyings and cannot add others" })
  await act(async () => root.render(<SymbolSearch onSymbol={chosen} onClose={closed} />))
  const fixed = host.querySelector("input")!
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(fixed, "NVDA")
    fixed.dispatchEvent(new Event("input", { bubbles: true }))
  })
  expect([...host.querySelectorAll("button")].some((b) => b.textContent?.startsWith("Subscribe"))).toBe(false)
  expect(host.textContent).toContain("The demo market generates its own underlyings and cannot add others.")
})
