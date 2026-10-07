// @vitest-environment jsdom
import { DestinationConfirmation } from "../api/action-client"
import { dataSource } from "../lib/data-source"
import { activeAccount } from "../lib/active-account"
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api, ApiError } from "../api/client"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { HeldStrategy, TradingStatus } from "../api/trading-types"
import { account, order, portfolio, quote, selection, status, trading } from "../test/trading-fixtures"
import { renderTimeout, waitForRender } from "../test/render"
import { OrderTicket } from "./OrderTicket"
import { CancelAllDialog, EditOrderDialog, FlattenDialog } from "./OrderActions"
import { StrategyTicket } from "./StrategyTicket"
import { CloseTicket } from "../views/PositionsView"
import { CloseStrategyDialog } from "./StrategyActions"
import type { Chain } from "../api/types"
import { chain, expiry } from "../test/trading-fixtures"
import type { StrategyLeg } from "../lib/strategy"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../api/trading", async (original) => ({ ...await original<typeof import("../api/trading")>(), useRefreshTrading: () => vi.fn() }))

let root: Root
let host: HTMLDivElement
let client: QueryClient
const scroll = vi.fn()
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  vi.spyOn(api, "orders").mockResolvedValue({ account_version: "17", orders: [] })
  vi.spyOn(api, "submitOrder").mockResolvedValue({ account_version: "18", order, fills: [] })
  vi.spyOn(api, "account").mockResolvedValue({ ...account, rules: { ...account.rules, buy_only: false } })
  vi.spyOn(api, "portfolio").mockResolvedValue({ ...portfolio, positions: [] })
  Object.defineProperty(HTMLElement.prototype, "scrollIntoView", { configurable: true, value: scroll })
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  host = document.createElement("div")
  document.body.append(host)
  root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, gcTime: Infinity } } })
})
afterEach(async () => {
  await act(async () => root.unmount())
  client.clear()
  host.remove()
  scroll.mockClear()
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
})
async function render(config: TradingStatus = trading) {
  await act(async () => root.render(<QueryClientProvider client={client}>
    <OrderTicket selection={selection} quote={quote} trading={config} onClose={() => {}} />
  </QueryClientProvider>))
}
function button(name: string) {
  const found = [...host.querySelectorAll("button")].find((b) => b.textContent === name || b.getAttribute("aria-label") === name)
  if (!found) throw new Error(`Missing button: ${name}`)
  return found
}
function field(name: string): HTMLInputElement | HTMLSelectElement {
  const found = [...host.querySelectorAll("label")].find((label) => label.textContent?.startsWith(name))?.querySelector("input, select")
  if (!(found instanceof HTMLInputElement || found instanceof HTMLSelectElement)) throw new Error(`Missing field: ${name}`)
  return found
}
async function setField(name: string, value: string) {
  const element = field(name)
  const prototype = element instanceof HTMLInputElement ? HTMLInputElement.prototype : HTMLSelectElement.prototype
  await act(async () => {
    Object.getOwnPropertyDescriptor(prototype, "value")!.set!.call(element, value)
    element.dispatchEvent(new Event(element instanceof HTMLInputElement ? "input" : "change", { bubbles: true }))
  })
}
async function click(name: string) { await act(async () => button(name).click()) }
function radio(group: string, option: string) {
  const found = [...host.querySelectorAll(`[role="radiogroup"][aria-label="${group}"] [role="radio"]`)].find((r) => r.textContent === option)
  if (!(found instanceof HTMLButtonElement)) throw new Error(`Missing option: ${group} ${option}`)
  return found
}
async function choose(group: string, option: string) { await act(async () => radio(group, option).click()) }
const checked = (group: string, option: string) => radio(group, option).getAttribute("aria-checked") === "true"

describe.each(["single", "strategy"] as const)("%s ticket joining a whole trade", { timeout: renderTimeout }, (ticket) => {
  const trade: HeldStrategy = {
    id: "7", underlying: "SPX", opened: "2026-09-22T14:00:00Z",
    legs: [{ symbol: "SPXW  261016P06900000", quantity: -1, trade: "7" }, { symbol: "SPXW  261016P06890000", quantity: 1, trade: "8" }],
    round_trips: 2, entries: 1, realised: "0.00", fees: "1.30", unrealised: "0.00", net: "-1.30",
  }
  const others = [{ ...trade, id: "9", underlying: "SPY" }, { ...trade, id: "10", legs: [] }]
  async function renderTicket(strategies: HeldStrategy[]) {
    vi.stubGlobal("ResizeObserver", class { observe() {} disconnect() {} })
    vi.spyOn(api, "previewOrder").mockRejectedValue(new Error("fixture preview"))
    const held = { ...portfolio, positions: [], strategies }
    vi.mocked(api.portfolio).mockResolvedValue(held)
    client.setQueryData(tradingQueries(0, "17", true).portfolio.queryKey, held)
    const legs: StrategyLeg[] = trade.legs.map((leg, i) => ({
      symbol: leg.symbol, underlying: "SPX", side: leg.quantity > 0 ? "buy" : "sell", ratio: 1, type: "put", strike: i ? 6890 : 6900, expiry: expiry.id,
      quote: { ...quote, symbol: leg.symbol, bid: 5 - i, ask: 5.2 - i, mid: 5.1 - i },
    }))
    await act(async () => root.render(<QueryClientProvider client={client}>
      {ticket === "single" ? <OrderTicket selection={selection} quote={quote} trading={trading} onClose={() => {}} />
        : <StrategyTicket legs={legs} onLegs={() => {}} expiries={[expiry]} underlying="SPX" spot={7000} trading={trading} onClose={() => {}} />}
    </QueryClientProvider>))
  }
  async function previewAndSubmit() {
    await waitForRender(() => expect(host.textContent).toContain("Preview failed"))
    expect(api.previewOrder).toHaveBeenCalled()
    await click(ticket === "single" ? "Submit order" : "Submit strategy order")
    expect(api.submitOrder).toHaveBeenCalledTimes(1)
    return [vi.mocked(api.previewOrder).mock.calls.at(-1)![0], vi.mocked(api.submitOrder).mock.calls[0]![0]]
  }
  it("lists open trades on the underlying and sends the chosen trade in previews and orders", async () => {
    await renderTicket([trade, ...others])
    const select = field("Join trade") as HTMLSelectElement
    expect([...select.options].map((option) => option.textContent)).toEqual(["None", "SPX Oct 16 −6900P +6890P · #7"])
    expect(select.value).toBe("")
    await setField("Join trade", "7")
    for (const body of await previewAndSubmit()) expect(body.group).toBe("7")
  })
  it("surfaces scaling refusals with the server's reason and evidence", async () => {
    vi.mocked(api.submitOrder).mockRejectedValue(new ApiError(422, "Scaling plan: 3 contracts exceed the limit", "SCALING_LIMIT", 3, 2))
    await renderTicket([])
    await previewAndSubmit()
    expect(host.textContent).toContain("SCALING_LIMIT")
    expect(host.textContent).toContain("Scaling plan: 3 contracts exceed the limit")
  })
  it("omits the group by default", async () => {
    await renderTicket([trade])
    for (const body of await previewAndSubmit()) expect(body).not.toHaveProperty("group")
  })
  it("omits the group after choosing None", async () => {
    await renderTicket([trade])
    await setField("Join trade", "7")
    await setField("Join trade", "")
    for (const body of await previewAndSubmit()) expect(body).not.toHaveProperty("group")
  })
  it("hides the field without an open trade on this underlying", async () => {
    await renderTicket(others)
    expect(host.textContent).not.toContain("Join trade")
    for (const body of await previewAndSubmit()) expect(body).not.toHaveProperty("group")
  })
  it("omits a selected trade when it no longer holds any legs", async () => {
    await renderTicket([trade])
    await setField("Join trade", "7")
    const held = { ...portfolio, positions: [], strategies: [] }
    vi.mocked(api.portfolio).mockResolvedValue(held)
    await act(async () => {
      client.setQueryData(tradingQueries(0, "17", true).portfolio.queryKey, held)
    })
    await waitForRender(() => expect(host.textContent).not.toContain("Join trade"))
    for (const body of await previewAndSubmit()) expect(body).not.toHaveProperty("group")
  })
})

