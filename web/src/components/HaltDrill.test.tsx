// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import { MAIN_ACCOUNT } from "../lib/active-account"
import { account, status, trading } from "../test/trading-fixtures"
import { HaltDrill } from "./HaltDrill"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
let host: HTMLDivElement, root: Root, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open", 0, MAIN_ACCOUNT, () => {}, "replay"))
  vi.spyOn(api, "haltUnderlying").mockResolvedValue(account)
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})
const minutes = () => [...host.querySelectorAll("label")].find((label) => label.textContent?.startsWith("Minutes"))!.querySelector("input")!
const halt = () => [...host.querySelectorAll("button")].find((button) => button.textContent === "Halt underlying")!
async function set(element: HTMLInputElement | HTMLSelectElement, value: string) {
  const prototype = element instanceof HTMLInputElement ? HTMLInputElement.prototype : HTMLSelectElement.prototype
  await act(async () => {
    Object.getOwnPropertyDescriptor(prototype, "value")!.set!.call(element, value)
    element.dispatchEvent(new Event(element instanceof HTMLInputElement ? "input" : "change", { bubbles: true }))
  })
}

it("lets the minutes be cleared and retyped, and halts for the minutes typed", async () => {
  await act(async () => root.render(<QueryClientProvider client={client}><HaltDrill trading={trading} /></QueryClientProvider>))
  await set(host.querySelector("select")!, "SPX")
  expect(minutes().value).toBe("5")
  await set(minutes(), "")
  expect(minutes().value).toBe("")
  expect(minutes().getAttribute("aria-invalid")).toBe("true")
  expect(halt().disabled).toBe(true)
  expect(host.textContent).toContain("Enter whole minutes from 1 to 390.")
  await set(minutes(), "3")
  await set(minutes(), "30")
  expect(minutes().value).toBe("30")
  expect(halt().disabled).toBe(false)
  await set(minutes(), "391")
  expect(halt().disabled).toBe(true)
  await set(minutes(), "30")
  await act(async () => halt().click())
  expect(api.haltUnderlying).toHaveBeenCalledWith("SPX", 30, trading.write)
})
