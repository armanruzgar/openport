// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import { account, fundedAccount, plans, risk, status, twoStepPlans } from "../test/trading-fixtures"
import { NewAccountDialog } from "./AccountSwitcher"
import { ResetDialog } from "./ResetDialog"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const features = vi.hoisted(() => ({ showFundedAccounts: false }))
vi.mock("../lib/features", () => features)
let host: HTMLDivElement
let root: Root
let client: QueryClient
beforeEach(() => {
  features.showFundedAccounts = false
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity } } })
  client.setQueryData(["plans"], { plans })
  client.setQueryData(tradingQueries(0, "17", true).account.queryKey, account)
  client.setQueryData(tradingQueries(0, "17", true).risk.queryKey, risk)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  vi.spyOn(api, "resetAccount").mockResolvedValue(account)
})
it("labels the Practice option as Practice in the reset dialog", async () => {
  await act(async () => root.render(<QueryClientProvider client={client}>
    <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} initial="practice" onClose={() => {}} />
  </QueryClientProvider>))
  const option = host.querySelector('input[value="practice"]')?.closest("label")
  expect(option?.textContent).toContain("Practice")
  expect(option?.querySelector(".text-faint")?.textContent).toContain("Practice")
  expect(option?.textContent).not.toContain("Evaluation")
})
it.each(["reset", "create"])("submits itemized fees when starting an account attempt (%s)", async (kind) => {
  vi.spyOn(api, "createAccount").mockResolvedValue({ account: { id: "fees", name: "Fees", account_version: "1", plan: "Practice", equity: "100000.00" } })
  const plan = plans.find((entry) => entry.id === "intraday-100k")!
  await act(async () => root.render(<QueryClientProvider client={client}>
    {kind === "reset" ? <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} initial={plan.id} onClose={() => {}} />
      : <NewAccountDialog trading={{ ...status.trading!, write: "open" }} onClose={() => {}} onCreated={() => {}} />}
  </QueryClientProvider>))
  if (kind === "create") {
    await act(async () => host.querySelector<HTMLInputElement>(`input[value="${plan.id}"]`)!.click())
  }
  const select = host.querySelector<HTMLSelectElement>('select[aria-label="Fees"]')!
  expect(select.value).toBe("flat")
  await act(async () => { select.value = "itemized"; select.dispatchEvent(new Event("change", { bubbles: true })) })
  expect(host.textContent).toContain("$10 per leg per order")
  expect(host.textContent).toContain("$5 per contract")
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  expect(kind === "reset" ? api.resetAccount : api.createAccount).toHaveBeenCalledWith(
    expect.objectContaining({ plan: plan.id, fee_model: "itemized" }), "open")
})
it.each(["reset", "create"])("submits midpoint fills (%s)", async (kind) => {
  vi.spyOn(api, "createAccount").mockResolvedValue({ account: { id: "mid", name: "Mid", account_version: "1", plan: "Practice", equity: "100000.00" } })
  const plan = plans.find((entry) => entry.id === "intraday-100k")!
  await act(async () => root.render(<QueryClientProvider client={client}>
    {kind === "reset" ? <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} initial={plan.id} onClose={() => {}} />
      : <NewAccountDialog trading={{ ...status.trading!, write: "open" }} onClose={() => {}} onCreated={() => {}} />}
  </QueryClientProvider>))
  if (kind === "create") { await act(async () => host.querySelector<HTMLInputElement>(`input[value="${plan.id}"]`)!.click()) }
  const select = host.querySelector<HTMLSelectElement>('select')!
  await act(async () => { select.value = "midpoint"; select.dispatchEvent(new Event("change", { bubbles: true })) })
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  expect(kind === "reset" ? api.resetAccount : api.createAccount).toHaveBeenCalledWith(expect.objectContaining({ fill_model: "midpoint" }), "open")
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  vi.restoreAllMocks(); vi.clearAllMocks(); vi.unstubAllGlobals()
})
it.each([false, true])("submits the selected fill preset, preserving the default request (%s)", async (conservative) => {
  const plan = plans.find((entry) => entry.id === "intraday-100k")!
  await act(async () => root.render(<QueryClientProvider client={client}>
    <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} initial={plan.id} onClose={() => {}} />
  </QueryClientProvider>))
  const select = host.querySelector("select")!
  expect(select.value).toBe("as_displayed")
  if (conservative) {
    await act(async () => { select.value = "conservative"; select.dispatchEvent(new Event("change", { bubbles: true })) })
    expect(host.textContent).toContain("wait 1,000 ms on market time")
    expect(host.textContent).toContain("1 extra tick")
  }
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  expect(api.resetAccount).toHaveBeenCalledWith({ plan: plan.id, reason: `Start ${plan.name}`,
    ...(conservative ? { fill_model: "conservative" } : {}) }, "open")
})
it("submits the margin a trader picks beside the plan, only where it differs from the plan's", async () => {
  const plan = plans.find((entry) => entry.id === "intraday-100k")!
  await act(async () => root.render(<QueryClientProvider client={client}>
    <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} initial={plan.id} onClose={() => {}} />
  </QueryClientProvider>))
  const choose = async (label: string, value: string) => {
    const field = [...host.querySelectorAll("label")].find((l) => l.textContent?.startsWith(label))!.querySelector("select, input") as HTMLSelectElement | HTMLInputElement
    const setter = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(field), "value")!.set!
    await act(async () => { setter.call(field, value); field.dispatchEvent(new Event(field.tagName === "SELECT" ? "change" : "input", { bubbles: true })) })
    return field
  }
  const shock = () => [...host.querySelectorAll("label")].find((l) => l.textContent?.startsWith("Portfolio IV shock"))!.querySelector("input")!
  expect(shock().disabled).toBe(true)
  await choose("Margin", "portfolio")
  expect(shock().disabled).toBe(false)
  await choose("Portfolio IV shock", "5")
  await choose("House margin %", "25")
  expect(host.textContent).toContain("House margin adds 25% to the scan")
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  expect(api.resetAccount).toHaveBeenLastCalledWith({ plan: plan.id, reason: `Start ${plan.name}`,
    margin: "portfolio", house_margin_percent: 25, pm_vol_shock: 5 }, "open")
  // A cash account keeps strategy margin, and its shock drops.
  const margin = await choose("Account type", "cash") as HTMLSelectElement
  expect(margin.value).toBe("cash")
  expect(host.textContent).toContain("short puts secured with their strike")
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  expect(api.resetAccount).toHaveBeenLastCalledWith({ plan: plan.id, reason: `Start ${plan.name}`,
    account_type: "cash", house_margin_percent: 25 }, "open")
})