describe("order ticket interaction", () => {
  it("submits a walk", async () => {
    await render()
    await setField("Limit price", "15.00")
    await act(async () => host.querySelector<HTMLInputElement>('[aria-label="Walk limit"]')!.click())
    await setField("Walk step", "0.10")
    await setField("Every (seconds)", "5")
    await setField("Walk cap", "16.00")
    await click("Submit order")
    expect(api.submitOrder).toHaveBeenCalledWith(expect.objectContaining({ walk: { step: "0.10", seconds: 5, limit: "16.00" } }), "open")
  })
  it("sends a signed credit cap from the strategy walk fields", async () => {
    vi.stubGlobal("ResizeObserver", class { observe() {} disconnect() {} })
    const legs: StrategyLeg[] = [
      { symbol: "SPXW  261016P06900000", underlying: "SPX", side: "sell", ratio: 1, type: "put", strike: 6900, expiry: expiry.id,
        quote: { ...quote, symbol: "SPXW  261016P06900000", bid: 5, ask: 5.2, mid: 5.1 } },
      { symbol: "SPXW  261016P06890000", underlying: "SPX", side: "buy", ratio: 1, type: "put", strike: 6890, expiry: expiry.id,
        quote: { ...quote, symbol: "SPXW  261016P06890000", bid: 4, ask: 4.2, mid: 4.1 } },
    ]
    await act(async () => root.render(<QueryClientProvider client={client}>
      <StrategyTicket legs={legs} onLegs={() => {}} expiries={[expiry]} underlying="SPX" spot={7000} trading={trading} onClose={() => {}} />
    </QueryClientProvider>))
    await act(async () => host.querySelector<HTMLInputElement>('[aria-label="Walk limit"]')!.click())
    await setField("Walk step", "0.05")
    await setField("Every (seconds)", "7")
    await setField("Walk cap", "-0.80")
    await click("Submit strategy order")
    expect(api.submitOrder).toHaveBeenCalledWith(expect.objectContaining({ limit_price: "-1.00", walk: { step: "0.05", seconds: 7, limit: "-0.80" } }), "open")
  })
  it("removes a working order's walk with an explicit null", async () => {
    vi.spyOn(api, "modifyOrder").mockResolvedValue({ account_version: "18", order, fills: [] })
    await act(async () => root.render(<QueryClientProvider client={client}>
      <EditOrderDialog order={{ ...order, walk: { limit: "5.00", seconds: 10, step: "0.10" } }} trading={trading} onClose={() => {}} onDone={() => {}} />
    </QueryClientProvider>))
    expect(host.querySelector<HTMLButtonElement>('button[type="submit"]')!.disabled).toBe(true)
    await act(async () => host.querySelector<HTMLInputElement>('[aria-label="Walk limit"]')!.click())
    await click("Save changes")
    expect(api.modifyOrder).toHaveBeenCalledWith(order.id, { walk: null }, "open")
  })
  it("does not send a hidden walk on IOC orders", async () => {
    await render()
    await act(async () => host.querySelector<HTMLInputElement>('[aria-label="Walk limit"]')!.click())
    await choose("Time in force", "IOC")
    expect(host.querySelector('[aria-label="Walk limit"]')).toBeNull()
    await click("Submit order")
    expect(vi.mocked(api.submitOrder).mock.calls[0]![0]).not.toHaveProperty("walk")
  })
  it.each([400, 403, 404, 409, 422])("starts a fresh ID after HTTP %s and keeps editable form values", async (status) => {
    vi.mocked(api.submitOrder).mockRejectedValueOnce(new ApiError(status, "Delta cap exceeded", "DELTA_LIMIT", 200, 100, "aggregate"))
    await render()
    await choose("Side", "Sell")
    await setField("Quantity", "3")
    await choose("Time in force", "IOC")
    await setField("Limit price", "15.80")
    await click("Submit order")
    const first = vi.mocked(api.submitOrder).mock.calls[0]![0]
    expect(first).toMatchObject({ side: "sell", quantity: 3, time_in_force: "ioc", limit_price: "15.80" })
    expect(host.textContent).toContain("DELTA_LIMIT: Delta cap exceeded")
    expect(host.textContent).toContain("Actual 200.00 · Limit 100.00 · aggregate")
    expect(host.textContent).not.toContain("Retry same order")
    const result = host.querySelector('[aria-label="Order result"]')!
    expect(document.activeElement).toBe(result)
    expect(scroll).toHaveBeenCalledWith({ block: "start" })
    expect(result.compareDocumentPosition(host.querySelector("form")!) & Node.DOCUMENT_POSITION_FOLLOWING).not.toBe(0)
    await click("New order")
    expect(host.querySelector("fieldset")?.disabled).toBe(false)
    expect(document.activeElement).toBe(radio("Side", "Sell"))
    expect(checked("Side", "Sell")).toBe(true)
    expect(field("Quantity").value).toBe("3")
    expect(checked("Time in force", "IOC")).toBe(true)
    expect(field("Limit price").value).toBe("15.80")
    expect(host.textContent).not.toContain("DELTA_LIMIT")
    await click("Submit order")
    const second = vi.mocked(api.submitOrder).mock.calls[1]![0]
    expect(second.client_order_id).not.toBe(first.client_order_id)
    expect({ ...second, client_order_id: first.client_order_id }).toEqual(first)
  })
  it.each([
    new TypeError("Failed to fetch"), new DOMException("Timed out", "TimeoutError"),
    new DOMException("Aborted", "AbortError"), new ApiError(503, "Busy"),
    new ApiError(408, "Request timeout"), new ApiError(504, "Gateway timeout"),
  ])("retries an interrupted request with its frozen ID and values: %s", async (error) => {
    vi.mocked(api.submitOrder).mockRejectedValueOnce(error)
    await render()
    await click("Submit order")
    expect(host.querySelector("fieldset")?.disabled).toBe(true)
    expect(host.textContent).not.toContain("New order")
    await click("Retry same order")
    expect(api.submitOrder).toHaveBeenCalledTimes(2)
    expect(vi.mocked(api.submitOrder).mock.calls[1]![0]).toBe(vi.mocked(api.submitOrder).mock.calls[0]![0])
    expect(host.textContent).toContain("Partial fill")
  })
  it("does not offer an automatic retry or fresh ID for an unknown server failure", async () => {
    vi.mocked(api.submitOrder).mockRejectedValueOnce(new ApiError(500, "Unexpected response"))
    await render()
    await click("Submit order")
    expect(host.textContent).not.toContain("Retry same order")
    expect(host.textContent).not.toContain("New order")
    expect(host.textContent).toContain("Check Positions and Orders to confirm")
  })
  it("trails a bracket stop by ticks of the mark", async () => {
    await render()
    await act(async () => (host.querySelector('input[aria-label="Bracket"]') as HTMLInputElement).click())
    await setField("Stop reads", "mark")
    await act(async () => ([...host.querySelectorAll("label")].find((l) => l.textContent === "Trailing")!.querySelector("input")!).click())
    await setField("Trail by", "ticks")
    await setField("Trail (ticks)", "4")
    expect(host.textContent).toContain("Sells at market when mark ≤ $3.50, trailing 4 ticks.")
    await click("Submit order")
    expect(vi.mocked(api.submitOrder).mock.calls[0]![0]).toMatchObject({
      bracket: { stop_loss: { trigger: { source: "option", direction: "at_or_below", level: "3.50", reference: "mark", trail: { unit: "ticks", value: 4 } } } },
    })
  })
  it("chains a target placed once the entry fills completely", async () => {
    await render()
    await setField("Quantity", "2")
    await choose("Chain", "Then, once filled")
    expect(button("Submit order").disabled).toBe(true)
    await setField("Chained limit", "6.20")
    expect(host.textContent).toContain("Once this order fills completely, places sell 2 @ $6.20 GTC")
    await click("Submit order")
    const sent = vi.mocked(api.submitOrder).mock.calls[0]![0]
    expect(sent.then).toEqual({ symbol: selection.symbol, side: "sell", quantity: 2, type: "limit", time_in_force: "gtc", limit_price: "6.20" })
    expect(sent.oco).toBeUndefined()
  })
  it("chains an order that cancels it, waiting for the underlying", async () => {
    await render()
    await setField("Quantity", "2")
    await choose("Chain", "Or, one cancels other")
    await setField("Chained limit", "4.10")
    await setField("Only when SPX crosses", "7010")
    expect(host.textContent).toContain("The first fill of either cancels the other")
    await click("Submit order")
    expect(vi.mocked(api.submitOrder).mock.calls[0]![0].oco).toEqual({ symbol: selection.symbol, side: "buy", quantity: 2, type: "limit",
      time_in_force: "gtc", limit_price: "4.10", trigger: { source: "underlying", direction: "at_or_above", level: "7010" } })
  })
  it("sends a conditional entry with a bracket whose exits oppose the position", async () => {
    await render({ ...trading, fee_per_contract: "0.65" })
    await choose("Condition", "When SPX crosses")
    await setField("SPX level", "7010")
    expect(host.textContent).toContain("Arms now and activates when SPX ≥ 7,010.00")
    await act(async () => (host.querySelector('input[aria-label="Bracket"]') as HTMLInputElement).click())
    // Suggestions snap to the SPXW tick: 25% below and 50% above the $4.60 entry.
    expect(field("Stop loss price").value).toBe("3.50")
    expect(field("Take profit price").value).toBe("6.90")
    expect(host.textContent).toContain("Sells at market when bid ≤ $3.50.")
    expect(host.textContent).toContain("Rests as a $6.90 limit to sell.")
    await choose("Take profit source", "SPX")
    await setField("Take profit SPX level", "7100")
    expect(button("Submit order").textContent).toBe("Arm · Buy 1 Long Call @ $4.60 · bracket")
    await click("Submit order")
    expect(vi.mocked(api.submitOrder).mock.calls[0]![0]).toMatchObject({
      trigger: { source: "underlying", direction: "at_or_above", level: "7010" },
      bracket: {
        stop_loss: { trigger: { source: "option", direction: "at_or_below", level: "3.50" } },
        take_profit: { trigger: { source: "underlying", direction: "at_or_above", level: "7100" } },
      },
    })
  })
  it("sends an order conditional on another underlying, a study or the time of day", async () => {
    await render({ ...trading, fee_per_contract: "0.65" })
    await choose("Condition", "When a price or study")
    expect(button("Submit order").disabled).toBe(true)
    await setField("Level", "20")
    expect(host.textContent).toContain("Arms now and activates when VIX ≥ 20.00")
    await click("Submit order")
    expect(vi.mocked(api.submitOrder).mock.calls[0]![0]).toMatchObject({
      trigger: { source: "underlying", symbol: "VIX", direction: "at_or_above", level: "20" },
    })
  })
  it("sends a study trigger on its own underlying without a symbol", async () => {
    await render({ ...trading, fee_per_contract: "0.65" })
    await choose("Condition", "When a price or study")
    await setField("Watch", "iv30")
    await setField("Underlying", "")
    await choose("Watch direction", "At or below")
    await setField("Level", "15")
    expect(host.textContent).toContain("SPX 30-day IV ≤ 15.00")
    await click("Submit order")
    const study = vi.mocked(api.submitOrder).mock.calls[0]![0]
    expect(study.trigger).toEqual({ source: "study", study: "iv30", direction: "at_or_below", level: "15" })
  })
  it("sends a time trigger as a New York minute", async () => {
    await render({ ...trading, fee_per_contract: "0.65" })
    await choose("Condition", "At a time")
    expect(field("New York time").value).toBe("15:30")
    await setField("New York time", "15:45")
    expect(host.textContent).toContain("activates at the first update at or after 15:45")
    await click("Submit order")
    expect(vi.mocked(api.submitOrder).mock.calls[0]![0].trigger).toEqual({ source: "time", at: "15:45" })
  })
  it("sends a stop-limit exit: the stop's trigger with the limit it then rests at", async () => {
    await render({ ...trading, fee_per_contract: "0.65" })
    await act(async () => (host.querySelector('input[aria-label="Bracket"]') as HTMLInputElement).click())
    const stopLimit = [...host.querySelectorAll("label")].find((label) => label.textContent === "Stop-limit")!.querySelector("input")!
    await act(async () => stopLimit.click())
    // It starts at the stop's own price.
    expect(field("Stop loss limit price").value).toBe("3.50")
    await setField("Stop loss limit price", "3.40")
    expect(host.textContent).toContain("When bid ≤ $3.50, rests as a $3.40 limit to sell")
    await click("Submit order")
    expect(vi.mocked(api.submitOrder).mock.calls[0]![0]).toMatchObject({
      bracket: { stop_loss: { trigger: { source: "option", direction: "at_or_below", level: "3.50" }, limit_price: "3.40" } },
    })
  })
  it("blocks submission until conditional and bracket levels are entered", async () => {
    await render({ ...trading, fee_per_contract: "0.65" })
    await choose("Condition", "When SPX crosses")
    expect(button("Submit order").disabled).toBe(true)
    await setField("SPX level", "7010")
    expect(button("Submit order").disabled).toBe(false)
    await act(async () => (host.querySelector('input[aria-label="Bracket"]') as HTMLInputElement).click())
    await setField("Stop loss price", "")
    expect(button("Submit order").disabled).toBe(true)
  })
  it("retains INVALID_TICK feedback for typed off-tick prices", async () => {
    vi.mocked(api.submitOrder).mockRejectedValueOnce(new ApiError(422, "Limit price is not a positive multiple of the product tier tick", "INVALID_TICK"))
    await render()
    await setField("Limit price", "4.61")
    await click("Submit order")
    expect(vi.mocked(api.submitOrder).mock.calls[0]![0]).toMatchObject({ limit_price: "4.61" })
    expect(host.textContent).toContain("INVALID_TICK: Limit price is not a positive multiple")
    expect(button("New order")).toBeDefined()
  })
  it("uses buttons and arrow keys for valid ticks and pads cents on blur", async () => {
    await render()
    await setField("Limit price", "3")
    await click("Decrease limit price one tick")
    expect(field("Limit price").value).toBe("2.95")
    await click("Increase limit price one tick")
    expect(field("Limit price").value).toBe("3.00")
    await act(async () => field("Limit price").dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowUp", bubbles: true })))
    expect(field("Limit price").value).toBe("3.10")
    await act(async () => field("Limit price").dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowDown", bubbles: true })))
    expect(field("Limit price").value).toBe("3.00")
    await setField("Limit price", "15.8")
    await act(async () => { field("Limit price").focus(); field("Limit price").blur() })
    expect(field("Limit price").value).toBe("15.80")
  })
  it("uses a server fee update in preference to the manual estimate", async () => {
    await render()
    await setField("Fee / contract", "1.25")
    await setField("Quantity", "3")
    expect(host.textContent).toContain("Estimated fees$3.75")
    await render({ ...trading, fee_per_contract: "0.65" })
    expect(host.querySelector('input[placeholder="Not provided by server"]')).toBeNull()
    expect(host.textContent).toContain("Estimated fees$1.95")
  })
  it("updates the session gate from live ticks: closed blocks, overnight takes limit orders only", async () => {
    vi.mocked(useLive).mockReturnValue(liveState(status, {
      type: "tick", engine: status.engine, feed: status.feed,
      underlyings: [{ ...status.underlyings[0]!, session: { name: "closed", open: false, note: "closed (weekend)" } }],
    }, "open"))
    await render()
    expect(host.textContent).toContain("SPX options are not trading now (closed (weekend)); paper orders wait for the next session.")
    expect(button("Submit order").disabled).toBe(true)
    await click("Submit order")
    expect(api.submitOrder).not.toHaveBeenCalled()
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!, session: { name: "global", open: true, note: "Overnight" } }] }, null, "open"))
    await render()
    expect(host.textContent).not.toContain("not trading now")
    expect(host.textContent).toContain("SPX is in its overnight session (8:15 pm to 9:25 am ET): limit orders only")
    expect(host.querySelectorAll('[role="radiogroup"][aria-label="Order type"] [role="radio"]')).toHaveLength(1)
    expect(host.querySelector('[role="radiogroup"][aria-label="Condition"]')).toBeNull()
    expect(host.querySelector('[aria-label="Bracket"]')).toBeNull()
    expect(button("Submit order").disabled).toBe(false)
    await click("Submit order")
    expect(api.submitOrder).toHaveBeenCalledTimes(1)
    expect(vi.mocked(api.submitOrder).mock.calls[0]![0]).toMatchObject({ type: "limit", time_in_force: "day" })
  })
  it("takes a condition and exits overnight only on a GTC limit, which waits for the regular session", async () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!, session: { name: "global", open: true, note: "Overnight" } }] }, null, "open"))
    await render({ ...trading, fee_per_contract: "0.65" })
    expect(host.querySelector('[role="radiogroup"][aria-label="Condition"]')).toBeNull()
    await choose("Time in force", "GTC")
    expect(host.textContent).toContain("GTC waits for the regular session")
    await choose("Condition", "When SPX crosses")
    await setField("SPX level", "7010")
    await act(async () => (host.querySelector('input[aria-label="Bracket"]') as HTMLInputElement).click())
    await click("Submit order")
    expect(vi.mocked(api.submitOrder).mock.calls[0]![0]).toMatchObject({
      type: "limit", time_in_force: "gtc", trigger: { source: "underlying", direction: "at_or_above", level: "7010" },
      bracket: { stop_loss: { trigger: { source: "option", direction: "at_or_below", level: "3.50" } }, take_profit: { limit_price: "6.90" } },
    })
  })
  it("leaves a Day order plain overnight, even after a condition was chosen as GTC", async () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!, session: { name: "global", open: true, note: "Overnight" } }] }, null, "open"))
    await render()
    await choose("Time in force", "GTC")
    await choose("Condition", "When SPX crosses")
    await setField("SPX level", "7010")
    await act(async () => (host.querySelector('input[aria-label="Bracket"]') as HTMLInputElement).click())
    await choose("Time in force", "Day")
    expect(host.querySelector('[aria-label="Bracket"]')).toBeNull()
    await click("Submit order")
    const sent = vi.mocked(api.submitOrder).mock.calls[0]![0]
    expect(sent).toMatchObject({ type: "limit", time_in_force: "day" })
    expect(sent).not.toHaveProperty("trigger")
    expect(sent).not.toHaveProperty("bracket")
  })
  it("follows the feed's session over the wall clock's", async () => {
    // Just after 09:30 a 15-minute delayed feed still shows the overnight session.
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!,
      session: { name: "regular", open: true, note: "Regular" },
      paper: { accepting: true, reason: null, message: null, session: "global" },
    }] }, null, "open"))
    await render()
    expect(host.textContent).toContain("limit orders only")
    expect(host.querySelectorAll('[role="radiogroup"][aria-label="Order type"] [role="radio"]')).toHaveLength(1)
  })
  it("blocks a regular-session ticket when paper stops accepting and resumes from a tick", async () => {
    const message = "SPX quotes are 10h 20m behind the market; the feed appears to have stalled"
    const stalled = { ...status, underlyings: [{ ...status.underlyings[0]!,
      session: { name: "regular" as const, open: true, note: "Regular" },
      paper: { accepting: false, reason: "FEED_STALLED", message },
    }] }
    vi.mocked(useLive).mockReturnValue(liveState(stalled, null, "open"))
    await render()
    expect(host.textContent).toContain(message)
    expect(button("Submit order").disabled).toBe(true)
    await click("Submit order")
    await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
    expect(api.submitOrder).not.toHaveBeenCalled()

    vi.mocked(useLive).mockReturnValue(liveState(stalled, {
      type: "tick", engine: status.engine, feed: status.feed,
      underlyings: [{ ...stalled.underlyings[0]!,
        session: { name: "closed", open: false, note: "Closed" },
        paper: { accepting: true, reason: null, message: null },
      }],
    }, "open"))
    await render()
    expect(host.textContent).not.toContain(message)
    expect(host.textContent).not.toContain("not trading now")
    expect(button("Submit order").disabled).toBe(false)
    await click("Submit order")
    expect(api.submitOrder).toHaveBeenCalledTimes(1)
  })
  it("blocks on paper.accepting even when the server message is null", async () => {
    vi.mocked(useLive).mockReturnValue(liveState(status, {
      type: "tick", engine: status.engine, feed: status.feed,
      underlyings: [{ ...status.underlyings[0]!, paper: { accepting: false, reason: "FEED_STALLED", message: null } }],
    }, "open"))
    await render()
    expect(host.textContent).toContain("SPX paper orders are unavailable (FEED_STALLED)")
    expect(button("Submit order").disabled).toBe(true)
    await click("Submit order")
    expect(api.submitOrder).not.toHaveBeenCalled()
  })
})


