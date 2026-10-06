// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, type ReactNode } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { useFills, useTrades } from "../api/trading"
import type { DividendPaid, Fill, StockFill } from "../api/trading-types"
import { alertStore, noAlerts } from "../lib/alerts"
import { toasts } from "../lib/notify"
import { fill, status } from "../test/trading-fixtures"
import { AlertsDialog, AlertWatcher, Toasts } from "./Alerts"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../api/trading", async (original) => ({ ...await original<typeof import("../api/trading")>(), useFills: vi.fn(), useTrades: vi.fn() }))

let root: Root
let host: HTMLDivElement
const client = new QueryClient()
const shown: { title: string; body?: string }[] = []
class FakeNotification {
  static permission: NotificationPermission = "granted"
  static requestPermission = vi.fn(async () => "granted" as NotificationPermission)
  constructor(title: string, options?: { body?: string }) { shown.push({ title, body: options?.body }) }
}
function live(spot: number, source: "live" | "replay" = "live", scope = 0) {
  const underlyings = [{ ...status.underlyings[0]!, spot }]
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings }, null, "open", scope, "main", () => {}, source))
}
function fills(list: Fill[] | undefined) {
  vi.mocked(useFills).mockReturnValue({ data: list ? { account_version: "1", fills: list } : undefined } as ReturnType<typeof useFills>)
}
function deliveries(stock_fills: StockFill[] | undefined, dividends: DividendPaid[] = []) {
  vi.mocked(useTrades).mockReturnValue({ data: stock_fills ? { account_version: "1", attempt: 1, trades: [], stock_fills, dividends } : undefined } as ReturnType<typeof useTrades>)
}
const exercised: StockFill = { id: "1", symbol: "SPY", shares: 100, price: "501.00", time: "2026-09-22T20:15:00Z", source: "expiry_exercise", option: "SPY   260922C00500000" }
const sold: StockFill = { id: "2", symbol: "SPY", shares: -100, price: "503.00", time: "2026-09-23T14:00:00Z", source: "trade", option: null }
const assigned: StockFill = { id: "3", symbol: "QQQ", shares: 200, price: "480.00", time: "2026-09-23T21:30:00Z", source: "assignment", option: "QQQ   261016P00490000" }
const dividend: DividendPaid = { symbol: "QQQ", ex_date: "2026-09-24", per_share: "0.70", shares: 200, amount: "140.00", time: "2026-09-23T21:30:00Z" }
async function render(node: ReactNode) {
  await act(async () => root.render(<QueryClientProvider client={client}>{node}<Toasts /></QueryClientProvider>))
}
const titles = () => toasts.get().map((t) => t.title)

beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.stubGlobal("Notification", FakeNotification)
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  host = document.createElement("div")
  document.body.append(host)
  root = createRoot(host)
  alertStore.set(noAlerts)
  fills(undefined)
  deliveries(undefined)
})
afterEach(async () => {
  await act(async () => root.unmount())
  host.remove()
  for (const toast of toasts.get()) toasts.dismiss(toast.id)
  shown.length = 0
  vi.unstubAllGlobals()
  vi.clearAllMocks()
})

