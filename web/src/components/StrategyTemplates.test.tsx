import { templateServer } from "../test/template-server"
// @vitest-environment jsdom
import { notifyManager, QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { Chain, Surface } from "../api/types"
import { templateChain as chain, farTemplateChain as far } from "../test/template-fixtures"
import { renderTimeout, waitForRender } from "../test/render"
import { account, order, portfolio, status, summary } from "../test/trading-fixtures"
import { ChainView } from "../views/ChainView"
import { StrategyTemplates } from "./StrategyTemplates"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../api/trading", async (original) => ({ ...await original<typeof import("../api/trading")>(), useRefreshTrading: () => vi.fn() }))
vi.mock("./PriceChart", () => ({ PriceChart: () => null }))

const surface: Surface = { symbol: "SPX", spot: 100, as_of: null, version: 1, expiries: [{
  ...chain.expiry, points: [], svi_years: .2, svi_min_k: -.5, svi_max_k: .5,
  svi: { a: .004, b: .025, rho: -.7, m: 0, sigma: .1, points: 13, status: "ok", reason: null, fit_ms: 1,
    rmse_vol_points: .1, butterfly_ok: true, butterfly_min_g: .2, butterfly_k: 0 },
}] }
let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.stubGlobal("ResizeObserver", class { observe() {} unobserve() {} disconnect() {} })
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  vi.spyOn(api, "buildTemplate").mockImplementation(templateServer)
  vi.spyOn(api, "summary").mockResolvedValue({ ...summary, spot: 100, expiries: [chain.expiry, far.expiry] })
  vi.spyOn(api, "chain").mockImplementation(async (_symbol, expiry) => expiry === far.expiry.id ? far : chain)
  vi.spyOn(api, "surface").mockResolvedValue(surface)
  vi.spyOn(api, "account").mockResolvedValue({ ...account, rules: { ...account.rules, buy_only: false } })
  vi.spyOn(api, "portfolio").mockResolvedValue({ ...portfolio, positions: [] })
  vi.spyOn(api, "orders").mockResolvedValue({ account_version: "17", orders: [] })
  vi.spyOn(api, "submitOrder").mockResolvedValue({ account_version: "18", order, fills: [] })
  vi.spyOn(api, "annotateTrade")
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  Object.defineProperty(HTMLElement.prototype, "scrollIntoView", { configurable: true, value() {} })
  localStorage.clear()
  host = document.createElement("div")
  document.body.append(host)
  root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, gcTime: Infinity, staleTime: Infinity } } })
  client.setQueryData(["summary", "SPX", 1], { ...summary, spot: 100, expiries: [chain.expiry, far.expiry] })
  client.setQueryData(["chain", "SPX", chain.expiry.id, .05, 1], chain)
})
afterEach(async () => {
  await act(async () => root.unmount())
  client.clear()
  host.remove()
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
})
async function render() {
  await act(async () => root.render(<QueryClientProvider client={client}><ChainView symbol="SPX" expiry={chain.expiry.id} onExpiry={() => {}} /></QueryClientProvider>))
}
function button(name: string) {
  const found = [...host.querySelectorAll("button")].find((b) => b.textContent === name || b.getAttribute("aria-label") === name)
  if (!found) throw new Error(`Missing button ${name}`)
  return found
}
async function click(name: string) { await act(async () => button(name).click()) }
async function field(label: string, value: string) {
  const input = [...host.querySelectorAll("label")].find((node) => node.firstChild?.textContent === label)?.querySelector("select, input")
  if (!(input instanceof HTMLInputElement || input instanceof HTMLSelectElement)) throw new Error(`Missing field ${label}`)
  await act(async () => {
    const prototype = input instanceof HTMLInputElement ? HTMLInputElement.prototype : HTMLSelectElement.prototype
    Object.getOwnPropertyDescriptor(prototype, "value")!.set!.call(input, value)
    input.dispatchEvent(new Event(input instanceof HTMLInputElement ? "input" : "change", { bubbles: true }))
  })
}
async function readyToReview() { await waitForRender(() => expect(button("Review in ticket").disabled).toBe(false)) }
async function review() {
  await readyToReview()
  await click("Review in ticket")
  await waitForRender(() => expect(host.querySelector('[aria-label="Submit strategy order"]')).not.toBeNull())
}
async function openTemplates() { await render(); await click("Strategy"); await click("Templates"); await readyToReview() }
async function settledSurface(count: number, state: "success" | "error") {
  await waitForRender(() => expect(client.getQueryState(["surface", "SPX", count, 0, 1])?.status).toBe(state))
  // Fallback text also appears while loading. Deliver the settled query's
  // notifications before checking that the ticket still uses the chain smile.
  await act(async () => { await new Promise<void>((resolve) => notifyManager.schedule(resolve)) })
}