it("submits a GTC limit with supplied template tags and note", async () => {
  await act(async () => root.render(<QueryClientProvider client={client}>
    <OrderTicket selection={selection} quote={quote} trading={trading} onClose={() => {}} tags={["put-credit-10d-5w"]} note="Plan" />
  </QueryClientProvider>))
  await choose("Time in force", "GTC")
  await click("Submit order")
  expect(vi.mocked(api.submitOrder).mock.calls[0]![0]).toMatchObject({ time_in_force: "gtc", tags: ["put-credit-10d-5w"], note: "Plan" })
})


it("explains that a marketable GTC limit waits overnight and markets offer IOC only", async () => {
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!, session: { name: "global", open: true, note: "Overnight" } }] }, null, "open"))
  await render()
  await choose("Time in force", "GTC")
  expect(host.textContent).toContain("GTC waits for the regular session")
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  await render()
  await choose("Order type", "Market")
  expect(host.querySelectorAll('[role="radiogroup"][aria-label="Time in force"] [role="radio"]')).toHaveLength(1)
  await click("Submit order")
  expect(vi.mocked(api.submitOrder).mock.calls[0]![0]).toMatchObject({ type: "market", time_in_force: "ioc" })
})

it("warns on thin liquidity without blocking and shows the executable market size on each side", async () => {
  const thin = { ...quote, bid: 10, ask: 20, mid: 15, volume: 12, oi: 34, bid_size: 3, ask_size: 7 }
  await act(async () => root.render(<QueryClientProvider client={client}>
    <OrderTicket selection={selection} quote={thin} trading={trading} onClose={() => {}} />
  </QueryClientProvider>))
  expect(host.textContent).toContain("Thin liquidity")
  expect(host.textContent).toContain("Spread 66.7% · Volume 12 · OI 34")
  expect(button("Submit order").disabled).toBe(false)
  await choose("Order type", "Market")
  expect(host.textContent).toContain("against displayed ask size 7")
  expect(host.textContent).toContain("Fills are simulated against the displayed quote and size only.")
  await choose("Side", "Sell")
  expect(host.textContent).toContain("against displayed bid size 3")
  await click("Submit order")
  expect(api.submitOrder).toHaveBeenCalledWith(expect.objectContaining({ side: "sell", type: "market" }), trading.write)
})

