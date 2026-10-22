// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import { order, status } from "../test/trading-fixtures"
import { waitForRender } from "../test/render"
import { OrdersView } from "./OrdersView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  const keys = tradingQueries(0, "17", true)
  client.setQueryData(keys.allOrders.queryKey, { account_version: "17", orders: [order] })
  client.setQueryData(keys.fills.queryKey, { account_version: "17", fills: [] })
  vi.spyOn(api, "orders").mockResolvedValue({ account_version: "18", orders: [{ ...order, status: "cancelled" }] })
  vi.spyOn(api, "fills").mockResolvedValue({ account_version: "18", fills: [] })
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); client.clear(); vi.restoreAllMocks(); vi.unstubAllGlobals() })
async function click(label: string) {
  const button = [...host.querySelectorAll("button")].find((b) => b.textContent === label || b.getAttribute("aria-label") === label)
  expect(button).toBeDefined()
  await act(async () => button!.click())
}
it.each([false, true])("confirms before cancelling and keeps the last result on empty Working (all: %s)", async (all) => {
  const cancel = vi.spyOn(api, "cancelOrder").mockResolvedValue({ account_version: "18", order: { ...order, status: "cancelled" } })
  const cancelAll = vi.spyOn(api, "cancelAllOrders").mockResolvedValue({ account_version: "18", cancelled_orders: [order.id] })
  await act(async () => root.render(<QueryClientProvider client={client}><OrdersView /></QueryClientProvider>))
  await click(all ? "Cancel all" : "Cancel")
  expect(host.querySelector("dialog[open]")).not.toBeNull()
  expect(cancel).not.toHaveBeenCalled()
  expect(cancelAll).not.toHaveBeenCalled()
  await click(all ? "Close Cancel orders" : "Keep order")
  expect(cancel).not.toHaveBeenCalled()
  expect(cancelAll).not.toHaveBeenCalled()
  await click(all ? "Cancel all" : "Cancel")
  await click(all ? "Cancel 1 order" : "Cancel order")
  await waitForRender(() => expect(host.textContent).toContain("No working orders."))
  expect(all ? cancelAll : cancel).toHaveBeenCalledOnce()
  expect(host.querySelector('[role="status"]')?.textContent).toContain(all ? "1 order cancelled" : ": cancelled")
  expect(host.querySelector("dialog")).toBeNull()
})
it("edits the order as it is now, and not one that has since filled", async () => {
  vi.stubGlobal("fetch", vi.fn(() => new Promise(() => {})))
  await act(async () => root.render(<QueryClientProvider client={client}><OrdersView /></QueryClientProvider>))
  await act(async () => host.querySelector<HTMLButtonElement>('[aria-label^="Edit order order-1"]')!.click())
  const key = tradingQueries(0, "17", true).allOrders.queryKey
  // The query cache notifies observers on a timer.
  const update = (changes: Partial<typeof order>) => act(async () => {
    client.setQueryData(key, { account_version: "17", orders: [{ ...order, ...changes }] })
    await new Promise((resolve) => setTimeout(resolve, 0))
  })
  await update({ filled_quantity: 4, remaining_quantity: 1 })
  expect(host.querySelector("dialog[open]")?.textContent).toContain("filled 4 of 5")
  const limit = host.querySelector<HTMLInputElement>('dialog[open] input[aria-label="Limit price"]')!
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(limit, "4.70")
    limit.dispatchEvent(new Event("input", { bubbles: true }))
  })
  await update({ filled_quantity: 5, remaining_quantity: 0, status: "filled" })
  expect(host.querySelector<HTMLButtonElement>('dialog[open] button[type="submit"]')!.disabled).toBe(true)
})