it("creates an account with the selected margin settings", async () => {
  vi.spyOn(api, "createAccount").mockResolvedValue({ account: { id: "new", name: "Account 2", account_version: "1", plan: "Practice", equity: "100000.00" } })
  const created = vi.fn()
  await act(async () => root.render(<QueryClientProvider client={client}>
    <NewAccountDialog trading={{ ...status.trading!, write: "open" }} onCreated={created} onClose={() => {}} />
  </QueryClientProvider>))
  await act(async () => (host.querySelector('input[value="practice"]') as HTMLInputElement).click())
  const type = host.querySelector('option[value="ira"]')!.closest("select")!
  await act(async () => { type.value = "ira"; type.dispatchEvent(new Event("change", { bubbles: true })) })
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  expect(api.createAccount).toHaveBeenCalledWith(expect.objectContaining({ plan: "practice", account_type: "ira" }), "open")
  expect(created).toHaveBeenCalledWith("new")
})

it("inherits a plan's margin settings until the trader changes them", async () => {
  const plan = { ...plans.find((entry) => entry.id === "practice")!, rules: {
    ...plans.find((entry) => entry.id === "practice")!.rules, margin: "portfolio" as const, house_margin_percent: 30, pm_vol_shock: 5,
  } }
  client.setQueryData(["plans"], { plans: [plan] })
  await act(async () => root.render(<QueryClientProvider client={client}>
    <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} initial={plan.id} onClose={() => {}} />
  </QueryClientProvider>))
  expect(host.textContent).toContain("House margin adds 30% to the scan")
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  expect(api.resetAccount).toHaveBeenCalledWith({ plan: plan.id, reason: `Start ${plan.name}` }, "open")
})