it("warns about a missing bid while keeping unknown volume distinct from zero", async () => {
  await act(async () => root.render(<QueryClientProvider client={client}>
    <OrderTicket selection={selection} quote={{ ...quote, bid: 0, volume: null }} trading={trading} onClose={() => {}} />
  </QueryClientProvider>))
  expect(host.textContent).toContain("No two-sided liquidity")
  expect(host.textContent).toContain("Volume unknown")
})

it("offers all-session protection and submits a zoned GTD timestamp", async () => {
  await render()
  await choose("Time in force", "EXTO")
  expect(host.textContent).toContain("Works through this trading date")
  await choose("Time in force", "GTC + EXTO")
  expect(host.textContent).toContain("Works in every product session")
  await choose("Time in force", "GTD")
  expect(button("Submit order").disabled).toBe(true)
  await setField("Good until (UTC)", "2026-10-22T14:15")
  await click("Submit order")
  expect(vi.mocked(api.submitOrder).mock.calls[0]![0]).toMatchObject({ time_in_force: "gtd", good_till: "2026-10-22T14:15:00.000Z" })
})

it("allows an EXTO triggered market in the overnight session", async () => {
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!, session: { name: "global", open: true, note: "Overnight" } }] }, null, "open"))
  await render()
  await choose("Time in force", "GTC + EXTO")
  await choose("Condition", "When SPX crosses")
  await setField("SPX level", "7010")
  await choose("Order type", "Market")
  await click("Submit order")
  expect(vi.mocked(api.submitOrder).mock.calls[0]![0]).toMatchObject({ type: "market", time_in_force: "gtc_exto", trigger: { source: "underlying", level: "7010" } })
})


