// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, type ReactNode } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api, ApiError } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { Order, Position } from "../api/trading-types"
import { order, portfolio, status, trading } from "../test/trading-fixtures"
import { renderTimeout, waitForRender } from "../test/render"
import { heldExitsOrder, PositionExitsDialog } from "./PositionExits"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../api/trading", async (original) => ({ ...await original<typeof import("../api/trading")>(), useRefreshTrading: () => vi.fn() }))
const long: Position = portfolio.positions[0]!
const short: Position = { ...long, type: "put", quantity: -3, average_price: "2.00" }
let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  vi.spyOn(api, "orders").mockResolvedValue({ account_version: "17", orders: [] })
  vi.spyOn(api, "submitOrder").mockResolvedValue({ account_version: "18", order, fills: [] })
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, gcTime: Infinity } } })
})
afterEach(async () => {
  await act(async () => root.unmount()); client.clear(); host.remove(); vi.restoreAllMocks(); vi.unstubAllGlobals()
})
async function render(node: ReactNode) {
  await act(async () => root.render(<QueryClientProvider client={client}>{node}</QueryClientProvider>))
}
async function click(text: string) {
  const button = [...host.querySelectorAll("button")].find((b) => b.textContent === text)
  if (!button) throw new Error(`Missing button ${text}`)
  await act(async () => button.click())
}
async function check(name: string) {
  const input = [...host.querySelectorAll("label")].find((l) => l.textContent === name)?.querySelector("input")
  if (!input) throw new Error(`Missing checkbox ${name}`)
  await act(async () => input.click())
}
async function setField(name: string, value: string) {
  const element = [...host.querySelectorAll("label")].find((l) => l.textContent?.startsWith(name))?.querySelector("input, select")
  if (!(element instanceof HTMLInputElement || element instanceof HTMLSelectElement)) throw new Error(`Missing field ${name}`)
  await act(async () => {
    Object.getOwnPropertyDescriptor(element instanceof HTMLInputElement ? HTMLInputElement.prototype : HTMLSelectElement.prototype, "value")!.set!.call(element, value)
    element.dispatchEvent(new Event(element instanceof HTMLInputElement ? "input" : "change", { bubbles: true }))
  })
}
const submit = () => act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))

it("attaches a take-profit and a bid stop to a held long as one exits_only order", async () => {
  await render(<PositionExitsDialog position={long} trading={trading} onClose={() => {}} />)
  await waitForRender(() => expect(host.textContent).toContain("Set exits"))
  // Suggested from the 4.60 average: a target at 1.5 times, a stop at three quarters.
  await setField("Limit price", "6.90")
  await setField("Stop price", "3.50")
  await submit()
  expect(api.submitOrder).toHaveBeenCalledWith({
    client_order_id: expect.any(String), symbol: long.symbol, side: "sell", quantity: 2, exits_only: true,
    type: "limit", time_in_force: "gtc", limit_price: "6.90",
    bracket: { take_profit: { limit_price: "6.90" }, stop_loss: { trigger: { source: "option", direction: "at_or_below", level: "3.50" } } },
  }, trading.write)
}, renderTimeout)

it("sends a stop-limit alone as its GTC limit with the trigger, buying back a short on its ask", async () => {
  await render(<PositionExitsDialog position={short} trading={trading} onClose={() => {}} />)
  await waitForRender(() => expect(host.textContent).toContain("Set exits"))
  await check("Take profit")
  await setField("Stop price", "3.00")
  await check("Stop-limit")
  await setField("Stop limit price", "3.20")
  expect(host.textContent).toContain("When ask ≥ $3.00, rests as a $3.20 limit to buy")
  await submit()
  expect(api.submitOrder).toHaveBeenCalledWith(expect.objectContaining({
    side: "buy", quantity: 3, type: "limit", time_in_force: "gtc", limit_price: "3.20",
    trigger: { source: "option", direction: "at_or_above", level: "3.00" },
    bracket: { stop_loss: { trigger: { source: "option", direction: "at_or_above", level: "3.00" }, limit_price: "3.20" } },
  }), trading.write)
}, renderTimeout)