it.each([
  ["100000", null, true], ["100001", null, true], ["99999.999999", null, false],
  ["200000", "100000", true], ["200000", "99999.999999", false], ["200000", "0", false],
])("warns before reset using the effective absolute floor (%s, pending %s)", async (floor, pending, warns) => {
  const g = { soft_floor: floor as string, soft_floor_percent: 0, max_opening_trades: 0, cooldown_loss: "0", cooldown_minutes: 0, profit_lock: "0" }
  client.setQueryData(tradingQueries(0, "17", true).risk.queryKey,
    { ...risk, guardrails: g, pending_guardrails: pending == null ? null : { ...g, soft_floor: pending } })
  await act(async () => root.render(<QueryClientProvider client={client}>
    <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} initial="intraday-100k" onClose={() => {}} />
  </QueryClientProvider>))
  expect(host.querySelector('[role="alert"]')?.textContent?.includes("soft floor will latch at once") ?? false).toBe(warns)
  expect(host.textContent).toContain("Manual and daily-loss latches clear")
  if (warns) {
    expect(host.textContent).toContain("a lower setting is pending until rollover, and a reset applies pending settings")
  }
  // A warning is informative; the trader can still start this attempt.
  expect(host.querySelector<HTMLButtonElement>('button[type="submit"]')!.disabled).toBe(false)
  expect(host.textContent).toContain("opening-order count and active cooldown carry over")
})

it("quotes the same-plan reset fee and disables an exhausted reset", async () => {
  const charged = { ...plans[1]!, rules: { ...account.rules, reset_fee: "50.00", max_resets: 2 } }
  client.setQueryData(["plans"], { plans: [charged] })
  client.setQueryData(tradingQueries(0, "17", true).account.queryKey, { ...account, rules: charged.rules, costs: {
    evaluation: "100", reset: "100", activation: "0", total: "200", resets_used: 2, resets_left: 0,
    payouts_received: "0", net: "-200", fee_charged: "50", fee_kind: "reset" } })
  await act(async () => root.render(<QueryClientProvider client={client}>
    <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={3} initial={charged.id} onClose={() => {}} />
  </QueryClientProvider>))
  expect(host.textContent).toContain("This start charges $50.00 (reset fee)")
  expect(host.textContent).toContain("Reset limit reached")
  expect(host.querySelector<HTMLButtonElement>('button[type="submit"]')!.disabled).toBe(true)
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  expect(api.resetAccount).not.toHaveBeenCalled()
})

it.each([
  ["reset", false], ["reset", true], ["create", false], ["create", true],
] as const)("shows locked verification with its prerequisite (%s, funded=%s)", async (kind, showFunded) => {
  features.showFundedAccounts = showFunded
  const [challenge, verification, funded] = twoStepPlans
  client.setQueryData(["plans"], { plans: twoStepPlans })
  await act(async () => root.render(<QueryClientProvider client={client}>
    {kind === "reset" ? <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} onClose={() => {}} />
      : <NewAccountDialog trading={{ ...status.trading!, write: "open" }} onClose={() => {}} onCreated={() => {}} />}
  </QueryClientProvider>))
  expect(host.textContent).toContain("Step 2 of 2: verification")
  expect(host.querySelector<HTMLInputElement>(`input[value="${verification.id}"]`)!.disabled).toBe(true)
  expect(host.textContent).toContain(`Pass ${challenge.name} to unlock`)
  const fundedChoice = host.querySelector<HTMLInputElement>(`input[value="${funded.id}"]`)
  if (showFunded) {
    expect(fundedChoice!.disabled).toBe(true)
    expect(host.textContent).toContain(`Pass ${verification.name} to unlock`)
    expect(host.textContent).toContain("activation $200.00")
  } else {
    expect(fundedChoice).toBeNull()
    expect(host.textContent).not.toContain("activation $200.00")
  }
})