it("enables limit flatten overnight and sends the same pricing to its preview", async () => {
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!, session: { name: "global", open: true, note: "Overnight" } }] }, null, "open"))
  vi.spyOn(api, "previewFlatten").mockRejectedValue(new Error("fixture preview"))
  vi.spyOn(api, "closePositions").mockRejectedValue(new Error("fixture close"))
  await act(async () => root.render(<QueryClientProvider client={client}><FlattenDialog positions={[portfolio.positions[0]!]} orders={[]} trading={trading} onClose={() => {}} /></QueryClientProvider>))
  expect(button("Close 1 position").disabled).toBe(true)
  await setField("Flatten order type", "limit")
  await setField("Ticks through the touch", "3")
  expect(button("Close 1 position").disabled).toBe(false)
  expect(api.previewFlatten).toHaveBeenLastCalledWith(null, "open", { type: "limit", limit_ticks: 3 })
  await click("Close 1 position")
  expect(api.closePositions).toHaveBeenCalledWith(null, "open", { type: "limit", limit_ticks: 3 })
})

it.each([3, -3])("keeps a close within the %s held contracts without opening-size suggestions", async (held) => {
  vi.mocked(api.portfolio).mockResolvedValue({ ...portfolio, positions: [{ ...portfolio.positions[0]!, symbol: selection.symbol, quantity: held }] })
  await act(async () => root.render(<QueryClientProvider client={client}>
    <OrderTicket selection={{ ...selection, closing: true, cell: held > 0 ? "bid" : "ask", quantity: Math.abs(held) }} quote={quote} trading={trading} onClose={() => {}} />
  </QueryClientProvider>))
  await waitForRender(() => expect(host.textContent).toContain(`You hold 3 ${held > 0 ? "long" : "short"}`))
  expect(field("Quantity").value).toBe("3")
  expect(host.textContent).not.toContain("Size to")
  expect(host.querySelector('[aria-label="Quantity 5"]')).toBeNull()
  await setField("Quantity", "37")
  expect(field("Quantity").value).toBe("3")
  await click("Submit order")
  expect(api.submitOrder).toHaveBeenCalledWith(expect.objectContaining({ quantity: 3, side: held > 0 ? "sell" : "buy" }), trading.write)
}, renderTimeout)


