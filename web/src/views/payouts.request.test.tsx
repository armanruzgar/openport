// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api, ApiError } from "../api/client"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { Account } from "../api/trading-types"
import { writeToken } from "../lib/write-token"
import { renderTimeout, waitForRender } from "../test/render"
import { fundedAccount, plans, status } from "../test/trading-fixtures"
import { PayoutsView } from "./PayoutsView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
// Payout #2 is open: $1,000.00 to $3,000.00, 80% to the trader.
const eligible: Account = { ...fundedAccount, payout: { ...fundedAccount.payout!, eligible: true, blocked: null, qualifying_days: 8 } }
// The server's answer once it pays: the payout recorded and a new cycle not yet eligible.
const paid: Account = { ...eligible,
  evaluation: { ...eligible.evaluation, payouts: [...eligible.evaluation.payouts,
    { number: 2, time: "2026-09-25T18:00:00Z", day: "2026-09-25", amount: "3000.00", trader_share: "2400.00", balance: "103050.00" }] },
  payout: { ...eligible.payout!, eligible: false, number: 3, qualifying_days: 0,
    blocked: { code: "PAYOUT_NOT_ELIGIBLE", message: "Not enough qualifying days in this payout cycle", actual: 0, limit: 8 } } }
let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  vi.spyOn(api, "account").mockResolvedValue(eligible)
  vi.spyOn(api, "plans").mockResolvedValue({ plans })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  client.setQueryData(tradingQueries(0, "17", true).account.queryKey, eligible)
  client.setQueryData(["plans"], { plans })
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear(); writeToken.set("")
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})
async function render() { await act(async () => root.render(<QueryClientProvider client={client}><PayoutsView /></QueryClientProvider>)) }
function amount() {
  const label = [...host.querySelectorAll("label")].find((entry) => entry.textContent === "Amount")!
  return document.getElementById(label.htmlFor) as HTMLInputElement
}
async function type(value: string) {
  const field = amount()
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(field, value)
    field.dispatchEvent(new Event("input", { bubbles: true }))
  })
}
const submitButton = () => host.querySelector<HTMLButtonElement>('form button[type="submit"]')!
// Pressing Enter or clicking submits the form whether or not the button has re-rendered disabled yet.
const submit = () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true }))

it("requests a payout once however quickly it is submitted again, then shows what the server recorded", async () => {
  let answer!: (value: Account) => void
  vi.spyOn(api, "requestPayout").mockImplementation(() => new Promise((resolve) => { answer = resolve }))
  await render()
  expect(amount().value).toBe("3000.00")
  expect(submitButton().disabled).toBe(false)
  await act(async () => { submit(); submit() })
  expect(api.requestPayout).toHaveBeenCalledOnce()
  expect(api.requestPayout).toHaveBeenCalledWith("3000.00", "open")
  expect(submitButton().textContent).toBe("Requesting…")
  vi.mocked(api.account).mockResolvedValue(paid)
  await act(async () => answer(paid))
  await waitForRender(() => expect(host.textContent).toContain("Payout #2 of $3,000.00 requested: $2,400.00 to you."))
  expect(amount().value).toBe("")
  expect(api.requestPayout).toHaveBeenCalledOnce()
}, renderTimeout)

it("sends the trimmed amount as text and shows the server's refusal", async () => {
  vi.spyOn(api, "requestPayout").mockRejectedValue(new ApiError(422, "Close every position before a payout", "PAYOUT_NOT_FLAT"))
  await render()
  await type(" 2500.50 ")
  await act(async () => submit())
  expect(api.requestPayout).toHaveBeenCalledWith("2500.50", "open")
  await waitForRender(() => expect(host.querySelector('[role="alert"]')?.textContent).toBe("PAYOUT_NOT_FLAT: Close every position before a payout"))
  expect(host.textContent).not.toContain("requested:")
  expect(submitButton().disabled).toBe(false)
}, renderTimeout)

it("sends nothing for an amount outside the payout's range", async () => {
  vi.spyOn(api, "requestPayout").mockResolvedValue(paid)
  await render()
  for (const [value, message] of [["3000.01", "The most you can request now is $3,000.00"], ["999.99", "The minimum payout is $1,000.00"],
    ["12.345", "Enter an amount in dollars and cents"]] as const) {
    await type(value)
    expect(amount().getAttribute("aria-invalid")).toBe("true")
    expect(document.getElementById(amount().getAttribute("aria-describedby")!)?.textContent).toBe(message)
    expect(submitButton().disabled).toBe(true)
    await act(async () => submit())
  }
  expect(api.requestPayout).not.toHaveBeenCalled()
})

it("waits for the write token on a server that requires one", async () => {
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, trading: { ...status.trading!, write: "token" } }, null, "open"))
  vi.spyOn(api, "requestPayout").mockResolvedValue(paid)
  await render()
  expect(submitButton().disabled).toBe(true)
  await act(async () => submit())
  expect(api.requestPayout).not.toHaveBeenCalled()
  await act(async () => { writeToken.set("fixture-token") })
  expect(submitButton().disabled).toBe(false)
  await act(async () => submit())
  expect(api.requestPayout).toHaveBeenCalledWith("3000.00", "token")
})
