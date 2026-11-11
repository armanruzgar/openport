// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, useState } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { NewOrder, OrderPreview } from "../api/trading-types"
import { order as resting, status, trading } from "../test/trading-fixtures"
import { EditOrderDialog } from "./OrderActions"
import { OrderPreviewPanel, previewQuoteRefreshMs, useOrderPreview } from "./OrderPreview"
vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const preview: OrderPreview = { account_version: "17", decision: "ok", reason: null, buying_power: { required: "501", before: "10000", after: "9499" },
  exposure_change: { dollar_delta: 100, dollar_gamma_1pct: 1, vega: 10, theta: -5 }, max_loss: "501", max_loss_basis: "expiry_payoff", equity_at_max_loss: "9499", breaches_floor: true,
  breaches_soft_floor: false, max_units: 3, max_units_basis: "floor", simulated: true, breach: { room: "1000", soft_room: null, complete: true, model: "reflection estimate", underlyings: [] } }
const order: NewOrder = { client_order_id: "preview:test", symbol: "SPXW  261022C05000000", side: "buy", type: "market", quantity: 1, time_in_force: "ioc" }
let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.useFakeTimers()
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); client.clear(); vi.useRealTimers(); vi.restoreAllMocks() })
async function tick() { await act(async () => { await vi.advanceTimersByTimeAsync(350) }); await act(async () => { await vi.advanceTimersByTimeAsync(10) }) }
function Harness() {
  const [quantity, setQuantity] = useState(1)
  const result = useOrderPreview({ ...order, quantity }, trading)
  return <><output>{quantity}</output><OrderPreviewPanel preview={result} onSize={setQuantity} /></>
}
describe("order preview", () => {
  it("debounces, renders server numbers and sets quantity with Size to floor", async () => {
    const request = vi.spyOn(api, "previewOrder").mockResolvedValue(preview)
    await act(async () => root.render(<QueryClientProvider client={client}><Harness /></QueryClientProvider>))
    expect(request).not.toHaveBeenCalled()
    await tick()
    expect(request).toHaveBeenCalledOnce()
    expect(host.textContent).toContain("$9,499.00")
    expect(host.textContent).toContain("could breach the plan floor")
    await act(async () => host.querySelector("button")!.click())
    expect(host.querySelector("output")!.textContent).toBe("3")
    expect(host.textContent).not.toContain("$9,499.00")
    await tick()
    expect(request).toHaveBeenLastCalledWith(expect.objectContaining({ quantity: 3 }), trading.write)
  })
  it("checks the order again as quotes move, at most every few seconds, keeping the last answer up", async () => {
    // An account with nothing on the contract: its version stays put while the market moves.
    const request = vi.spyOn(api, "previewOrder").mockResolvedValue(preview)
    const view = () => act(async () => root.render(<QueryClientProvider client={client}><Harness /></QueryClientProvider>))
    const moved = (version: number) => liveState({ ...status, underlyings: status.underlyings.map((u) => ({ ...u, version })) }, null, "open")
    await view()
    await tick()
    expect(request).toHaveBeenCalledOnce()
    let answer!: (value: OrderPreview) => void
    request.mockImplementationOnce(() => new Promise((resolve) => { answer = resolve }))
    vi.mocked(useLive).mockReturnValue(moved(2))
    await view()
    await act(async () => { await vi.advanceTimersByTimeAsync(previewQuoteRefreshMs) })
    await act(async () => { await vi.advanceTimersByTimeAsync(10) })
    expect(request).toHaveBeenCalledTimes(2)
    expect(host.textContent).toContain("$9,499.00")
    expect(host.textContent).not.toContain("Checking order")
    await act(async () => { answer({ ...preview, buying_power: { ...preview.buying_power, after: "9399" } }); await vi.advanceTimersByTimeAsync(10) })
    expect(host.textContent).toContain("$9,399.00")
    // A quote a second asks once per period, not once per quote.
    for (const version of [3, 4, 5]) {
      vi.mocked(useLive).mockReturnValue(moved(version))
      await view()
      await act(async () => { await vi.advanceTimersByTimeAsync(500) })
    }
    expect(request).toHaveBeenCalledTimes(2)
    await act(async () => { await vi.advanceTimersByTimeAsync(previewQuoteRefreshMs) })
    await act(async () => { await vi.advanceTimersByTimeAsync(10) })
    expect(request).toHaveBeenCalledTimes(3)
  })
  it("shows failure without guessing and disables size", async () => {
    vi.spyOn(api, "previewOrder").mockRejectedValue(new Error("offline"))
    await act(async () => root.render(<QueryClientProvider client={client}><Harness /></QueryClientProvider>))
    await tick()
    expect(host.textContent).toContain("Preview failed")
    expect(host.textContent).not.toContain("$9,499.00")
    expect(host.querySelector("button")!.disabled).toBe(true)
  })
  it("labels the Greeks as the order's change, not the book after it", async () => {
    const result = { ...preview, exposure_change: { dollar_delta: -69965.38, dollar_gamma_1pct: 1, vega: 10, theta: -5 } }
    await act(async () => root.render(<OrderPreviewPanel preview={{ data: result, error: null, loading: false }} onSize={() => {}} />))
    const changes = host.querySelector('[aria-label="Change in Greeks"]')!
    expect(changes.textContent).toContain("Change from this order")
    expect(changes.textContent).toContain("Dollar delta-69965.38")
    expect(changes.textContent).toContain("Vega+10.00")
    expect(host.textContent).toContain("the change in Greeks")
  })
  it("names the soft floor when an account has only that, and shows no levels without a floor", async () => {
    const underlyings = [{ underlying: "SPX", spot: 6000, complete: true, close_sigma: 0.01, down: null, up: { points: 30, percent: 0.5, touch_probability: 0.2 } }]
    const soft = { ...preview, breach: { ...preview.breach, room: null, soft_room: "468.70", underlyings } }
    await act(async () => root.render(<OrderPreviewPanel preview={{ data: soft, error: null, loading: false }} onSize={() => {}} />))
    expect(host.textContent).toContain("Up to soft floor: 30.00 points")
    const none = { ...preview, breach: { ...preview.breach, room: null, soft_room: null, underlyings } }
    await act(async () => root.render(<OrderPreviewPanel preview={{ data: none, error: null, loading: false }} onSize={() => {}} />))
    expect(host.textContent).toContain("No plan or soft floor")
    expect(host.textContent).not.toContain("Up to")
  })
  it("adds the soft floor's own levels when an account has both floors", async () => {
    const underlyings = [{ underlying: "SPX", spot: 6000, complete: true, close_sigma: 0.01,
      down: { points: -60, percent: -1, touch_probability: 0.3 }, up: null,
      soft_down: { points: -24, percent: -0.4, touch_probability: 0.7 }, soft_up: null }]
    const both = { ...preview, breach: { ...preview.breach, room: "1000", soft_room: "400", underlyings } }
    await act(async () => root.render(<OrderPreviewPanel preview={{ data: both, error: null, loading: false }} onSize={() => {}} />))
    expect(host.textContent).toContain("Down to floor: -60.00 points")
    expect(host.textContent).toContain("Down to soft floor: -24.00 points")
    expect(host.textContent).toContain("Up to soft floor: No crossing in scan")
  })
  it("shows the buying-power fit apart from the floor fit, and no floor fit without a floor", async () => {
    const split = { ...preview, max_units: 0, max_units_buying_power: 16, max_units_floor: 0 }
    await act(async () => root.render(<OrderPreviewPanel preview={{ data: split, error: null, loading: false }} onSize={() => {}} />))
    expect(host.textContent).toContain("Fits buying power and limits16 units")
    expect(host.textContent).toContain("Fits 50% of floor room0 units")
    expect(host.textContent).toContain("Size at 50% of floor room0 units")
    expect(host.querySelector("button")!.disabled).toBe(true)
    const floorless = { ...preview, max_units: 23, max_units_basis: "buying_power" as const, max_units_buying_power: 23, max_units_floor: null,
      breach: { ...preview.breach, room: null, soft_room: null } }
    await act(async () => root.render(<OrderPreviewPanel preview={{ data: floorless, error: null, loading: false }} onSize={() => {}} />))
    expect(host.textContent).toContain("Size within buying power23 units")
    expect(host.textContent).toContain("Size to buying power")
    expect(host.textContent).not.toContain("floor room")
    const unavailable = { ...preview, decision: "INVALID_TICK", reason: { code: "INVALID_TICK", message: "off tick" },
      max_units: null, max_units_basis: null, max_units_buying_power: null, max_units_floor: null }
    await act(async () => root.render(<OrderPreviewPanel preview={{ data: unavailable, error: null, loading: false }} onSize={() => {}} />))
    expect(host.textContent).toContain("Suggested sizeUnavailable")
    expect(host.querySelector("button")!.disabled).toBe(true)
  })
  it("says what fills at once, what cancels and the full size block by block", async () => {
    const symbol = "SPXW  261022C05000000"
    const execution = { status: "cancelled" as const, filled_quantity: 15, remaining_quantity: 5,
      reason: { code: "IOC_REMAINDER", message: "IOC exhausted available displayed liquidity" },
      fills: [{ symbol, side: "buy" as const, quantity: 15, price: "4.2" }], average_fill_price: "4.2",
      schedule: [{ symbol, side: "buy" as const, quantity: 8, price: "4.2" }, { symbol, side: "buy" as const, quantity: 12, price: "4.3" }],
      average_price: "4.26" }
    await act(async () => root.render(<OrderPreviewPanel preview={{ data: { ...preview, execution }, error: null, loading: false }} onSize={() => {}} />))
    const section = host.querySelector('[aria-label="Expected execution"]')!
    expect(section.textContent).toContain("Fills 15 at once at $4.20 on average. 5 would cancel: IOC exhausted available displayed liquidity.")
    expect(section.textContent).toContain("Full size at the current quotes: $4.26 per unit")
    expect(section.textContent).toContain("Buy 12 SPXW Oct 22 5000C at $4.30")
    const resting = { ...execution, status: "working" as const, filled_quantity: 0, remaining_quantity: 2, reason: null, fills: [], average_fill_price: null }
    await act(async () => root.render(<OrderPreviewPanel preview={{ data: { ...preview, execution: resting }, error: null, loading: false }} onSize={() => {}} />))
    expect(host.textContent).toContain("Nothing fills at once. 2 work as a resting order.")
  })
  it("previews an order change as the terms are edited, and sizes the units beside those filled", async () => {
    const execution = { status: "filled" as const, filled_quantity: 3, remaining_quantity: 0, reason: null,
      fills: [{ symbol: resting.symbol!, side: "buy" as const, quantity: 3, price: "4.70" }], average_fill_price: "4.70", schedule: [], average_price: null }
    const request = vi.spyOn(api, "previewChange").mockResolvedValue({ ...preview, max_units: 4, execution,
      buying_power: { required: "1412", before: "10000", working: "9200", after: "8600" } })
    await act(async () => root.render(<QueryClientProvider client={client}>
      <EditOrderDialog order={resting} trading={trading} onClose={() => {}} onDone={() => {}} /></QueryClientProvider>))
    expect(host.querySelector('[aria-label="Order preview"]')).toBeNull()
    const price = host.querySelector<HTMLInputElement>('input[aria-label="Limit price"]')!
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(price, "4.70")
      price.dispatchEvent(new Event("input", { bubbles: true }))
    })
    await tick()
    expect(request).toHaveBeenCalledWith("order-1", { limit_price: "4.70" }, trading.write)
    expect(host.textContent).toContain("Room after this change")
    expect(host.textContent).toContain("Fills 3 at once at $4.70 on average.")
    await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Order preview"] button')!.click())
    // Four more units beside the two filled.
    expect(host.querySelector<HTMLInputElement>('input[aria-label="Quantity"]')!.value).toBe("6")
  })
  it("shows the server's warnings about terms that act at once", async () => {
    const warned = { ...preview, warnings: [{ code: "STOP_REACHED", message: "The stop's trigger, at or below 3.10, is already reached (now 3.00): it fires as soon as the entry fills" }] }
    await act(async () => root.render(<OrderPreviewPanel preview={{ data: warned, error: null, loading: false }} onSize={() => {}} />))
    expect(host.textContent).toContain("Check: The stop's trigger, at or below 3.10, is already reached (now 3.00): it fires as soon as the entry fills")
  })
  it("labels scenario estimates and prevents zero-unit sizing", async () => {
    const result = { ...preview, max_loss_basis: "scenario_grid" as const, max_units: 0 }
    await act(async () => root.render(<OrderPreviewPanel preview={{ data: result, error: null, loading: false }} onSize={() => {}} />))
    expect(host.textContent).toContain("Scenario-grid loss")
    expect(host.querySelector("button")!.disabled).toBe(true)
  })
})