it.each(["passed", "failed"] as const)("allows a close after the attempt %s and blocks increasing or reversing it", async (status) => {
  const decided = { ...account, rules: { ...account.rules, buy_only: false }, evaluation: { ...account.evaluation, status } }
  const held = { ...portfolio, positions: [{ ...portfolio.positions[0]!, symbol: selection.symbol, quantity: 3 }] }
  vi.mocked(api.account).mockResolvedValue(decided)
  vi.mocked(api.portfolio).mockResolvedValue(held)
  const queries = tradingQueries(0, "17", true)
  client.setQueryData(queries.account.queryKey, decided)
  client.setQueryData(queries.portfolio.queryKey, held)
  await render()
  await choose("Side", "Buy")
  expect(button("Submit order").disabled).toBe(true)
  await choose("Side", "Sell")
  await setField("Quantity", "3")
  expect(host.textContent).toContain("Closing orders are allowed")
  expect(button("Submit order").disabled).toBe(false)
  await setField("Quantity", "4")
  expect(button("Submit order").disabled).toBe(true)
  await setField("Quantity", "3")
  await click("Submit order")
  expect(api.submitOrder).toHaveBeenCalledWith(expect.objectContaining({ side: "sell", quantity: 3 }), trading.write)
})

it("blocks an opening single order without its plan's required stop", async () => {
  vi.mocked(api.account).mockResolvedValue({ ...account, rules: { ...account.rules, buy_only: false, require_stop_loss: true } })
  client.setQueryData(tradingQueries(0, "17", true).account.queryKey,
    { ...account, rules: { ...account.rules, buy_only: false, require_stop_loss: true } })
  await render()
  expect(host.textContent).toContain("Stop-loss required by this plan")
  expect(button("Submit order").disabled).toBe(true)
  await act(async () => field("Protect with a stop-loss").click())
  expect(button("Submit order").disabled).toBe(false)
  await click("Submit order")
  expect(api.submitOrder).toHaveBeenCalledWith(expect.objectContaining({ bracket: expect.objectContaining({ stop_loss: expect.any(Object) }) }), "open")
})

it("includes a strategy's required stop in preview and blocks an unprotected entry", async () => {
  vi.stubGlobal("ResizeObserver", class { observe() {} disconnect() {} })
  vi.mocked(api.account).mockResolvedValue({ ...account, rules: { ...account.rules, buy_only: false, require_stop_loss: true } })
  vi.spyOn(api, "previewOrder").mockRejectedValue(new Error("fixture preview"))
  client.setQueryData(tradingQueries(0, "17", true).account.queryKey,
    { ...account, rules: { ...account.rules, buy_only: false, require_stop_loss: true } })
  client.setQueryData(tradingQueries(0, "17", true).portfolio.queryKey, { ...portfolio, positions: [] })
  const legs: StrategyLeg[] = [
    { symbol: selection.symbol, underlying: "SPX", side: "buy", ratio: 1, type: "call", strike: 7000, expiry: expiry.id, quote },
    { symbol: "SPXW  261016C07005000", underlying: "SPX", side: "sell", ratio: 1, type: "call", strike: 7005, expiry: expiry.id,
      quote: { ...quote, symbol: "SPXW  261016C07005000", bid: 2, ask: 2.2, mid: 2.1 } },
  ]
  await act(async () => root.render(<QueryClientProvider client={client}>
    <StrategyTicket legs={legs} onLegs={() => {}} expiries={[expiry]} underlying="SPX" spot={7000} trading={trading} onClose={() => {}} />
  </QueryClientProvider>))
  expect(host.textContent).toContain("Stop-loss required by this plan")
  expect(button("Submit strategy order").disabled).toBe(true)
  await act(async () => field("Spread exits").click())
  await setField("Stop level", "-1.00")
  expect(button("Submit strategy order").disabled).toBe(false)
  await waitForRender(() => expect(vi.mocked(api.previewOrder).mock.calls.at(-1)?.[0])
    .toHaveProperty("bracket.stop_loss.trigger.level", "-1.00"))
  await click("Submit strategy order")
  expect(api.submitOrder).toHaveBeenCalledWith(expect.objectContaining({ bracket: expect.objectContaining({ stop_loss: expect.any(Object) }) }), "open")
}, renderTimeout)

