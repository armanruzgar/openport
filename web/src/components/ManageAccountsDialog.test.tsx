// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, type ReactNode } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import { plans, status, trading } from "../test/trading-fixtures"
import { ManageAccountsDialog } from "./ManageAccountsDialog"
import { NewAccountDialog } from "./AccountSwitcher"
import { PendingSettingsNotice } from "./PendingSettingsNotice"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
let host: HTMLDivElement
let root: Root
let client: QueryClient
const switchAccount = vi.fn()
const accounts = [
  { id: "main", name: "Main", trading, equity: "100000.00" },
  { id: "swing", name: "Swing", trading: { ...trading, plan: "End-of-day 50K", plan_id: "eod-50k" }, equity: "50000.00" },
  { id: "sandbox-trial", name: "Sandbox trial", trading, equity: "100000.00" },
  { id: "old", name: "Old", archived: true, trading, equity: "50000.00" },
]
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity } } })
  client.setQueryData(["live-accounts"], { accounts })
  client.setQueryData(["plans"], { plans })
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, accounts }, null, "open", 0, "swing", switchAccount))
  vi.spyOn(api, "liveAccounts").mockResolvedValue({ accounts })
  vi.spyOn(api, "updateAccount").mockResolvedValue({ account: { id: "swing", name: "Changed", archived: false } })
  vi.spyOn(api, "deleteAccount").mockResolvedValue({ deleted: "swing" })
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  vi.restoreAllMocks(); vi.clearAllMocks(); vi.unstubAllGlobals()
})
async function render(node: ReactNode = <ManageAccountsDialog trading={trading} onClose={() => {}} />) {
  await act(async () => root.render(<QueryClientProvider client={client}>{node}</QueryClientProvider>))
}
async function click(text: string) {
  const button = [...host.querySelectorAll("button")].find((b) => b.textContent === text)!
  expect(button).toBeDefined()
  await act(async () => button.click())
}
it("renames a fixed id, freezes and resumes accounts, and confirms deletion", async () => {
  await render()
  expect(host.textContent).toContain("eod-50k")
  expect(host.querySelector('[aria-label="Archived accounts"]')?.textContent).toContain("Old")
  expect(host.textContent).not.toContain("Delete Main")
  expect(host.textContent).toContain("Delete Sandbox trial")
  await click("Rename Swing")
  const input = host.querySelector("input")!
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(input, "Changed")
    input.dispatchEvent(new Event("input", { bubbles: true }))
  })
  await click("Save name")
  expect(api.updateAccount).toHaveBeenLastCalledWith("swing", { name: "Changed" }, trading.write)
  await click("Archive Swing")
  expect(api.updateAccount).toHaveBeenLastCalledWith("swing", { archived: true }, trading.write)
  expect(switchAccount).toHaveBeenCalledWith("main", "user")
  await click("Unarchive Old")
  expect(api.updateAccount).toHaveBeenLastCalledWith("old", { archived: false }, trading.write)
  await click("Delete Swing")
  expect(api.deleteAccount).not.toHaveBeenCalled()
  await click("Cancel")
  expect(api.deleteAccount).not.toHaveBeenCalled()
  await click("Delete Swing")
  await click("Confirm delete")
  expect(api.deleteAccount).toHaveBeenCalledWith("swing", trading.write)
})
it("copies active settings when creating a new account", async () => {
  vi.spyOn(api, "createAccount").mockResolvedValue({ account: { id: "copy", name: "Copy", account_version: "1", plan: "Practice", equity: "100000.00" } })
  await render(<NewAccountDialog trading={trading} onClose={() => {}} onCreated={() => {}} />)
  const radio = host.querySelector<HTMLInputElement>('input[type="radio"]')!
  const select = host.querySelector<HTMLSelectElement>('[aria-label="Copy limits and guardrails from"]')!
  await act(async () => { radio.click(); select.value = "swing"; select.dispatchEvent(new Event("change", { bubbles: true })) })
  await act(async () => host.querySelector('button[type="submit"]')!.dispatchEvent(new MouseEvent("click", { bubbles: true })))
  expect(api.createAccount).toHaveBeenCalledWith(expect.objectContaining({ copy_settings_from: "swing" }), trading.write)
})
it("shows reset guidance only when queued settings cannot reach their effective day", async () => {
  await render(<PendingSettingsNotice requiresReset />)
  expect(host.textContent).toContain("reset the replay account to apply them now")
  await render(<PendingSettingsNotice requiresReset={false} />)
  expect(host.textContent).toBe("")
})