it("keeps the close preview's risk and fees while hiding every sizing control and suggestion", async () => {
  await act(async () => root.render(<OrderPreviewPanel sizing={false} preview={{ data: { ...preview, max_units: 37, fee: "1.30" }, error: null, loading: false }} onSize={() => {}} />))
  expect(host.querySelector("button")).toBeNull()
  expect(host.textContent).not.toMatch(/Size|Fits|Sizing|units/)
  expect(host.textContent).toContain("$1.30")
  expect(host.textContent).toContain("Buying power after")
})


it.each(["floor", "buying_power", "limits"] as const)("names the binding %s constraint, even when an account has a floor", async (basis) => {
  await act(async () => root.render(<OrderPreviewPanel preview={{ data: { ...preview, max_units: 2, max_units_basis: basis }, error: null, loading: false }} onSize={() => {}} />))
  expect(host.querySelector("button")!.textContent).toBe(`Size to ${basis === "buying_power" ? "buying power" : basis}`)
})
it("never labels old floorless previews as floor sizing and does not guess between power and limits", async () => {
  const older = { ...preview, max_units_basis: undefined, breach: { ...preview.breach, room: null, soft_room: null } }
  await act(async () => root.render(<OrderPreviewPanel preview={{ data: older, error: null, loading: false }} onSize={() => {}} />))
  expect(host.textContent).not.toContain("floor room")
  expect(host.querySelector("button")!.textContent).toBe("Use suggested size")
  const limited = { ...older, max_units_basis: "limits" as const, max_units: 100 }
  await act(async () => root.render(<OrderPreviewPanel preview={{ data: limited, error: null, loading: false }} onSize={() => {}} />))
  expect(host.textContent).toContain("Size within limits100 units")
  expect(host.textContent).not.toContain("floor room")
})