it("keeps an unprotected reducing single order available under a stop-required plan", async () => {
  vi.mocked(api.account).mockResolvedValue({ ...account, rules: { ...account.rules, buy_only: false, require_stop_loss: true } })
  const held = { ...portfolio, positions: [{ ...portfolio.positions[0]!, symbol: selection.symbol, quantity: 1 }] }
  vi.mocked(api.portfolio).mockResolvedValue(held)
  client.setQueryData(tradingQueries(0, "17", true).portfolio.queryKey, held)
  client.setQueryData(tradingQueries(0, "17", true).account.queryKey,
    { ...account, rules: { ...account.rules, buy_only: false, require_stop_loss: true } })
  await render()
  await choose("Side", "Sell")
  expect(button("Submit order").disabled).toBe(false)
  expect(host.textContent).not.toContain("Stop-loss required by this plan")
})

it("blocks plan-disallowed openings and shows the reason on the ticket", async () => {
  const restricted = { ...account, rules: { ...account.rules, buy_only: false, underlyings: ["SPY"] } }
  vi.mocked(api.account).mockResolvedValue(restricted)
  client.setQueryData(tradingQueries(0, "17", true).account.queryKey, restricted)
  await render()
  expect(host.textContent).toContain("INSTRUMENT_NOT_ALLOWED")
  expect(button("Submit order").disabled).toBe(true)
})
it("blocks openings outside plan hours using market time", async () => {
  const restricted = { ...account, time: "2026-09-23T20:00:00Z",
    rules: { ...account.rules, buy_only: false, trading_start: "09:30", trading_end: "16:00" } }
  vi.mocked(api.account).mockResolvedValue(restricted)
  client.setQueryData(tradingQueries(0, "17", true).account.queryKey, restricted)
  await render()
  expect(host.textContent).toContain("OUTSIDE_PLAN_HOURS")
  expect(button("Submit order").disabled).toBe(true)
})
it("blocks flat-time openings but leaves reductions available", async () => {
  const restricted = { ...account, time: "2026-09-23T19:45:00Z",
    rules: { ...account.rules, buy_only: false, flat_time: "15:45" } }
  const held = { ...portfolio, positions: [{ ...portfolio.positions[0]!, symbol: selection.symbol, quantity: 1 }] }
  vi.mocked(api.account).mockResolvedValue(restricted)
  vi.mocked(api.portfolio).mockResolvedValue(held)
  client.setQueryData(tradingQueries(0, "17", true).account.queryKey, restricted)
  client.setQueryData(tradingQueries(0, "17", true).portfolio.queryKey, held)
  await render()
  expect(host.textContent).toContain("FLAT_TIME")
  expect(button("Submit order").disabled).toBe(true)
  await choose("Side", "Sell")
  expect(host.textContent).not.toContain("FLAT_TIME")
  expect(button("Submit order").disabled).toBe(false)
})


describe.each(["single", "together"] as const)("%s close ticket quote refresh", { timeout: renderTimeout }, (kind) => {
  it("keeps the typed limit, quantity, focus and scroll while new market quotes load", async () => {
    vi.stubGlobal("ResizeObserver", class { observe() {} disconnect() {} })
    vi.spyOn(api, "previewOrder").mockRejectedValue(new Error("fixture preview"))
    const position = portfolio.positions[0]!
    const secondExpiry = { ...expiry, id: "2026-10-23PM", expiry: "2026-10-23" }
    const secondQuote = { ...quote, symbol: "SPXW  261023C07000000" }
    const secondChain = { ...chain, expiry: secondExpiry, strikes: [{ ...chain.strikes[0]!, call: secondQuote }] }
    const legs: StrategyLeg[] = [quote, secondQuote].map((q, i) => ({
      symbol: q.symbol!, underlying: "SPX", expiry: i ? secondExpiry.id : expiry.id,
      strike: 7000, type: "call", side: "sell", ratio: 1, quote: q,
    }))
    vi.mocked(api.portfolio).mockResolvedValue({ ...portfolio, positions: [position,
      { ...position, symbol: secondQuote.symbol!, expiry: secondExpiry.expiry }] })
    client.setQueryDefaults(["chain"], { staleTime: Infinity })
    client.setQueryData(["chain", "SPX", expiry.id, 0, 1], chain)
    client.setQueryData(["chain", "SPX", secondExpiry.id, 0, 1], secondChain)
    const pending = new Map<string, (data: Chain) => void>()
    vi.spyOn(api, "chain").mockImplementation((_symbol, id) => new Promise<Chain>((resolve) => { pending.set(id!, resolve) }))
    const renderClose = () => act(async () => root.render(<QueryClientProvider client={client}>
      {kind === "single" ? <CloseTicket position={position} trading={trading} onClose={() => {}} />
        : <CloseStrategyDialog plan={{ legs, units: 2 }} underlying="SPX" trading={trading} onClose={() => {}} />}
    </QueryClientProvider>))
    await renderClose()
    const limitName = kind === "single" ? "Limit price" : "Net limit"
    await setField(limitName, "0.95")
    await setField("Quantity", "1")
    const limit = field(limitName)
    const dialog = host.querySelector("dialog")!
    limit.focus()
    dialog.scrollTop = 150
    const preserved = () => {
      expect(field(limitName)).toBe(limit)
      expect(limit.value).toBe("0.95")
      expect(field("Quantity").value).toBe("1")
      expect(host.querySelector("dialog")).toBe(dialog)
      expect(dialog.scrollTop).toBe(150)
      expect(document.activeElement).toBe(limit)
    }
    vi.mocked(useLive).mockReturnValue(liveState({ ...status,
      underlyings: status.underlyings.map((u) => ({ ...u, version: 2 })) }, null, "open"))
    await renderClose()
    expect(pending.size).toBe(kind === "single" ? 1 : 2)
    preserved()
    await act(async () => pending.get(expiry.id)!({ ...chain, version: 2,
      strikes: [{ ...chain.strikes[0]!, call: { ...quote, bid: 0.7, ask: 0.8, mid: 0.75 } }] }))
    await waitForRender(() => expect(host.textContent).toContain("0.70"))
    preserved()
    if (kind === "together") {
      await act(async () => pending.get(secondExpiry.id)!({ ...secondChain, version: 2,
        strikes: [{ ...secondChain.strikes[0]!, call: { ...secondQuote, bid: 0.6, ask: 0.7, mid: 0.65 } }] }))
      await waitForRender(() => expect(host.textContent).toContain("0.60"))
      preserved()
    }
    await waitForRender(() => expect(host.textContent).toContain("Preview failed"))
    await click(kind === "single" ? "Submit order" : "Submit strategy order")
    expect(api.submitOrder).toHaveBeenCalledWith(expect.objectContaining({
      limit_price: kind === "single" ? "0.95" : "-0.95", quantity: 1,
    }), trading.write)
  })
})

