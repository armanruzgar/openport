// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, type ReactNode } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { ApiError, api } from "../api/client"
import type { AccountGroup, CopyOrderResponse } from "../api/trading-types"
import { trading } from "../test/trading-fixtures"
import { AccountGroupsEditor, CopyGroupField, CopyResults, useAccountGroups } from "./AccountGroups"

let host: HTMLDivElement
let root: Root
let client: QueryClient
const accounts = [
  { id: "main", name: "Main", trading, equity: "100000.00" },
  { id: "swing", name: "Swing", trading, equity: "50000.00" },
  { id: "old", name: "Old", archived: true, trading, equity: "50000.00" },
]
const desk: AccountGroup = { name: "desk", allocation: "ratio", members: [{ account: "main", ratio: "1.00" }, { account: "swing", ratio: "0.50" }] }
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})
async function render(node: ReactNode) {
  await act(async () => root.render(<QueryClientProvider client={client}>{node}</QueryClientProvider>))
  await act(async () => { await new Promise((resolve) => setTimeout(resolve, 0)) })
}
function type(input: HTMLInputElement, value: string) {
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(input, value)
  input.dispatchEvent(new Event("input", { bubbles: true }))
}

it("offers groups to copy to, and none when the order cannot be copied", async () => {
  const chosen: string[] = []
  await render(<CopyGroupField groups={[desk]} value="desk" onChange={(g) => chosen.push(g)} />)
  expect(host.textContent).toContain("Each account of desk submits it as its own order")
  expect(host.textContent).toContain("main ×1, swing ×0.5")
  await render(<CopyGroupField groups={[desk]} value="desk" onChange={() => {}} unavailable="Chained orders are sized by each account" />)
  expect(host.querySelector("select")!.disabled).toBe(true)
  expect(host.textContent).toContain("Chained orders")
  await render(<CopyGroupField groups={[]} value="" onChange={() => {}} />)
  expect(host.querySelector("select")).toBeNull()
})

it("lists each account's outcome, including skipped and refused ones", async () => {
  const result: CopyOrderResponse = { group: "desk", allocation: "ratio", client_order_id: "c", quantity: 2, preview: false, results: [
    { account: "main", quantity: 2, ok: true, status: 201, response: { order: { id: "7", status: "working" } } },
    { account: "swing", quantity: 1, ok: false, status: 422, response: { error: { code: "DELTA_LIMIT", message: "Too much delta" } } },
    { account: "tiny", quantity: 0, ok: false, skipped: true, message: "The allocation gives this account no units" },
  ] }
  await render(<CopyResults result={result} />)
  expect(host.textContent).toContain("1 of 3 accounts accepted")
  expect(host.textContent).toContain("main · 2 — Order 7 working")
  expect(host.textContent).toContain("DELTA_LIMIT: Too much delta")
  expect(host.textContent).toContain("no units")
})

it("reads a server without groups as having none", async () => {
  vi.spyOn(api, "accountGroups").mockRejectedValue(new ApiError(404, "Account groups are not offered", "NOT_FOUND"))
  let groups: AccountGroup[] | undefined
  function Probe() { groups = useAccountGroups().data; return null }
  await render(<Probe />)
  await act(async () => { await Promise.resolve() })
  expect(groups).toEqual([])
})

it("creates a ratio group from active accounts and deletes one", async () => {
  vi.spyOn(api, "accountGroups").mockResolvedValue({ groups: [desk] })
  const put = vi.spyOn(api, "putAccountGroup").mockResolvedValue(desk)
  const remove = vi.spyOn(api, "deleteAccountGroup").mockResolvedValue({ deleted: "desk" })
  await render(<AccountGroupsEditor trading={trading} accounts={accounts} />)
  expect(host.textContent).toContain("desk · main ×1, swing ×0.5")
  expect(host.textContent).not.toContain("Old")  // archived accounts cannot join
  const name = host.querySelector<HTMLInputElement>('input[placeholder="desk"]')!
  await act(async () => type(name, "pair"))
  const boxes = [...host.querySelectorAll<HTMLInputElement>('input[type="checkbox"]')]
  await act(async () => { boxes[0]!.click(); boxes[1]!.click() })
  await act(async () => type(host.querySelector<HTMLInputElement>('input[aria-label="Ratio for Swing"]')!, "2.5"))
  const save = [...host.querySelectorAll("button")].find((b) => b.textContent === "Save group")!
  expect(save.disabled).toBe(false)
  await act(async () => save.click())
  expect(put).toHaveBeenCalledWith("pair", { allocation: "ratio", members: [{ account: "main", ratio: "1" }, { account: "swing", ratio: "2.5" }] }, trading.write)
  await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Delete desk")!.click())
  expect(remove).toHaveBeenCalledWith("desk", trading.write)
})
