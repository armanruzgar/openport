// @vitest-environment jsdom
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { ActionBoundary } from "../api/action-destination"
import { DestinationConfirmation } from "../api/action-client"
import { activeAccount } from "../lib/active-account"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { Alert } from "../api/trading-types"
import { describeServerAlert, firedAlerts, heldLegs, serverAlertMessage } from "../lib/alerts"
import * as notifier from "../lib/notify"
import { portfolio, status } from "../test/trading-fixtures"
import { ServerAlerts, ServerAlertWatcher } from "./ServerAlerts"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
let root: Root
let host: HTMLDivElement
let client: QueryClient
const alert: Alert = {
  id: "1", label: "Vol spike", scope: "underlying", metric: "iv30", symbol: "SPX", legs: null, direction: "at_or_above", level: "25.00",
  repeat: false, created_at: "2026-09-22T14:00:00.000Z", actor: "loopback", armed: true, fired: 0, fired_at: null, value: null,
}
function live(version = "1") {
  const state = liveState(status, null, "open", 0, "main", () => {}, "live")
  vi.mocked(useLive).mockReturnValue({ ...state, trading: { enabled: true, reason: null, account_version: version, write: "open", kill_latched: false, fee_per_contract: "0.65", initial_cash: "100000.00" } })
}
async function render(node: React.ReactNode) {
  await act(async () => root.render(<QueryClientProvider client={client}>{node}</QueryClientProvider>))
  // Deliver query notifications and the watcher's effects before either positive
  // or negative assertions. Elapsed wall time does not prove they have run.
  await act(async () => { await vi.runOnlyPendingTimersAsync() })
}
function field(label: string) { return host.querySelector<HTMLInputElement | HTMLSelectElement>(`[aria-label="${label}"]`)! }
async function set(label: string, value: string) {
  const element = field(label)
  const prototype = element instanceof HTMLInputElement ? HTMLInputElement.prototype : HTMLSelectElement.prototype
  await act(async () => {
    Object.getOwnPropertyDescriptor(prototype, "value")!.set!.call(element, value)
    element.dispatchEvent(new Event(element instanceof HTMLInputElement ? "input" : "change", { bubbles: true }))
  })
}
beforeEach(() => {
  vi.useFakeTimers()
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
  vi.spyOn(api, "alerts").mockResolvedValue({ account_version: "1", alerts: [alert] })
  vi.spyOn(api, "portfolio").mockResolvedValue({ ...portfolio, positions: [] })
  vi.spyOn(api, "createAlert").mockResolvedValue({ account_version: "2", alert })
  vi.spyOn(api, "deleteAlert").mockResolvedValue({ account_version: "3", deleted: "1" })
  live()
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); client.clear(); vi.useRealTimers(); vi.restoreAllMocks(); vi.unstubAllGlobals() })