describe("destination protection", () => {
  beforeEach(() => { vi.spyOn(api, "summary").mockRejectedValue(new Error("summary unavailable")) })
  afterEach(() => { dataSource.set("live"); activeAccount.set("main") })
  it.each(["source", "account"])("blocks a ticket after a %s change and sends only after explicit confirmation", async change => {
    dataSource.set(change === "source" ? "replay" : "live")
    activeAccount.set("main")
    vi.mocked(api.submitOrder).mockRestore()
    vi.spyOn(api, "previewOrder").mockRejectedValue(new Error("preview unavailable"))
    const send = vi.fn(async () => ({ ok: true, json: async () => ({ account_version: "18", order, fills: [] }) }))
    vi.stubGlobal("fetch", send)
    await act(async () => root.render(<QueryClientProvider client={client}>
      <OrderTicket selection={selection} quote={quote} trading={trading} onClose={() => {}} /><DestinationConfirmation />
    </QueryClientProvider>))
    if (change === "source") dataSource.set("live")
    else activeAccount.set("second")
    vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"), accountScope: 2 })
    // Even a scope refresh must preserve an open ticket's destination.
    await act(async () => root.render(<QueryClientProvider client={client}>
      <OrderTicket variant="panel" selection={selection} quote={quote} trading={trading} onClose={() => {}} /><DestinationConfirmation />
    </QueryClientProvider>))
    await click("Submit order")
    expect(send).not.toHaveBeenCalled()
    expect(host.textContent).toContain("Destination changed from")
    await click(change === "source" ? "Send to live account Practice" : "Send to live account second")
    expect(send).toHaveBeenCalledWith(change === "source" ? "/api/orders" : "/api/orders?account=second", expect.objectContaining({ method: "POST" }))
  })
  it.each(["source", "account"])("cancelling a ticket destination confirmation after a %s change sends nothing", async change => {
    dataSource.set(change === "source" ? "replay" : "live")
    activeAccount.set("main")
    vi.mocked(api.submitOrder).mockRestore()
    vi.spyOn(api, "previewOrder").mockRejectedValue(new Error("preview unavailable"))
    const send = vi.fn()
    vi.stubGlobal("fetch", send)
    await act(async () => root.render(<QueryClientProvider client={client}>
      <OrderTicket selection={selection} quote={quote} trading={trading} onClose={() => {}} /><DestinationConfirmation />
    </QueryClientProvider>))
    if (change === "source") dataSource.set("live")
    else activeAccount.set("second")
    await click("Submit order")
    await click("Cancel")
    expect(send).not.toHaveBeenCalled()
  })
  it.each(["source", "account"])("guards cancel-all after a %s change", async change => {
    dataSource.set(change === "source" ? "replay" : "live")
    activeAccount.set("main")
    const send = vi.fn(async () => ({ ok: true, json: async () => ({ cancelled_orders: [order.id] }) }))
    vi.stubGlobal("fetch", send)
    await act(async () => root.render(<QueryClientProvider client={client}>
      <CancelAllDialog orders={[{ ...order, status: "working" }]} trading={trading} onClose={() => {}} onDone={() => {}} /><DestinationConfirmation />
    </QueryClientProvider>))
    if (change === "source") dataSource.set("live")
    else activeAccount.set("second")
    await click("Cancel 1 order")
    expect(send).not.toHaveBeenCalled()
    await click(change === "source" ? "Send to live account Practice" : "Send to live account second")
    expect(send).toHaveBeenCalledWith(change === "source" ? "/api/orders/cancel" : "/api/orders/cancel?account=second", expect.objectContaining({ method: "POST" }))
  })
})

it("does not send cancel-all if the destination changes again while confirming", async () => {
  dataSource.set("replay"); activeAccount.set("main")
  const send = vi.fn()
  vi.stubGlobal("fetch", send)
  await act(async () => root.render(<QueryClientProvider client={client}>
    <CancelAllDialog orders={[{ ...order, status: "working" }]} trading={trading} onClose={() => {}} onDone={() => {}} /><DestinationConfirmation />
  </QueryClientProvider>))
  dataSource.set("live")
  await click("Cancel 1 order")
  activeAccount.set("third")
  await click("Send to live account Practice")
  expect(send).not.toHaveBeenCalled()
  expect(host.textContent).toContain("live account third")
  activeAccount.set("main")
})


it.each(["source", "account"])("keeps an open flatten bound after a silent %s fallback", async change => {
  dataSource.set(change === "source" ? "replay" : "live")
  activeAccount.set(change === "account" ? "second" : "main")
  vi.spyOn(api, "previewFlatten").mockRejectedValue(new Error("preview unavailable"))
  const send = vi.fn()
  vi.stubGlobal("fetch", send)
  try {
    const draw = () => root.render(<QueryClientProvider client={client}><FlattenDialog positions={[portfolio.positions[0]!]} orders={[]} trading={trading} onClose={() => {}} /><DestinationConfirmation /></QueryClientProvider>)
    await act(async () => draw())
    await setField("Flatten order type", "limit")
    dataSource.set("live"); activeAccount.set("main")
    vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"), accountScope: 2 })
    await act(async () => draw())
    await click("Close 1 position")
    expect(send).not.toHaveBeenCalled()
    expect(host.textContent).toContain("Confirm changed destination")
    await click("Cancel")
    expect(send).not.toHaveBeenCalled()
  } finally { dataSource.set("live"); activeAccount.set("main") }
})
