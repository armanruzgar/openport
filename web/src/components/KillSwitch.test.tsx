// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { KillResponse, KillState, TradingStatus } from "../api/trading-types"
import { writeToken } from "../lib/write-token"
import { renderTimeout, waitForRender } from "../test/render"
import { status, trading } from "../test/trading-fixtures"
import { KillSwitch } from "./KillSwitch"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear(); writeToken.set("")
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})
async function render(kill: KillState, config: TradingStatus = trading) {
  await act(async () => root.render(<QueryClientProvider client={client}><KillSwitch kill={kill} trading={config} /></QueryClientProvider>))
}
function button(text: string) {
  const found = [...host.querySelectorAll("button")].find((item) => item.textContent === text)
  if (!found) throw new Error(`Missing ${text}`)
  return found
}
async function click(text: string) { await act(async () => button(text).click()) }
async function reason(text: string) {
  const field = [...host.querySelectorAll("label")].find((label) => label.textContent === "Reason")!.querySelector("input")!
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(field, text)
    field.dispatchEvent(new Event("input", { bubbles: true }))
  })
}
const answer = (latched: boolean, cancelled: string[]): KillResponse =>
  ({ account_version: "18", kill: { latched, reason: latched ? "Stepping away" : null }, cancelled_orders: cancelled })
const result = () => host.querySelector('[role="status"]')?.textContent

it("trips an armed switch with the trimmed reason only once confirmed, and says what it cancelled", async () => {
  const setKill = vi.spyOn(api, "setKill").mockResolvedValue(answer(true, ["order-1", "order-2"]))
  await render({ latched: false, reason: null })
  expect(button("Trip kill switch…").disabled).toBe(true)
  await reason("   ")
  expect(button("Trip kill switch…").disabled).toBe(true)
  await reason("  Stepping away  ")
  await click("Trip kill switch…")
  expect(setKill).not.toHaveBeenCalled()
  expect(host.querySelector("dialog")?.textContent).toContain("Latch the kill switch in reduce-only mode?")
  await click("Confirm trip")
  expect(setKill).toHaveBeenCalledOnce()
  expect(setKill).toHaveBeenCalledWith("trip", "Stepping away", "open")
  await waitForRender(() => expect(result()).toBe("Kill switch latched · reduce-only. 2 orders cancelled."))
  expect(host.querySelector("dialog")).toBeNull()
}, renderTimeout)

it("resets a latched switch with the trimmed reason only once confirmed", async () => {
  const setKill = vi.spyOn(api, "setKill").mockResolvedValue(answer(false, []))
  await render({ latched: true, reason: "Daily loss reached" })
  await reason(" Reviewed the day ")
  await click("Reset kill switch…")
  expect(setKill).not.toHaveBeenCalled()
  expect(host.querySelector("dialog")?.textContent).toContain("Clear the latched kill switch and allow new orders?")
  await click("Back")
  expect(host.querySelector("dialog")).toBeNull()
  await click("Reset kill switch…")
  await click("Confirm reset")
  expect(setKill).toHaveBeenCalledOnce()
  expect(setKill).toHaveBeenCalledWith("reset", "Reviewed the day", "open")
  await waitForRender(() => expect(result()).toBe("Kill switch reset. 0 orders cancelled."))
}, renderTimeout)

it("says before a reset that a guardrail's latch will stay", async () => {
  await render({ latched: true, reason: "TRADE_LIMIT", reset_blocked: { code: "TRADE_LIMIT", message: "Neither a kill-switch reset nor an account reset clears it" },
    history: [{ time: "2026-09-22T14:00:01Z", action: "trip", reason: "TRADE_LIMIT", previous: null, actor: "system" }] })
  expect(host.textContent).toContain("Tripped: TRADE_LIMIT · system")
  await reason("Try again")
  await click("Reset kill switch…")
  const dialog = host.querySelector("dialog")!.textContent
  expect(dialog).toContain("This kill-switch reset cannot clear the latch: Neither a kill-switch reset nor an account reset clears it")
  expect(dialog).not.toContain("Clear the latched kill switch")
})

it("sends nothing once write access is lost, even from an open confirmation", async () => {
  const setKill = vi.spyOn(api, "setKill").mockResolvedValue(answer(true, []))
  writeToken.set("fixture-token")
  await render({ latched: false, reason: null }, { ...trading, write: "token" })
  await reason("Stepping away")
  await click("Trip kill switch…")
  await act(async () => { writeToken.set("") })
  expect(button("Confirm trip").disabled).toBe(true)
  await click("Confirm trip")
  expect(setKill).not.toHaveBeenCalled()
  expect(button("Trip kill switch…").disabled).toBe(true)
})