it("makes a plain stop alone a triggered market IOC", () => {
  const trigger = { source: "underlying" as const, direction: "at_or_below" as const, level: "6950" }
  expect(heldExitsOrder(long, "c", { stop_loss: { trigger } })).toEqual({ client_order_id: "c", symbol: long.symbol, side: "sell", quantity: 2,
    exits_only: true, bracket: { stop_loss: { trigger } }, type: "market", time_in_force: "ioc", trigger })
})

it("lists the held exits and cancels the pair in one call", async () => {
  const stop: Order = { ...order, id: "8", symbol: long.symbol, side: "sell", type: "market", time_in_force: "ioc", quantity: 2, filled_quantity: 0,
    remaining_quantity: 2, limit_price: null, status: "armed", role: "stop_loss", oco: "9", exits_only: false,
    trigger: { source: "option", direction: "at_or_below", level: "3.50" } }
  const target: Order = { ...stop, id: "9", type: "limit", time_in_force: "gtc", limit_price: "6.90", status: "working", role: "take_profit", oco: "8", trigger: null }
  vi.mocked(api.orders).mockResolvedValue({ account_version: "17", orders: [target, stop] })
  vi.spyOn(api, "cancelOrders").mockResolvedValue({ account_version: "19", cancelled_orders: ["9", "8"] })
  await render(<PositionExitsDialog position={long} trading={trading} onClose={() => {}} />)
  await waitForRender(() => expect(host.textContent).toContain("Cancel exits"))
  expect(host.textContent).toContain("Stop loss · #8 · bid ≤ $3.50")
  expect(host.textContent).toContain("Take profit · #9 · $6.90")
  await click("Cancel exits")
  expect(api.cancelOrders).toHaveBeenCalledWith(["9", "8"], trading.write)
}, renderTimeout)

it("trails a held long's stop by a percentage of the mid", async () => {
  await render(<PositionExitsDialog position={long} trading={trading} onClose={() => {}} />)
  await waitForRender(() => expect(host.textContent).toContain("Set exits"))
  await check("Take profit")
  await setField("Stop price", "3.50")
  await setField("Stop reads", "mid")
  await check("Trailing")
  await setField("Trail by", "percent")
  await setField("Trail (%)", "100")
  expect(host.textContent).toContain("Enter a percentage above 0 and below 100.")
  expect([...host.querySelectorAll("button")].find((b) => b.textContent === "Set exits")?.disabled).toBe(true)
  await setField("Trail (%)", "10")
  expect(host.textContent).toContain("Sells at market when mid ≤ $3.50, trailing 10%.")
  await submit()
  const trigger = { source: "option", direction: "at_or_below", level: "3.50", reference: "mid", trail: { unit: "percent", value: "10.00" } }
  expect(api.submitOrder).toHaveBeenCalledWith(expect.objectContaining({
    side: "sell", type: "market", time_in_force: "ioc", trigger, bracket: { stop_loss: { trigger } },
  }), trading.write)
}, renderTimeout)

it("sends corrected exits after a rejection under a new client order ID, and a retry of the same terms under the same one", async () => {
  vi.mocked(api.submitOrder).mockRejectedValue(new ApiError(422, "Stop is through the market", "INVALID_TRIGGER"))
  await render(<PositionExitsDialog position={long} trading={trading} onClose={() => {}} />)
  await waitForRender(() => expect(host.textContent).toContain("Set exits"))
  await setField("Stop price", "3.50")
  await submit()
  await waitForRender(() => expect(host.textContent).toContain("Stop is through the market"))
  await submit()
  await setField("Stop price", "3.40")
  await submit()
  const ids = vi.mocked(api.submitOrder).mock.calls.map(([request]) => request.client_order_id)
  expect(ids).toHaveLength(3)
  expect(ids[1]).toBe(ids[0])
  expect(ids[2]).not.toBe(ids[0])
}, renderTimeout)
