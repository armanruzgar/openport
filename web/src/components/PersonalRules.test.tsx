// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import { account, risk, status } from "../test/trading-fixtures"
import { PersonalRules } from "./PersonalRules"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
let host: HTMLDivElement
let root: Root
let client: QueryClient
const guardrails = { soft_floor: "0", soft_floor_percent: 0, max_opening_trades: 0, cooldown_loss: "0", cooldown_minutes: 0, profit_lock: "0" }
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity } } })
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  vi.spyOn(api, "updateGuardrails").mockResolvedValue(risk)
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  vi.restoreAllMocks(); vi.clearAllMocks(); vi.unstubAllGlobals()
})
it.each([null, "2500.00"])("explains when percent floors do nothing and rejects 100 (drawdown %s)", async (drawdown) => {
  client.setQueryData(tradingQueries(0, "17", true).account.queryKey, { ...account, rules: { ...account.rules, max_drawdown: drawdown } })
  await act(async () => root.render(<QueryClientProvider client={client}><PersonalRules risk={{ ...risk, guardrails }} /></QueryClientProvider>))
  const edit = [...host.querySelectorAll("button")].find((button) => button.textContent === "Edit guardrails")!
  await act(async () => edit.click())
  const label = [...host.querySelectorAll("label")].find((entry) => entry.textContent?.startsWith("Soft floor (%"))!
  expect(label.textContent?.includes("This plan has no drawdown floor")).toBe(drawdown == null)
  const input = label.querySelector("input")!
  expect(input.max).toBe("99")
  const save = host.querySelector<HTMLButtonElement>('button[type="submit"]')!
  const change = async (value: string) => act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(input, value)
    input.dispatchEvent(new Event("input", { bubbles: true }))
  })
  await change("100")
  expect(save.disabled).toBe(true)
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  expect(api.updateGuardrails).not.toHaveBeenCalled()
  await change("99")
  expect(save.disabled).toBe(false)
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  expect(api.updateGuardrails).toHaveBeenCalledWith(risk.limits_revision, { ...guardrails, soft_floor_percent: 99 }, "open")
})
it("says under each guardrail what it takes, and offers the write token in the dialog", async () => {
  const discipline = { ...guardrails, loss_streak: 0, size_creep_percent: 0, size_creep_window: 0, size_creep_block: false }
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, trading: { ...status.trading!, write: "token" } }, null, "open"))
  await act(async () => root.render(<QueryClientProvider client={client}><PersonalRules risk={{ ...risk, guardrails: discipline }} /></QueryClientProvider>))
  await act(async () => [...host.querySelectorAll("button")].find((button) => button.textContent === "Edit guardrails")!.click())
  const dialog = host.querySelector("dialog")!
  expect([...dialog.querySelectorAll("button")].some((button) => button.textContent === "Enter write token")).toBe(true)
  const input = (label: string) => [...dialog.querySelectorAll("label")].find((entry) => entry.textContent?.startsWith(label))!.querySelector("input")!
  const set = async (element: HTMLInputElement, value: string) => act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(element, value)
    element.dispatchEvent(new Event("input", { bubbles: true }))
  })
  const hint = (element: HTMLInputElement) => document.getElementById(element.getAttribute("aria-describedby") ?? "")?.textContent
  const creep = input("Size creep: opening order over % of recent median (101–10000; 0 is off)")
  await set(creep, "50")
  expect(creep.getAttribute("aria-invalid")).toBe("true")
  expect(hint(creep)).toBe("Use 101 to 10000, or 0 to turn it off.")
  await set(creep, "150")
  expect(creep.getAttribute("aria-invalid")).toBe("false")
  expect(hint(creep)).toBeUndefined()
  const streak = input("Losing closes in a row that start the cooldown (0–100)")
  await set(streak, "3")
  expect(hint(streak)).toBe("A losing streak needs a cooldown of at least one minute.")
  await set(input("Cooldown after stop-out (market minutes) (0–1440)"), "15")
  expect(streak.getAttribute("aria-invalid")).toBe("false")
})
