// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { NewOrder, WhatIfAccount, WhatIfResponse } from "../api/trading-types"
import { addWhatIfOrder, updateWhatIf, whatIfNameError } from "../lib/what-if"
import { portfolio, status, trading } from "../test/trading-fixtures"
import { WhatIfPanel, whatIfName, whatIfOrderText } from "./WhatIfPanel"
vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))

const scope = "live:main"
const position = portfolio.positions[0]!
const account = (equity: string, delta: number, loss: string, pnl: number[]): WhatIfAccount => ({
  equity, buying_power: "9000.00", exposure: { dollar_delta: delta, dollar_gamma_1pct: 10, vega: 5, theta: -2 },
  max_loss: loss, equity_at_max_loss: String(10000 - Number(loss)), breaches_floor: false, breaches_soft_floor: null,
  scenarios: { spot_percent: [-5, 0, 5], vol_points: [-5, 0, 5], pnl: pnl.map((value) => [value - 1, value, value + 1]), complete: true },
  breach: { room: String(Number(equity) - 9000), soft_room: null, complete: true, model: "reflection estimate", underlyings: [] },
})
const response: WhatIfResponse = {
  account_version: "17", simulated: true, current: account("10000.00", 5000, "800.00", [-800, 0, 600]),
  candidates: [
    { name: "Close all", decision: "ok", reason: null, orders: [{ decision: "ok", reason: null }], after: account("9978.70", 0, "21.30", [-21.3, -21.3, -21.3]) },
    { name: "Buy 22", decision: "BUYING_POWER", reason: { code: "BUYING_POWER", message: "Insufficient buying power" },
      orders: [{ decision: "BUYING_POWER", reason: { code: "BUYING_POWER", message: "Insufficient buying power" } }], after: null },
  ],
}
const more: NewOrder = { client_order_id: "preview:single", symbol: position.symbol, side: "buy", type: "market", time_in_force: "ioc", quantity: 22 }
let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  updateWhatIf(scope, () => ({ candidates: [], target: null }))
  client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); client.clear(); vi.restoreAllMocks() })
const render = () => act(async () => root.render(<QueryClientProvider client={client}><WhatIfPanel positions={[position]} trading={trading} /></QueryClientProvider>))
const button = (text: string) => [...host.querySelectorAll("button")].find((b) => b.textContent?.includes(text))!
const name = (value: string) => act(async () => {
  const input = host.querySelector<HTMLInputElement>('input[aria-label="Candidate name"]')!
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(input, value)
  input.dispatchEvent(new Event("input", { bubbles: true }))
})