describe("account alerts", () => {
  it.each(["live", "replay"] as const)("shows the simulated forwarding opt-in on %s", async (source) => {
    const configured = { ...status, provider: { ...status.provider, simulated: true }, notifications: {
      enabled: true, include_simulated: false, queue_depth: 0, queue_capacity: 256, dropped: 0,
      channels: [{ id: "hook", type: "webhook" as const, enabled: true, events: ["alert" as const], floor_distance: "500.00",
        delivered: 0, failures: 0, dropped: 0, last_attempt: null, last_delivery: null, last_error: null }],
    } }
    vi.mocked(useLive).mockReturnValue(liveState(configured, null, "open", 0, "main", () => {}, source))
    await render(<ServerAlerts />)
    expect(host.textContent).toContain("demo and replay also require include_simulated")
    configured.notifications.include_simulated = true
    vi.mocked(useLive).mockReturnValue(liveState(configured, null, "open", 0, "main", () => {}, source))
    await render(<ServerAlerts />)
    expect(host.textContent).toContain("forwards each firing")
    configured.notifications.channels[0]!.enabled = false
    await render(<ServerAlerts />)
    expect(host.textContent).not.toContain("forwards each firing")
  })
  it("lists the server's alerts and sets a new one with its measure and level", async () => {
    await render(<ServerAlerts />)
    expect(host.textContent).toContain("Vol spike: SPX 30-day iv ≥ 25.00")
    expect(host.textContent).toContain("waiting")
    await set("Alert scope", "account")
    await set("Alert measure", "day_pnl")
    await set("Alert direction", "at_or_below")
    await set("Alert level", "-500")
    await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Set account alert")!.click())
    expect(api.createAlert).toHaveBeenCalledWith({ scope: "account", metric: "day_pnl", direction: "at_or_below", level: "-500", repeat: false }, "open")
    await act(async () => host.querySelector<HTMLButtonElement>('[aria-label^="Delete alert"]')!.click())
    expect(api.deleteAlert).toHaveBeenCalledWith("1", "open")
  })
  it("announces each firing once, not the alerts it first sees", async () => {
    const notify = vi.spyOn(notifier, "notify").mockImplementation(() => {})
    vi.mocked(api.alerts).mockResolvedValueOnce({ account_version: "1", alerts: [{ ...alert, fired: 1, armed: false, value: "26.10" }] })
    await render(<ServerAlertWatcher sound={false} />)
    expect(notify).not.toHaveBeenCalled()
    vi.mocked(api.alerts).mockResolvedValueOnce({ account_version: "2", alerts: [{ ...alert, fired: 1, armed: false, value: "26.10" },
      { ...alert, id: "2", label: "", scope: "account", metric: "equity", symbol: null, fired: 1, value: "99000.00" }] })
    live("2")
    await render(<ServerAlertWatcher sound={false} />)
    expect(notify).toHaveBeenCalledTimes(1)
    expect(notify).toHaveBeenCalledWith("Account alert", "account equity ≥ 25.00; now 99000.00.", false)
  })
  it("sets a contract Greek alert and an unheld spread with exact signed levels", async () => {
    await render(<ServerAlerts />)
    await set("Alert scope", "contract")
    await set("Alert symbol", "SPXW  261022P05000000")
    await set("Alert measure", "delta")
    await set("Alert level", "-0.123456")
    await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Set account alert")!.click())
    expect(api.createAlert).toHaveBeenLastCalledWith({ scope: "contract", metric: "delta", symbol: "SPXW  261022P05000000",
      level: "-0.123456", direction: "at_or_above", repeat: false }, "open")
    await set("Alert scope", "spread")
    await set("Leg 1 contract", "SPXW  261022C05000000")
    await set("Leg 2 contract", "SPXW  261022C05100000")
    await set("Leg 2 ratio", "2")
    await set("Alert level", "-1.50")
    await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Set account alert")!.click())
    expect(api.createAlert).toHaveBeenLastCalledWith({ scope: "spread", metric: "mark", direction: "at_or_above", level: "-1.50", repeat: false,
      legs: [{ symbol: "SPXW  261022C05000000", side: "buy", ratio: 1 }, { symbol: "SPXW  261022C05100000", side: "sell", ratio: 2 }] }, "open")
  })
  it("shows when an alert fired and reports a refusal without losing the form", async () => {
    vi.mocked(api.alerts).mockResolvedValue({ account_version: "1", alerts: [{ ...alert, fired: 2, armed: false, repeat: true,
      fired_at: "2026-09-22T14:30:00.000Z", value: "26.50" }] })
    await render(<ServerAlerts />)
    expect(host.textContent).toContain("fired 2× at 26.50")
    expect(host.textContent).toContain("waiting to rearm")
    expect(host.querySelector("time")?.dateTime).toBe("2026-09-22T14:30:00.000Z")
    vi.mocked(api.createAlert).mockRejectedValueOnce(new Error("Unknown contract"))
    await set("Alert scope", "contract")
    await set("Alert symbol", "SPXW  261022P05000000")
    await set("Alert level", "4")
    await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Set account alert")!.click())
    expect(host.textContent).toContain("Could not set the alert: Unknown contract")
    expect(field("Alert level").value).toBe("4")
  })
  it("does not announce history when switching accounts", async () => {
    const notify = vi.spyOn(notifier, "notify").mockImplementation(() => {})
    await render(<ServerAlertWatcher sound={false} />)
    const state = vi.mocked(useLive).mock.results.at(-1)!.value as ReturnType<typeof useLive>
    vi.mocked(useLive).mockReturnValue({ ...state, accountScope: state.accountScope + 1 })
    vi.mocked(api.alerts).mockResolvedValue({ account_version: "1", alerts: [{ ...alert, fired: 5 }] })
    await render(<ServerAlertWatcher sound={false} />)
    expect(notify).not.toHaveBeenCalled()
  })
})

describe("account alert helpers", () => {
  it("describes alerts, finds new firings and sizes held legs to their smallest ratio", () => {
    expect(describeServerAlert({ ...alert, scope: "contract", metric: "delta", symbol: "SPXW  261022C05000000", level: "0.60" }))
      .toBe("SPXW 261022C05000000 delta ≥ 0.60")
    expect(serverAlertMessage({ ...alert, value: "25.40" })).toEqual({ title: "Vol spike", body: "SPX 30-day iv ≥ 25.00; now 25.40." })
    expect(firedAlerts(new Map([["1", 1]]), [{ ...alert, fired: 1 }, { ...alert, id: "2", fired: 1 }]).map((a) => a.id)).toEqual(["2"])
    expect(heldLegs([{ symbol: "A", underlying: "SPX", quantity: 2 }, { symbol: "B", underlying: "SPX", quantity: -4 }, { symbol: "C", underlying: "QQQ", quantity: 1 }], "SPX"))
      .toEqual([{ symbol: "A", side: "buy", ratio: 1 }, { symbol: "B", side: "sell", ratio: 2 }])
  })
})


it("sends an alert after a deliberate account switch without destination confirmation", async () => {
  activeAccount.set("main")
  vi.mocked(api.deleteAlert).mockRestore()
  const send = vi.fn(async () => new Response(JSON.stringify({ account_version: "3", deleted: "1" })))
  vi.stubGlobal("fetch", send)
  try {
    await render(<ActionBoundary><ServerAlerts /><DestinationConfirmation /></ActionBoundary>)
    activeAccount.set("second")
    const state = vi.mocked(useLive)()
    vi.mocked(useLive).mockReturnValue({ ...state, account: "second", accountScope: state.accountScope + 1 })
    await render(<ActionBoundary><ServerAlerts /><DestinationConfirmation /></ActionBoundary>)
    await act(async () => host.querySelector<HTMLButtonElement>('[aria-label^="Delete alert"]')!.click())
    expect(send).toHaveBeenCalledWith("/api/alerts/1?account=second", expect.objectContaining({ method: "DELETE" }))
    expect(host.textContent).not.toContain("Confirm changed destination")
  } finally { activeAccount.set("main") }
})
