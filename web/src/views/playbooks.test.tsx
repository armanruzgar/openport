// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, type ReactNode } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { PlaybooksResponse, PlaybookStats } from "../api/playbook-types"
import type { BacktestReport } from "../api/backtest-types"
import { AutoPlaybookIndicator, PassOddsCard, StagedOrders } from "../components/Playbooks"
import { trades, status } from "../test/trading-fixtures"
import { JournalView } from "./JournalView"
import { newPlaybook, PlaybooksView } from "./PlaybooksView"
import { renderTimeout, waitForRender } from "../test/render"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../charts/useSize", () => ({ useSize: () => [{ current: null }, { width: 700, height: 200 }] }))
const stats: PlaybookStats = { trades: 2, win_rate: .5, average_win: "100", average_loss: "-50", expectancy: "25", profit_factor: 2, no_losses: false, average_r: .25, adherence: .8 }
function catalogue(): PlaybooksResponse {
  return { definitions: { "put-spread": { versions: [{ ...newPlaybook, version: 1 }], deleted: false } }, modes: { "put-spread": "stage" }, auto_allowed: true,
    reasons: { "put-spread:SPX": "Ready" }, reports: { "put-spread": { all: stats, followed: stats, deviated: { ...stats, trades: 0 }, trades: [] } },
    staged: [{ id: "stage-1", playbook: "put-spread", version: 1, name: "Morning put spread", underlying: "SPX", units: 2, net: "-1.20", max_loss: "760", max_loss_basis: "expiry_payoff", close_by: "2026-09-22T19:45:00Z", simulated: true,
      legs: [{ symbol: "synthetic-put", side: "sell", ratio: 1, strike: 5900, type: "put", expiry: "2026-09-22PM" }] }] }
}
let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  vi.spyOn(api, "backtests").mockResolvedValue({ runs: [], active: null, label: "Simulated" })
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, gcTime: Infinity, staleTime: Infinity } } })
  vi.spyOn(api, "playbooks").mockResolvedValue(catalogue())
  vi.spyOn(api, "passOdds").mockRejectedValue(new Error("Pass odds need at least 10 completed days with recorded intraday lows and highs"))
  vi.spyOn(api, "savePlaybook").mockResolvedValue(catalogue())
  vi.spyOn(api, "playbookMode").mockResolvedValue(catalogue())
  vi.spyOn(api, "stagedAction").mockImplementation(async () => { const value = { ...catalogue(), staged: [] }; vi.mocked(api.playbooks).mockResolvedValue(value); return value })
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); client.clear(); vi.restoreAllMocks(); vi.unstubAllGlobals() })
async function render(node: ReactNode) { await act(async () => root.render(<QueryClientProvider client={client}>{node}</QueryClientProvider>)) }
function button(text: string) { const found = [...host.querySelectorAll("button")].find((item) => item.textContent === text); if (!found) throw new Error(`Missing ${text}`); return found }
async function click(text: string) { await act(async () => button(text).click()) }
async function value(selector: string, text: string) {
  const field = host.querySelector(selector) as HTMLTextAreaElement | HTMLSelectElement
  const prototype = field instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLSelectElement.prototype
  await act(async () => { Object.getOwnPropertyDescriptor(prototype, "value")!.set!.call(field, text); field.dispatchEvent(new Event(field instanceof HTMLTextAreaElement ? "input" : "change", { bubbles: true })) })
}
describe("Playbooks page and staged actions", { timeout: renderTimeout }, () => {
  it.each(["page", "shared card"])("renders preview warnings on the %s", async (surface) => {
    const saved = catalogue()
    const message = "The stop's trigger, at or above 0.40, is already reached (now 0.50): it fires as soon as the entry fills"
    saved.staged[0]!.warnings = [{ code: "STOP_REACHED", message }]
    vi.mocked(api.playbooks).mockResolvedValue(saved)
    await render(surface === "page" ? <PlaybooksView /> : <StagedOrders />)
    await waitForRender(() => expect(host.textContent).toContain(message))
    const warning = [...host.querySelectorAll('[role="status"]')].find((node) => node.textContent?.includes(message))
    expect(warning?.classList.contains("text-warn")).toBe(true)
    expect(api.stagedAction).not.toHaveBeenCalled()
  })
  it("shows the additional written exits and preserves technical rules in the editor", async () => {
    const saved = catalogue()
    const definition = saved.definitions["put-spread"]!.versions[0]!
    definition.conditions = { technical: [{ indicator: "rsi", interval: "minute", period: 14, min: 30, max: 70 }],
      gap: { min_percent: -2, max_percent: 2 }, vix: { min: 10, max: 30 } }
    definition.management = { close_by: "15:45", trailing_stop: { percent: 25 }, close_at_dte: 1, max_days_in_trade: 2, stop_loss_percent: 40 }
    saved.staged[0]!.management = definition.management
    vi.mocked(api.playbooks).mockResolvedValue(saved)
    await render(<><PlaybooksView /><StagedOrders /></>)
    await waitForRender(() => expect(host.textContent).toContain("Trailing stop: 25%"))
    for (const text of ["40% debit loss", "Close at 1 calendar DTE", "Close after 2 trading days"]) expect(host.textContent).toContain(text)
    await click("Edit Morning put spread")
    const editor = host.querySelector("textarea")!
    expect(JSON.parse(editor.value).conditions).toEqual(definition.conditions)
    expect(JSON.parse(editor.value).management).toEqual(definition.management)
  })
  it("shows saved versions, safe live modes and followed-versus-deviated statistics", async () => {
    await render(<PlaybooksView />)
    await waitForRender(() => expect(host.textContent).toContain("Morning put spread"))
    expect(host.textContent).toContain("Morning put spread · v1")
    expect(host.querySelector<HTMLOptionElement>('option[value="auto"]')?.disabled).toBe(false)
    await click("Stats and versions")
    await waitForRender(() => expect(host.textContent).toContain("at least 10 completed days"))
    for (const text of ["Followed rules", "Deviated", "Expectancy", "80.0%", "Saved versions", "Estimate from past results, not a prediction", "at least 10 completed days"]) expect(host.textContent).toContain(text)
    expect(api.passOdds).toHaveBeenCalledWith(20, 1000, "put-spread", expect.any(AbortSignal))
  })
  it("confirms the named live account before enabling Auto and allows cancellation", async () => {
    vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"), account: "named-paper" })
    await render(<PlaybooksView />)
    await waitForRender(() => expect(host.textContent).toContain("Morning put spread"))
    await value('select[aria-label="Mode for Morning put spread"]', "auto")
    expect(api.playbookMode).not.toHaveBeenCalled()
    expect(host.querySelector("dialog")?.textContent).toContain("named-paper")
    expect(host.querySelector("dialog")?.textContent).toContain("Orders will be sent automatically on paper")
    await click("Cancel")
    expect(host.querySelector("dialog")).toBeNull()
    expect(api.playbookMode).not.toHaveBeenCalled()
    await value('select[aria-label="Mode for Morning put spread"]', "auto")
    await click("Enable Auto")
    expect(api.playbookMode).toHaveBeenCalledWith("put-spread", "auto", "open")
    expect(host.querySelector("dialog")).toBeNull()
  })
  it("keeps replay Auto direct and does not fetch live backtest comparisons", async () => {
    vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"), source: "replay" })
    await render(<PlaybooksView />)
    await waitForRender(() => expect(host.textContent).toContain("Morning put spread"))
    await value('select[aria-label="Mode for Morning put spread"]', "auto")
    expect(api.playbookMode).toHaveBeenCalledWith("put-spread", "auto", "open")
    expect(host.querySelector("dialog")).toBeNull()
    expect(api.backtests).not.toHaveBeenCalled()
  })
  it("shows forward windows, stats and only the latest completed backtest of the same version", async () => {
    const saved = catalogue()
    saved.modes["put-spread"] = "auto"
    saved.forward_tests = { "put-spread": { running: true, days_running: 2.5, entries: 3, time_stops: 2, rejected_entries: 1,
      windows: [{ account: "main", playbook: "put-spread", version: 1, started: "2026-09-22T13:30:00Z", ended: null, actor: "alice", first_order: "1", end_order: null }],
      report: saved.reports!["put-spread"], versions: { "1": saved.reports!["put-spread"]! } } }
    vi.mocked(api.playbooks).mockResolvedValue(saved)
    const summary = { trades: 10, win_rate: .6, expectancy: "42", average_return_on_buying_power: .1 } as BacktestReport["summary"]
    const run = { id: "000001", status: "completed" as const, phase: "done", completed: 1, total: 1, label: "Simulated", report: null, playbook: { id: "put-spread", version: 1 }, summary }
    vi.mocked(api.backtests).mockResolvedValue({ active: null, label: "Simulated", runs: [
      { ...run, id: "000005", playbook: { id: "put-spread", version: 2 } },
      { ...run, id: "000004", playbook: { id: "other", version: 1 } },
      { ...run, id: "000003", status: "failed" }, { ...run, id: "000002" }, run,
    ] })
    await render(<><PlaybooksView /><AutoPlaybookIndicator /></>)
    await waitForRender(() => expect(host.textContent).toContain("Latest saved backtest 000002"))
    for (const text of ["Forward test · Running", "2.50 elapsed market days", "3 entries", "2 time stops", "1 rejected entries", "alice", "Auto paper trading · 1 playbook on main", "All forward trades", "60.0%", "42"]) expect(host.textContent).toContain(text)
    expect(host.textContent).not.toContain("000005")
  })
  it("compares a matching joint member using its own statistics", async () => {
    const summary = { trades: 100, win_rate: .9, expectancy: "999", per_playbook: {
      "put-spread": { trades: 3, open_trades: 0, win_rate: .5, expectancy: "12.34", adherence: 1,
        realised_pnl: "37.02", marked_pnl: "37.02", entry_reasons: {}, rule_trips: [] },
    } } as unknown as BacktestReport["summary"]
    vi.mocked(api.backtests).mockResolvedValue({ active: null, label: "Simulated", runs: [{ id: "000009", status: "completed", phase: "finished",
      completed: 1, total: 1, label: "Simulated", report: null, mode: "joint",
      playbooks: [{ id: "other", version: 1 }, { id: "put-spread", version: 1 }], summary }] })
    await render(<PlaybooksView />)
    await waitForRender(() => expect(host.textContent).toContain("Latest saved backtest 000009"))
    expect(host.textContent).toContain("Joint backtest")
    expect(host.textContent).toContain("$12.34")
    expect(host.textContent).not.toContain("$999")
  })
  it("supports older publications and explains a missing same-version backtest", async () => {
    await render(<PlaybooksView />)
    await waitForRender(() => expect(host.textContent).toContain("Forward test · Stopped"))
    expect(host.textContent).toContain("No completed saved backtest for this version")
    expect(host.textContent).toContain("Forward-test windows (0)")
  })
  it("keeps archived statistics and versions readable without offering new entries", async () => {
    const saved = catalogue()
    saved.definitions["put-spread"]!.deleted = true
    saved.staged = []
    vi.mocked(api.playbooks).mockResolvedValue(saved)
    await render(<PlaybooksView />)
    await waitForRender(() => expect(host.textContent).toContain("Morning put spread"))
    expect(host.querySelector('select[aria-label="Mode for Morning put spread"]')).toBeNull()
    await click("Morning put spread · archived stats and versions")
    expect(host.textContent).toContain("Saved versions")
    expect(host.textContent).toContain("Followed rules")
    expect(host.textContent).toContain("Version 1")
  })
  it("saves an edited version and displays JSON errors without losing the editor", async () => {
    await render(<PlaybooksView />)
    await waitForRender(() => expect(host.textContent).toContain("Morning put spread"))
    await click("Edit Morning put spread")
    await value('textarea[aria-label="Playbook definition"]', "{")
    await click("Save version")
    expect(host.querySelector('[role="alert"]')).not.toBeNull()
    expect(api.savePlaybook).not.toHaveBeenCalled()
    const edit = { ...newPlaybook, version: 1, name: "Changed setup" }
    await value('textarea[aria-label="Playbook definition"]', JSON.stringify(edit))
    await click("Save version")
    expect(api.savePlaybook).toHaveBeenCalledWith(edit, "open")
    expect(host.querySelector("textarea")).toBeNull()
  })
  it("stages on the selected account without sending when its mode changes", async () => {
    await render(<PlaybooksView />)
    await waitForRender(() => expect(host.textContent).toContain("Morning put spread"))
    await value('select[aria-label="Mode for Morning put spread"]', "stage")
    expect(api.playbookMode).toHaveBeenCalledWith("put-spread", "stage", "open")
    expect(api.stagedAction).not.toHaveBeenCalled()
  })
  it.each(["send", "dismiss"] as const)("%s uses the server stage token and removes the stage after refresh", async (action) => {
    await render(<StagedOrders />)
    await waitForRender(() => expect(host.textContent).toContain("Simulated orders"))
    expect(host.textContent).toContain("Simulated orders")
    expect(host.textContent).toContain("playbook:put-spread@v1")
    await click(`${action === "send" ? "Send" : "Dismiss"} Morning put spread`)
    expect(api.stagedAction).toHaveBeenCalledWith("stage-1", action, "open")
    await waitForRender(() => expect(host.textContent).not.toContain("Staged playbook orders"))
  })
  it("explains stale-stage refusal and keeps the order unsent", async () => {
    vi.mocked(api.stagedAction).mockRejectedValue(new Error("Staged order expired or changed; refresh playbooks"))
    await render(<StagedOrders />)
    await waitForRender(() => expect(host.textContent).toContain("Simulated orders"))
    await click("Send Morning put spread")
    expect(host.querySelector('[role="alert"]')?.textContent).toContain("expired or changed")
  })
  it("shows pass, fail, neither, history count and seed as estimates", async () => {
    vi.mocked(api.passOdds).mockResolvedValue({ pass: .25, fail: .1, neither: .65, median_days_to_pass: 9, historical_days: 15, seed: "81723", days: 20, samples: 1000, label: "Estimate from past results, not a prediction", history_basis: "Account equity days", path_assumption: "Observed extrema only", simulated: true })
    await render(<PassOddsCard />)
    await waitForRender(() => expect(host.textContent).toContain("15 historical days"))
    for (const text of ["25.0%", "10.0%", "65.0%", "Median days to pass: 9", "15 historical days", "seed 81723", "not a prediction"]) expect(host.textContent).toContain(text)
  })
  it("filters Journal across all versions of a playbook while retaining other filters", async () => {
    vi.spyOn(api, "trades").mockResolvedValue({ account_version: "17", attempt: 1, trades: [
      { ...trades[2]!, id: "pb1", tags: ["playbook:put-spread@v1"] },
      { ...trades[2]!, id: "pb2", tags: ["playbook:put-spread@v2"] },
      { ...trades[2]!, id: "other", tags: ["playbook:other@v1"] },
    ], share_trades: [], day_notes: {} })
    vi.spyOn(api, "orders").mockResolvedValue({ account_version: "17", orders: [] })
    vi.spyOn(api, "fills").mockResolvedValue({ account_version: "17", fills: [] })
    await render(<JournalView />)
    await waitForRender(() => expect(host.textContent).toContain("3 closed trades"))
    await value('select[aria-label="Playbook"]', "put-spread")
    expect(host.textContent).toContain("2 closed trades")
    await value('select[aria-label="Tag"]', "playbook:put-spread@v1")
    expect(host.textContent).toContain("1 closed trade")
  })
})