describe("strategy template review", { timeout: renderTimeout }, () => {
  it("shows picked legs, risk, net, POP, source, actual width and tag before sending", async () => {
    await openTemplates()
    expect(host.textContent).toContain("Sell 1 × 90 put")
    expect(host.textContent).toContain("Buy 1 × 85 put")
    await review()
    await waitForRender(() => expect(host.textContent).toContain("from the fitted smile"))
    for (const text of ["Bull put spread", "90 P", "85 P", "Net mid", "credit", "Max loss", "Probability of profit · risk-neutral", "from the fitted smile", "put-credit-15d-5w", "Width: 5 points"]) expect(host.textContent).toContain(text)
    expect(api.surface).toHaveBeenCalledWith("SPX", 1, 0, expect.any(AbortSignal))
    expect(api.submitOrder).not.toHaveBeenCalled()
    expect(api.annotateTrade).not.toHaveBeenCalled()
    await click("Submit strategy order")
    expect(api.submitOrder).toHaveBeenCalledWith(expect.objectContaining({ legs: [
      { symbol: chain.strikes.find((r) => r.strike === 90)!.put!.symbol, side: "sell", ratio: 1 },
      { symbol: chain.strikes.find((r) => r.strike === 85)!.put!.symbol, side: "buy", ratio: 1 },
    ] }), "open")
    expect(api.submitOrder).toHaveBeenCalledTimes(1)
    expect(api.annotateTrade).not.toHaveBeenCalled()
  })
  it.each([
    ["condor", "Iron condor", "iron-condor-15d-5w"],
    ["iron-butterfly", "Iron butterfly", "iron-butterfly-atm-5w"],
    ["strangle", "Short strangle", "short-strangle-15d"],
    ["straddle", "Short straddle", "short-straddle-atm"],
    ["butterfly", "Long call butterfly", "long-call-butterfly-atm-5w"],
    ["diagonal", "Diagonal spread", "call-diagonal-atm-5pt"],
  ])("reviews a %s with its risk and tag", async (kind, label, tag) => {
    await openTemplates()
    await field("Template", kind!)
    await review()
    await waitForRender(() => expect(host.textContent).toContain("Probability of profit · risk-neutral"))
    for (const text of [label!, tag!, "Max loss", "Probability of profit · risk-neutral", "Width:"]) expect(host.textContent).toContain(text)
    if (kind === "butterfly") expect(host.querySelector('button[aria-label="Fewer 100 call per unit"]')?.parentElement?.textContent).toContain("2")
    expect(api.submitOrder).not.toHaveBeenCalled()
  })
  it("labels demo prices in the template preview and both tickets", async () => {
    vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"), source: "replay", replay: {
      file: "synthetic-demo", demo: true, provider: "demo", symbols: ["SPX"], started: null, delay_seconds: 0, speed: 1, paused: false, finished: false, time: null,
    } })
    await openTemplates()
    expect(host.textContent).toContain("Demo market · simulated prices")
    await review()
    expect(host.textContent).toContain("Demo market · simulated prices")
    await click("Close Strategy order")
    await click("Single")
    const ask = [...host.querySelectorAll("button")].find((b) => b.getAttribute("aria-label")?.startsWith("buy 100 call at ask"))!
    await act(async () => ask.click())
    expect(host.textContent).toContain("Demo market · simulated prices")
  })
  it("clears the template tag when the trader changes a resulting leg", async () => {
    await openTemplates()
    await review()
    expect(host.textContent).toContain("put-credit-15d-5w")
    await click("Sell 90 put: switch side")
    expect(host.textContent).not.toContain("Template tag:")
  })
  it("saves and recalls parameters by name without submitting an order", async () => {
    await openTemplates()
    await field("Delta (absolute, 0–100)", "10")
    await readyToReview()
    await field("Preset name", "0DTE put spread 10Δ 5 wide")
    await click("Save preset")
    expect(host.textContent).toContain("Preset saved in this browser")
    await field("Template", "straddle")
    await field("Saved preset", "0DTE put spread 10Δ 5 wide")
    expect(host.textContent).toContain("put-credit-10d-5w")
    expect(JSON.parse(localStorage.getItem("openport.strategy-presets.SPX")!)[0].template.target.value).toBe(10)
    expect(api.submitOrder).not.toHaveBeenCalled()
  })
  it("explains unavailable wings and disables review", async () => {
    await openTemplates()
    await field("Width (points)", "50")
    await waitForRender(() => expect(host.textContent).toContain("put wing at 40: outside the loaded strike range 70–130"))
    expect(button("Review in ticket").disabled).toBe(true)
    expect(api.submitOrder).not.toHaveBeenCalled()
  })
  it("waits for the chosen far chain, then reviews a calendar at the near expiry", async () => {
    let resolveFar: (value: Chain) => void = () => {}
    vi.mocked(api.chain).mockImplementation((_symbol, id) => id === far.expiry.id ? new Promise<Chain>((resolve) => { resolveFar = resolve }) : Promise.resolve(chain))
    await openTemplates()
    await field("Template", "calendar")
    expect(button("Review in ticket").disabled).toBe(true)
    expect(host.textContent).toContain(`Loading the far chain (${far.expiry.id})…`)
    expect(api.chain).toHaveBeenCalledWith("SPX", far.expiry.id, 0, expect.any(AbortSignal))
    await act(async () => resolveFar(far))
    await readyToReview()
    expect(button("Review in ticket").disabled).toBe(false)
    await review()
    expect(host.textContent).toContain("Calendar spread")
    expect(host.textContent).toContain("Oct 16 / Nov 20")
    expect(host.textContent).toContain("Estimated P&L at the Oct 16 expiry")
    expect(host.textContent).toContain("call-calendar-atm")
    expect(api.submitOrder).not.toHaveBeenCalled()
  })
  it("says what a calendar's far leg lacks instead of waiting for a chain that never loads", async () => {
    const templates = (expiries: Chain["expiry"][]) => act(async () => root.render(<QueryClientProvider client={client}>
      <StrategyTemplates near={chain} expiries={expiries} onApply={() => {}} onClose={() => {}} />
    </QueryClientProvider>))
    await templates([chain.expiry])
    await field("Template", "calendar")
    expect(host.textContent).toContain(`No expiry after ${chain.expiry.expiry} is listed, so this template has no far leg.`)
    expect(button("Review in ticket").disabled).toBe(true)
    await templates([chain.expiry, far.expiry])
    expect(host.textContent).toContain("Choose a later expiry for the far leg.")
    await field("Far expiry", far.expiry.id)
    await readyToReview()
    await field("Far expiry", "")
    expect(host.textContent).toContain("Choose a later expiry for the far leg.")
    expect(button("Review in ticket").disabled).toBe(true)
  })
  it("recalls a calendar preset whose far expiry has passed with the first later one", async () => {
    localStorage.setItem("openport.strategy-presets.SPX", JSON.stringify([{ name: "Old calendar",
      template: { kind: "calendar", type: "call", target: { mode: "atm" }, farExpiry: "2026-09-18AM", offset: 0 } }]))
    await act(async () => root.render(<QueryClientProvider client={client}>
      <StrategyTemplates near={chain} expiries={[chain.expiry, far.expiry]} onApply={() => {}} onClose={() => {}} />
    </QueryClientProvider>))
    await field("Saved preset", "Old calendar")
    const farExpiry = [...host.querySelectorAll("label")].find((node) => node.firstChild?.textContent === "Far expiry")!.querySelector("select")!
    expect(farExpiry.value).toBe(far.expiry.id)
    expect(host.textContent).not.toContain("2026-09-18AM")
    await readyToReview()
  })
  it("requests the surface prefix containing a later selected expiry", async () => {
    client.setQueryData(["chain", "SPX", far.expiry.id, .05, 1], far)
    await act(async () => root.render(<QueryClientProvider client={client}><ChainView symbol="SPX" expiry={far.expiry.id} onExpiry={() => {}} /></QueryClientProvider>))
    const ask = [...host.querySelectorAll("button")].find((b) => b.getAttribute("aria-label")?.startsWith("buy 100 call at ask"))!
    await act(async () => ask.click())
    await settledSurface(2, "success")
    await waitForRender(() => expect(host.textContent).toContain("from the chain's smile"))
    expect(api.surface).toHaveBeenCalledWith("SPX", 2, 0, expect.any(AbortSignal))
    // A fit for the earlier expiry is never used for the later ticket.
    expect(host.textContent).toContain("from the chain's smile")
  })
  it("shows a far-chain load error without manufacturing legs", async () => {
    vi.mocked(api.chain).mockRejectedValue(new Error("Expiry unavailable"))
    await openTemplates()
    await field("Template", "diagonal")
    await waitForRender(() => expect(host.textContent).toContain("Far chain unavailable: Error: Expiry unavailable"))
    expect(button("Review in ticket").disabled).toBe(true)
  })
  it("uses the fitted smile for a single-leg ticket as well", async () => {
    await render()
    const ask = [...host.querySelectorAll("button")].find((b) => b.getAttribute("aria-label")?.startsWith("buy 100 call at ask"))!
    await act(async () => ask.click())
    await waitForRender(() => expect(host.textContent).toContain("from the fitted smile"))
    expect(host.textContent).toContain("Probability of profit · risk-neutral")
    expect(host.textContent).toContain("from the fitted smile")
  })
  it("keeps the chain smile when the surface is unavailable", async () => {
    vi.mocked(api.surface).mockRejectedValue(new Error("No surface"))
    await openTemplates()
    await review()
    await settledSurface(1, "error")
    await waitForRender(() => expect(host.textContent).toContain("from the chain's smile"))
    expect(host.textContent).not.toContain("from the fitted smile")
  })
})