it("shows trade risk and the plan cap alongside existing preview numbers", async () => {
  const data = { ...preview, trade_risk: "120.00", trade_risk_limit: "100.00", trade_risk_basis: "stop_loss" as const }
  await act(async () => root.render(<OrderPreviewPanel preview={{ data, error: null, loading: false }} onSize={() => {}} />))
  for (const text of ["Trade risk before fees", "$120.00", "Plan trade-risk limit", "$100.00"]) expect(host.textContent).toContain(text)
  await act(async () => root.render(<OrderPreviewPanel preview={{ data: { ...data, trade_risk: null, trade_risk_basis: "unbounded_or_unknown" }, error: null, loading: false }} onSize={() => {}} />))
  expect(host.textContent).toContain("Unbounded or unknown")
})

it("shows direction and volume refusal evidence from the server in tickets", async () => {
  for (const [code, message] of [
    ["HEDGING", "Opening SPY dollar delta -$30,316 opposes held dollar delta +$29,576"],
    ["COUNTER_POSITION", "Opening SPY dollar delta -$30,316 opposes account other held dollar delta +$29,576"],
    ["MAX_VOLUME_SHARE", "SPY option: held plus opening contracts 2 exceeds 10% of current-date volume unknown (missing, invalid or stale)"],
  ] as const) {
    const data = { ...preview, decision: code, reason: { code, message, actual: null, limit: null, scope: "SPY" } }
    await act(async () => root.render(<OrderPreviewPanel preview={{ data, error: null, loading: false }} onSize={() => {}} />))
    expect(host.textContent).toContain(message)
  }
})