describe("the alert watcher", () => {
  it("announces assignments, exercises at expiry and dividends once, but not history or trades", async () => {
    live(5000)
    deliveries([exercised])
    await render(<AlertWatcher />)
    expect(titles()).toEqual([])
    deliveries([exercised, sold, assigned], [dividend])
    await render(<AlertWatcher />)
    expect(titles()).toEqual(["Assigned QQQ Oct 16 490P", "QQQ dividend"])
    expect(shown).toEqual([{ title: "Assigned QQQ Oct 16 490P", body: "Bought 200 QQQ at $480.00" },
      { title: "QQQ dividend", body: "+$140.00 on 200 shares at $0.70" }])
    deliveries([exercised, sold, assigned], [dividend])
    await render(<AlertWatcher />)
    expect(titles()).toHaveLength(2)
    // Without fill alerts on, and whatever the account switch, only new ones speak.
    live(5000, "live", 1)
    deliveries([exercised, sold, assigned, { ...assigned, id: "4" }], [dividend])
    await render(<AlertWatcher />)
    expect(titles()).toHaveLength(2)
  })

  it("announces a delivery shortfall once even with fill alerts off", async () => {
    alertStore.set({ ...noAlerts, fills: false, sound: false })
    live(5000)
    deliveries([])
    await render(<AlertWatcher />)
    const fill = { ...exercised, buying_power_after: "-83000.000001" }
    deliveries([fill])
    await render(<AlertWatcher />)
    expect(shown).toHaveLength(1)
    expect(shown[0]!.body).toContain("Buying power after delivery: −$83,000.000001")
    expect(shown[0]!.body).toContain("closes that free buying power remain allowed")
    deliveries([fill])
    await render(<AlertWatcher />)
    expect(shown).toHaveLength(1)
  })

  it("fires a price alert once, in the page and as a browser notification", async () => {
    alertStore.set({ fills: false, sound: false, prices: [
      { id: "up", symbol: "SPX", direction: "above", level: 5000, created: "2026-09-23T19:00:00.000Z" },
      { id: "down", symbol: "SPX", direction: "below", level: 4900, created: "2026-09-23T19:00:00.000Z" }] })
    live(4990)
    await render(<AlertWatcher />)
    expect(titles()).toEqual([])
    live(5001)
    await render(<AlertWatcher />)
    expect(titles()).toEqual(["SPX rose to 5,000.00"])
    expect(shown).toEqual([{ title: "SPX rose to 5,000.00", body: "Now 5,001.00." }])
    expect(alertStore.get().prices.map((a) => a.id)).toEqual(["down"])
    expect(host.textContent).toContain("SPX rose to 5,000.00")
  })

  it("ignores a replay's prices", async () => {
    alertStore.set({ ...noAlerts, prices: [{ id: "up", symbol: "SPX", direction: "above", level: 5000, created: "2026-09-23T19:00:00.000Z" }] })
    live(5100, "replay")
    await render(<AlertWatcher />)
    expect(titles()).toEqual([])
    expect(alertStore.get().prices).toHaveLength(1)
  })

  it("notifies fills after the first list, and starts over on another account", async () => {
    alertStore.set({ ...noAlerts, fills: true, sound: false })
    live(4990)
    fills([{ ...fill, id: "1" }])
    await render(<AlertWatcher />)
    expect(titles()).toEqual([])
    fills([{ ...fill, id: "1" }, { ...fill, id: "2", side: "buy", quantity: 3 }])
    await render(<AlertWatcher />)
    expect(titles()).toEqual(["Order filled"])
    expect(toasts.get()[0]!.body).toMatch(/^Bought 3 /)
    live(4990, "live", 1)
    fills([{ ...fill, id: "9" }])
    await render(<AlertWatcher />)
    expect(titles()).toEqual(["Order filled"])
  })
})

describe("the alerts dialog", () => {
  it("adds and removes price alerts and turns fill alerts on", async () => {
    live(4990)
    await render(<AlertsDialog initial={{ symbol: "SPX", level: 4990 }} onClose={() => {}} />)
    const input = host.querySelector<HTMLInputElement>('input[inputmode="decimal"]')!
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(input, "5010")
      input.dispatchEvent(new Event("input", { bubbles: true }))
    })
    expect(host.textContent).toContain("Alerts once when SPX rises to 5,010.00, from 4990.00 now.")
    await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
    expect(alertStore.get().prices).toMatchObject([{ symbol: "SPX", direction: "above", level: 5010 }])
    expect(host.textContent).toContain("Browser notifications are on")
    await act(async () => host.querySelector<HTMLInputElement>('[aria-label="Fill alerts"]')!.click())
    expect(alertStore.get().fills).toBe(true)
    await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Remove alert: SPX rises to 5,010.00"]')!.click())
    expect(alertStore.get().prices).toEqual([])
  })
})