describe("what-if", () => {
  it("renders past a candidate another build saved without a name", async () => {
    // It threw on name.trim() in every render, blanking Positions on each reload.
    localStorage.setItem("openport.what-if.live:old-build", '{"candidates":[{"id":"a1","orders":[{"symbol":"X"}]}],"target":null}')
    try {
      vi.mocked(useLive).mockReturnValue(liveState(status, null, "open", 0, "old-build"))
      await render()
      expect(host.querySelector('[aria-label="What-if"]')).not.toBeNull()
      expect(host.textContent).toContain("Add closing every position")
    } finally { localStorage.removeItem("openport.what-if.live:old-build") }
  })
  it("builds candidates from positions and tickets, and compares them against the book", async () => {
    await render()
    expect(host.textContent).toContain("Build each candidate from a ticket's preview")
    await act(async () => button("Add closing every position").click())
    // A ticket's order joins as a new candidate, its client ID dropped.
    await act(async () => { addWhatIfOrder(scope, more, "Buy 22") })
    expect(host.textContent).toContain(`Sell ${position.quantity} SPX`)
    expect(host.textContent).toContain("Buy 22 SPX")
    const request = vi.spyOn(api, "whatIf").mockResolvedValue(response)
    await act(async () => button("Compare 2 candidates").click())
    expect(request).toHaveBeenCalledOnce()
    const sent = request.mock.calls[0]![0]
    expect(sent.map((c) => c.name)).toEqual(["Close all", "Buy 22"])
    expect(sent[1]!.orders[0]).toMatchObject({ client_order_id: "", quantity: 22 })
    const table = host.querySelector("table")!.textContent!
    expect(table).toContain("Now")
    expect(table).toContain("$9,978.70")
    expect(table).toContain("$21.30")
    // Each candidate's change in dollar delta from now.
    expect(table).toContain("-5000")
    expect(table).toContain("BUYING_POWER: Insufficient buying power")
    expect(table).toContain("Cannot be projected")
    expect(host.textContent).toContain("P&L from today's equity by spot move, volatility unchanged")
    expect(host.textContent).toContain("+5%")
  })
  it("adds a ticket's orders to a picked candidate, up to four", async () => {
    await act(async () => { addWhatIfOrder(scope, more, "First") })
    await render()
    await act(async () => { host.querySelector<HTMLInputElement>('input[type="checkbox"]')!.click() })
    for (let i = 0; i < 3; ++i) await act(async () => { addWhatIfOrder(scope, { ...more, quantity: i + 1 }, "ignored") })
    expect(host.querySelectorAll("ol > li")).toHaveLength(1)
    expect(host.querySelectorAll("ol > li li")).toHaveLength(4)
    let added = true
    await act(async () => { added = addWhatIfOrder(scope, more, "ignored") })
    expect(added).toBe(false)
    await act(async () => button("Remove").click())
    expect(host.querySelectorAll("ol > li")).toHaveLength(0)
  })
  it("removes the candidate and its target when its last order is removed", async () => {
    await render()
    await act(async () => button("Add closing every position").click())
    await act(async () => { host.querySelector<HTMLInputElement>('input[type="checkbox"]')!.click() })
    await act(async () => { addWhatIfOrder(scope, more, "ignored") })
    const request = vi.spyOn(api, "whatIf").mockResolvedValue(response)
    await act(async () => button("×").click())
    expect(host.querySelectorAll("ol > li li")).toHaveLength(1)
    expect(button("Compare").disabled).toBe(false)
    await act(async () => button("×").click())
    expect(host.querySelectorAll("ol > li")).toHaveLength(0)
    expect(host.textContent).toContain("Build each candidate from a ticket's preview")
    expect(button("Compare").disabled).toBe(true)
    await act(async () => button("Compare").click())
    expect(request).not.toHaveBeenCalled()
    expect(JSON.parse(window.localStorage.getItem(`openport.what-if.${scope}`)!)).toEqual({ candidates: [], target: null })
    await act(async () => { addWhatIfOrder(scope, more, "Next") })
    expect(host.querySelector<HTMLInputElement>('input[aria-label="Candidate name"]')!.value).toBe("Next")
    expect(button("Compare").disabled).toBe(false)
  })
  it.each([
    ["accented", "é", 32, 40],
    ["emoji", "😀", 16, 40],
    ["ASCII", "a", 64, 65],
  ])("checks trimmed %s names against the 64 UTF-8 byte limit", async (_, character, fits, exceeds) => {
    await render()
    await act(async () => button("Add closing every position").click())
    const request = vi.spyOn(api, "whatIf").mockResolvedValue(response)
    await name(`  ${character.repeat(exceeds)}  `)
    expect(button("Compare").disabled).toBe(true)
    expect(host.textContent).toContain("Use a name of 64 UTF-8 bytes or fewer.")
    expect(host.querySelector('input[aria-label="Candidate name"]')!.getAttribute("aria-invalid")).toBe("true")
    await act(async () => button("Compare").click())
    expect(request).not.toHaveBeenCalled()
    await name("   ")
    expect(button("Compare").disabled).toBe(true)
    expect(host.textContent).toContain("Enter a candidate name.")
    await name(`  ${character.repeat(fits)}  `)
    expect(button("Compare").disabled).toBe(false)
    expect(host.textContent).not.toContain("Use a name of 64 UTF-8 bytes or fewer.")
    expect(host.querySelector('input[aria-label="Candidate name"]')!.getAttribute("aria-invalid")).toBe("false")
    await act(async () => button("Compare").click())
    expect(request).toHaveBeenCalledOnce()
    expect(request.mock.calls[0]![0][0]!.name).toBe(character.repeat(fits))
  })
  it("shows results only while the candidates are the ones they were computed for", async () => {
    await render()
    await act(async () => button("Add closing every position").click())
    const request = vi.spyOn(api, "whatIf").mockResolvedValue(response)
    await act(async () => button("Compare 1 candidate").click())
    expect(host.querySelector("table")).not.toBeNull()
    // A ticket or the hedge panel adds an order from outside the panel.
    await act(async () => { addWhatIfOrder(scope, more, "Buy 22") })
    expect(host.querySelectorAll("ol > li")).toHaveLength(2)
    expect(host.querySelector("table")).toBeNull()
    // An answer for a list the trader has changed since does not come back.
    let answer: (value: WhatIfResponse) => void = () => {}
    request.mockReturnValueOnce(new Promise<WhatIfResponse>((resolve) => { answer = resolve }))
    await act(async () => button("Compare 2 candidates").click())
    await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Remove Buy 22"]')!.click())
    await act(async () => answer(response))
    expect(host.querySelectorAll("ol > li")).toHaveLength(1)
    expect(host.querySelector("table")).toBeNull()
    expect(host.textContent).not.toContain("BUYING_POWER")
  })
})

describe("what-if order text", () => {
  const sell: NewOrder = { client_order_id: "", symbol: position.symbol, side: "sell", type: "market", time_in_force: "ioc", quantity: 1 }
  it("says when a triggered order activates and what exits and walk it carries", () => {
    expect(whatIfOrderText(sell)).toMatch(/^Sell 1 SPX\S* Oct 16 7000C at market$/)
    expect(whatIfOrderText({ ...sell, trigger: { source: "option", direction: "at_or_below", level: "4.20" } })).toMatch(/ at market when bid ≤ \$4\.20$/)
    const entry: NewOrder = { client_order_id: "", symbol: position.symbol, side: "buy", type: "limit", time_in_force: "day", limit_price: "5.00", quantity: 1,
      walk: { step: "0.05", seconds: 10, limit: "5.50" },
      bracket: { stop_loss: { trigger: { source: "option", direction: "at_or_below", level: "3.50" } }, take_profit: { limit_price: "6.90" } } }
    expect(whatIfOrderText(entry)).toMatch(/ at \$5\.00, walking to \$5\.50 with stop and target$/)
  })
  it("names a candidate within the 64 UTF-8 bytes the server takes", () => {
    const long: NewOrder = { ...sell, trigger: { source: "underlying", direction: "at_or_above", level: "7010", symbol: "SPX" },
      bracket: { stop_loss: { trigger: { source: "option", direction: "at_or_above", level: "6.00" } }, take_profit: { limit_price: "2.00" } } }
    expect(new TextEncoder().encode(whatIfOrderText(long)).length).toBeGreaterThan(64)
    const name = whatIfName(long)
    expect(whatIfNameError(name)).toBeNull()
    expect(name.endsWith("…")).toBe(true)
    expect(whatIfOrderText(long).startsWith(name.slice(0, -1))).toBe(true)
    expect(whatIfName(sell)).toBe(whatIfOrderText(sell))
  })
})