it.each([false, true])("confirms an unlocked verification through the ordinary reset route (funded=%s)", async (showFunded) => {
  features.showFundedAccounts = showFunded
  const verification = { ...plans[1]!, id: "verify", name: "Verification 100K", unlocked_by: plans[1]!.id,
    rules: { ...account.rules, phase: "verification" as const, activation_fee: "25.00" } }
  client.setQueryData(["plans"], { plans: [...plans, verification] })
  client.setQueryData(tradingQueries(0, "17", true).account.queryKey,
    { ...account, next_plans: ["verify"], evaluation: { ...account.evaluation, status: "passed" } })
  await act(async () => root.render(<QueryClientProvider client={client}>
    <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} initial="verify" onClose={() => {}} />
  </QueryClientProvider>))
  expect(host.textContent).toContain("This start charges $25.00 (activation fee)")
  expect(api.resetAccount).not.toHaveBeenCalled()
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  expect(api.resetAccount).toHaveBeenCalledWith(expect.objectContaining({ plan: "verify" }), "open")
})

it.each([false, true])("gates the funded activation quote and confirmation after verification (funded=%s)", async (showFunded) => {
  features.showFundedAccounts = showFunded
  const [, verification, funded] = twoStepPlans
  client.setQueryData(["plans"], { plans: twoStepPlans })
  client.setQueryData(tradingQueries(0, "17", true).account.queryKey,
    { ...account, rules: verification.rules, next_plans: [funded.id], evaluation: { ...account.evaluation, status: "passed" } })
  await act(async () => root.render(<QueryClientProvider client={client}>
    <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} initial={funded.id} onClose={() => {}} />
  </QueryClientProvider>))
  expect(api.resetAccount).not.toHaveBeenCalled()
  expect(host.querySelector<HTMLButtonElement>('button[type="submit"]')!.disabled).toBe(!showFunded)
  if (showFunded) {
    expect(host.querySelector<HTMLInputElement>(`input[value="${funded.id}"]`)!.checked).toBe(true)
    expect(host.textContent).toContain("This start charges $200.00 (activation fee)")
  } else {
    expect(host.textContent).not.toContain(funded.name)
    expect(host.textContent).not.toContain("activation $200.00")
    expect(host.textContent).not.toContain("This start charges")
  }
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  if (showFunded) expect(api.resetAccount).toHaveBeenCalledWith(expect.objectContaining({ plan: funded.id }), "open")
  else expect(api.resetAccount).not.toHaveBeenCalled()
})

it("shows funded choices for an already-funded account without allowing a new account to skip the prerequisite", async () => {
  const [, verification, funded] = twoStepPlans
  client.setQueryData(["plans"], { plans: twoStepPlans })
  client.setQueryData(tradingQueries(0, "17", true).account.queryKey, { ...fundedAccount, rules: funded.rules })
  await act(async () => root.render(<QueryClientProvider client={client}>
    <NewAccountDialog trading={{ ...status.trading!, write: "open" }} onClose={() => {}} onCreated={() => {}} />
  </QueryClientProvider>))
  expect(host.querySelector<HTMLInputElement>(`input[value="${funded.id}"]`)!.disabled).toBe(true)
  expect(host.textContent).toContain(`Pass ${verification.name} to unlock`)
  expect(host.textContent).toContain("activation $200.00")
})
