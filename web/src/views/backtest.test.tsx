// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { BacktestComparison, BacktestReport, BacktestState } from "../api/backtest-types"
import { plans, status } from "../test/trading-fixtures"
import { newPlaybook } from "./PlaybooksView"
import { BacktestView, BacktestReportView, BacktestCompareView, backtestHistogram } from "./BacktestView"
import { dataSource } from "../lib/data-source"
import { renderTimeout, waitForRender } from "../test/render"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../charts/useSize", () => ({ useSize: () => [{ current: null }, { width: 700, height: 200 }] }))
const label = "Simulated trading on recorded or generated days. Not a prediction. Not investment advice."
const state: BacktestState = { id: "000001", status: "running", phase: "days", completed: 1, total: 4, label, report: null }
const distribution = { values: ["-20.00", "40.00"], min: "-20.00", p25: "-20.00", median: "-20.00", p75: "-20.00", max: "40.00", mean: "10.00" }
const result = { started: "2026-09-14T13:30:00Z", ended: "2026-09-14T20:15:00Z", pnl: "40.00", last_mark_pnl: "40.00", valuation_complete: true, quality_flags: [], max_drawdown: "20.00", min_floor_distance: "80.00", outcome: "open" as const,
  decision: "", rule_trips: [], trades: [], fills: [], stock_fills: [], stock_trades: [], adherence: null, entry_reasons: { "test:SPX": "IV rank unavailable (no IV history)" }, open_positions: 0, journal: "days/000001.jsonl", journal_head: "abc" }
const report: BacktestReport = { schema: 1, simulated: true, label, status: "completed", playbook: newPlaybook,
  days: [{ ...result, date: "2026-09-14", input: { kind: "scenario", id: "fade", seed: "18446744073709551610" } }],
  attempts: [{ ...result, first_day: 0, last_day: 0, days: 1 }], errors: [],
  summary: { daily_pnl: distribution, daily_drawdown: { ...distribution, values: ["20.00"], min: "20.00", max: "20.00" }, worst_days: [0], completed_days: 1, marked_days: 1,
    trades: 0, expectancy: null, win_rate: null, day_win_rate: 1, attempts: 1, passed: 0, failed: 0, open: 1, pass_rate: null } }
const comparison: BacktestComparison = {
  label, different_inputs: true, different_plans: false, incomplete_inputs: false, apples_to_oranges: true,
  runs: ["000001", "000002"].map((id) => ({ id, status: "completed", playbook: { id: "test", version: 1 }, plan: { rules: { plan: "eod-50k" } }, input_set: [], summary: report.summary })),
  daily: [{ date: "2026-09-14", pnl: { "000001": "40.00", "000002": null } }],
  combined: { label: "Sum of independent single-playbook days. No shared buying power, risk limits or plan floor; not a joint account simulation.",
    daily_pnl: distribution, curve: [{ date: "2026-09-14", pnl: "40.00", cumulative: "40.00", contributors: 1 }], max_drawdown: "0.00", worst_days: [], day_win_rate: 1 },
}
let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, gcTime: Infinity } } })
  vi.spyOn(api, "backtestPlaybooks").mockResolvedValue({ definitions: { test: { versions: [{ ...newPlaybook, id: "test", version: 1 }], deleted: false } }, modes: {}, auto_allowed: false, staged: [], reasons: {} })
  vi.spyOn(api, "backtestPlans").mockResolvedValue({ plans })
  vi.spyOn(api, "replay").mockResolvedValue({ directory: "recordings", recordings: [{ file: "second.oprec", bytes: 1, started: "2026-09-15" }, { file: "first.oprec", bytes: 1, started: "2026-09-14" }], replay: null, demos: [] })
  vi.spyOn(api, "backtests").mockResolvedValue({ runs: [], active: null, label })
  vi.spyOn(api, "backtest").mockResolvedValue(state)
  vi.spyOn(api, "startBacktest").mockResolvedValue(state)
  vi.spyOn(api, "compareBacktests").mockResolvedValue(comparison)
  vi.spyOn(api, "keepBacktest").mockResolvedValue({ ...state, keep: true })
  vi.spyOn(api, "deleteBacktest").mockResolvedValue({ id: state.id, deleted: true })
  vi.spyOn(api, "cancelBacktest").mockResolvedValue({ ...state, status: "cancelling" })
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); client.clear(); vi.restoreAllMocks(); vi.unstubAllGlobals(); dataSource.set("live") })
async function render(node = <BacktestView />) {
  await act(async () => root.render(<QueryClientProvider client={client}>{node}</QueryClientProvider>))
  if (node.type === BacktestView) await waitForRender(() => {
    expect(host.querySelector('option[value="test@1"]')).not.toBeNull()
    expect(host.querySelector('option[value="intraday-100k"]')).not.toBeNull()
  })
}
function button(text: string) { const found = [...host.querySelectorAll("button")].find((item) => item.textContent === text); if (!found) throw new Error(`Missing ${text}`); return found }
async function click(text: string) { await act(async () => button(text).click()) }
async function field(labelText: string, text: string) {
  const labelElement = [...host.querySelectorAll("label")].find((item) => item.firstChild?.textContent === labelText)!
  const fieldElement = labelElement.querySelector("input,select,textarea")!
  const prototype = fieldElement instanceof HTMLSelectElement ? HTMLSelectElement.prototype : fieldElement instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype
  await act(async () => { Object.getOwnPropertyDescriptor(prototype, "value")!.set!.call(fieldElement, text); fieldElement.dispatchEvent(new Event(fieldElement instanceof HTMLSelectElement ? "change" : "input", { bubbles: true })) })
}
describe("Backtest page", { timeout: renderTimeout }, () => {
  it("pins the selected version and preserves the entire seed string", async () => {
    await render(); await field("Starting seed", "18446744073709551610"); await field("Day count", "2"); await field("Evaluation plan", "intraday-100k"); await click("Start backtest")
    expect(api.startBacktest).toHaveBeenCalledWith({ playbook: "test@1", plan: "intraday-100k", scenarios: 2, seed: "18446744073709551610" }, "open")
    expect(vi.mocked(api.backtest).mock.calls.every(([id]) => id !== "")).toBe(true)
    expect(host.textContent).toContain("Not investment advice")
    expect(host.textContent).not.toContain("Funded Intraday")
  })
  it("adds distinct playbooks for a joint start and can return to a single run", async () => {
    vi.mocked(api.backtestPlaybooks).mockResolvedValue({ definitions: {
      test: { versions: [{ ...newPlaybook, id: "test", version: 1 }, { ...newPlaybook, id: "test", version: 2 }], deleted: false },
      alpha: { versions: [{ ...newPlaybook, id: "alpha", name: "Alpha", version: 1 }], deleted: false },
    }, modes: {}, auto_allowed: false, staged: [], reasons: {} })
    await render(); await click("Add playbook")
    expect(button("Start backtest").disabled).toBe(true)
    const select = host.querySelectorAll<HTMLSelectElement>("select")[3]!
    expect([...select.options].find(option => option.value === "test@2")?.disabled).toBe(true)
    await field("Playbook version 2", "alpha@1"); await click("Start backtest")
    expect(api.startBacktest).toHaveBeenLastCalledWith({ playbooks: ["test@1", "alpha@1"], plan: "eod-50k", scenarios: 10, seed: "1" }, "open")
    expect(host.textContent).toContain("ascending playbook ID")
    await click("Remove playbook 2"); await click("Start backtest")
    expect(api.startBacktest).toHaveBeenLastCalledWith({ playbook: "test@1", plan: "eod-50k", scenarios: 10, seed: "1" }, "open")
  })
  it("renders joint identities and option attribution alongside account results", async () => {
    const joint: BacktestReport = { ...report, schema: 3, mode: "joint", playbook: undefined,
      playbooks: [{ ...newPlaybook, id: "alpha", name: "Alpha", version: 2 }, { ...newPlaybook, id: "beta", name: "Beta", version: 1 }],
      summary: { ...report.summary, per_playbook: {
        alpha: { trades: 1, open_trades: 0, expectancy: "40.00", win_rate: 1, adherence: 1, realised_pnl: "40.00", marked_pnl: "40.00", entry_reasons: {}, rule_trips: [] },
        beta: { trades: 0, open_trades: 0, expectancy: null, win_rate: null, adherence: null, realised_pnl: "0.00", marked_pnl: "0.00", entry_reasons: { "beta:SPX": "Maximum held contracts exceeded" }, rule_trips: [] },
      } } }
    joint.days = [{ ...report.days[0]!, per_playbook: joint.summary.per_playbook }]
    vi.mocked(api.backtests).mockResolvedValue({ active: null, label, runs: [{ ...state, status: "completed", mode: "joint", playbooks: joint.playbooks, summary: joint.summary }] })
    vi.mocked(api.backtest).mockResolvedValue({ ...state, status: "completed", report: joint })
    await render()
    await waitForRender(() => expect(host.textContent).toContain("Joint account backtest"))
    expect(host.querySelector('table')?.textContent).toContain("Joint · Alpha@2, Beta@1")
    expect(host.querySelectorAll('[aria-label="Per-playbook breakdown"]')).toHaveLength(2)
    expect(host.textContent).toContain("Maximum held contracts exceeded")
    expect(host.textContent).toContain("Evaluation attempts")
    expect(host.textContent).toContain("Independent daily results")
  })
  it("shows global progress and cancels from read-only history while preventing overlapping starts", async () => {
    dataSource.set("history:kept")
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, trading: { ...status.trading!, write: "disabled", reason: "REPLAY_READ_ONLY" } }, null, "open"))
    vi.mocked(api.replay).mockResolvedValue({ directory: "recordings", recordings: [], replay: null, demos: [], write: "open" })
    vi.mocked(api.backtests).mockResolvedValue({ active: state.id, runs: [state], label })
    await render()
    await waitForRender(() => expect(host.textContent).toContain("1 / 4 day runs"))
    expect(button("Start backtest").disabled).toBe(true)
    await click("Cancel backtest"); expect(api.cancelBacktest).toHaveBeenCalledWith("000001", "open")
  })
  it("sorts chosen recordings chronologically and submits a mixed manifest", async () => {
    await render(); await field("Days", "recordings")
    await waitForRender(() => expect(host.querySelectorAll('input[type="checkbox"]')).toHaveLength(2))
    for (const checkbox of host.querySelectorAll<HTMLInputElement>('input[type="checkbox"]')) await act(async () => checkbox.click())
    await click("Start backtest")
    expect(api.startBacktest).toHaveBeenLastCalledWith(expect.objectContaining({ days: [{ file: "first.oprec" }, { file: "second.oprec" }] }), "open")
    await field("Days", "manifest"); await field("Day manifest JSON", '[{"scenario":"fade","seed":18446744073709551610}]'); await click("Start backtest")
    expect(host.textContent).toContain("quoted decimal strings")
    expect(api.startBacktest).toHaveBeenCalledTimes(1)
    await field("Day manifest JSON", '[{"scenario":"fade","date":"2026-09-14","seed":"18446744073709551610"},{"file":"second.oprec"}]'); await click("Start backtest")
    expect(api.startBacktest).toHaveBeenLastCalledWith(expect.objectContaining({ days: [{ scenario: "fade", date: "2026-09-14", seed: "18446744073709551610" }, { file: "second.oprec" }] }), "open")
  })
  it("validates count and seed and displays server errors", async () => {
    await render(); await field("Day count", "253"); expect(button("Start backtest").disabled).toBe(true)
    await field("Day count", "1"); await field("Starting seed", "18446744073709551616"); expect(button("Start backtest").disabled).toBe(true)
    await field("Starting seed", "0"); vi.mocked(api.startBacktest).mockRejectedValue(new Error("Days must have distinct, increasing trading dates"))
    await click("Start backtest"); expect(host.textContent).toContain("Days must have distinct, increasing trading dates")
  })
  it("blocks writes on a read-only server", async () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, trading: { ...status.trading!, write: "disabled" } }, null, "open"))
    await render(); expect(button("Start backtest").disabled).toBe(true)
  })
  it("renders charts, exact seed labels, missing statistics and journal paths", async () => {
    await render(<BacktestReportView report={report} />)
    expect(host.querySelectorAll("svg").length).toBeGreaterThanOrEqual(2)
    expect(host.textContent).toContain("18446744073709551610")
    expect(host.textContent).toContain("days/000001.jsonl")
    expect(host.textContent).toContain("Observed 2026-09-14T13:30:00Z to 2026-09-14T20:15:00Z")
    expect(host.textContent).toContain("0 passed · 0 failed · 1 open")
    expect(host.textContent).toContain("Trade expectancy—")
    expect(host.textContent).toContain("IV rank unavailable (no IV history)")
  })
  it("labels partial results and incomplete marks", async () => {
    await render(<BacktestReportView report={{ ...report, status: "cancelled", days: [{ ...report.days[0]!, pnl: null, valuation_complete: false }], errors: [{ day: 0, message: "Truncated recording" }] }} />)
    expect(host.textContent).toContain("Partial report")
    expect(host.textContent).toContain("Incomplete marks")
    expect(host.textContent).toContain("Truncated recording")
  })
  it("expands attempt day rows while accepting older reports without them", async () => {
    await render(<BacktestReportView report={{ ...report, schema: 2, attempts: [{ ...report.attempts[0]!, day_rows: [{
      date: "2026-09-14", day_index: 0, ended: result.ended, start_balance: "50000.00", end_balance: "50040.00", start_equity: "50000.00", end_equity: "50040.00",
      pnl: "40.00", valuation_complete: true, floor: "48000.00", floor_distance: "2040.00", target: "3000.00", target_progress: "40.00", peak: "50040.00",
      day_lock: "DAILY_LOSS_LIMIT", outcome: "open", decision: "", decision_code: "NONE", rule_trips: [], trades_opened: 2, trades_closed: 1,
    }] }] }} />)
    const summary = [...host.querySelectorAll("summary")].find((item) => item.textContent === "Daily evaluation (1 days)")!
    await act(async () => summary.click())
    expect(summary.parentElement?.getAttribute("open")).not.toBeNull()
    expect(summary.parentElement?.textContent).toContain("$50,000.00 → $50,040.00")
    expect(summary.parentElement?.textContent).toContain("DAILY_LOSS_LIMIT")
    expect(summary.parentElement?.textContent).toContain("2 / 1")
  })
  it("compares selected saved runs and charts their independent combined curve", async () => {
    vi.mocked(api.backtests).mockResolvedValue({ active: null, label, runs: ["000001", "000002"].map((id) => ({ ...state, id, status: "completed", summary: report.summary, bytes: 1048576 })) })
    await render()
    for (const id of ["000001", "000002"]) await act(async () => host.querySelector<HTMLInputElement>(`[aria-label="Compare run ${id}"]`)!.click())
    await waitForRender(() => expect(host.textContent).toContain("Compare saved runs"))
    expect(api.compareBacktests).toHaveBeenCalledWith(["000001", "000002"], expect.any(AbortSignal))
    expect(host.textContent).toContain("different inputs")
    expect(host.textContent).toContain("No shared buying power")
    expect(host.textContent).toContain("1.00 MiB")
    expect(host.querySelector('path[aria-label="Combined cumulative P&L"]')).not.toBeNull()
    expect(host.querySelectorAll("table")[1]?.textContent).toContain("—")
  })
  it("drops a compared or selected run the server has since deleted", async () => {
    const runs = ["000001", "000002", "000003"].map((id) => ({ ...state, id, status: "completed" as const, summary: report.summary }))
    vi.mocked(api.backtests).mockResolvedValue({ active: null, label, runs })
    vi.mocked(api.backtest).mockImplementation(async (id) => ({ ...state, id, status: "completed" }))
    await render()
    for (const id of ["000001", "000002", "000003"]) await act(async () => host.querySelector<HTMLInputElement>(`[aria-label="Compare run ${id}"]`)!.click())
    await field("Saved report", "000002")
    await waitForRender(() => expect(host.textContent).toContain("Run 000002 · completed"))
    await waitForRender(() => expect(api.compareBacktests).toHaveBeenCalledWith(["000001", "000002", "000003"], expect.any(AbortSignal)))
    // Retention removes 000002 when another backtest starts.
    vi.mocked(api.backtests).mockResolvedValue({ active: null, label, runs: [runs[0]!, runs[2]!] })
    vi.mocked(api.compareBacktests).mockClear(); vi.mocked(api.backtest).mockClear()
    await waitForRender(() => expect(host.querySelector('[aria-label="Compare run 000002"]')).toBeNull())
    await waitForRender(() => expect(api.compareBacktests).toHaveBeenCalledWith(["000001", "000003"], expect.any(AbortSignal)))
    expect(api.compareBacktests).not.toHaveBeenCalledWith(expect.arrayContaining(["000002"]), expect.anything())
    await waitForRender(() => expect(host.textContent).toContain("Run 000001 · completed"))
    expect(api.backtest).not.toHaveBeenCalledWith("000002", expect.anything())
  })
  it("compares joint and legacy reports and labels already-joint inputs", async () => {
    await render(<BacktestCompareView comparison={{ ...comparison,
      runs: [{ ...comparison.runs[0]!, playbook: undefined, mode: "joint", playbooks: [{ id: "alpha", name: "Alpha", version: 2 }, { id: "beta", name: "Beta", version: 1 }] }, comparison.runs[1]!],
      combined: { ...comparison.combined, includes_joint: true, label: "Sum of independent run days; includes already-joint account reports." },
    }} />)
    expect(host.textContent).toContain("Alpha@2, Beta@1")
    expect(host.textContent).toContain("test@1")
    expect(host.textContent).toContain("already-joint account reports")
  })
  it("pins saved runs and confirms permanent deletion", async () => {
    const finished: BacktestState = { ...state, status: "completed", summary: report.summary, keep: false }
    vi.mocked(api.backtests).mockResolvedValue({ active: null, label, runs: [finished] })
    vi.mocked(api.backtest).mockResolvedValue(finished)
    await render()
    await act(async () => host.querySelector<HTMLInputElement>('[aria-label="Keep run 000001"]')!.click())
    await waitForRender(() => expect(api.keepBacktest).toHaveBeenCalledWith("000001", true, "open"))
    await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Delete run 000001"]')!.click())
    expect(api.deleteBacktest).not.toHaveBeenCalled()
    expect(document.body.textContent).toContain("Permanently delete run 000001")
    vi.mocked(api.backtests).mockResolvedValue({ active: null, label, runs: [] })
    const confirm = [...document.body.querySelectorAll("button")].find((item) => item.textContent === "Delete saved run")!
    await act(async () => confirm.click())
    await waitForRender(() => expect(api.deleteBacktest).toHaveBeenCalledWith("000001", "open"))
    expect(document.body.textContent).not.toContain("Delete saved backtest?")
  })
  it("disables deletion and comparison for active runs", async () => {
    vi.mocked(api.backtests).mockResolvedValue({ active: state.id, label, runs: [state] })
    await render()
    expect(host.querySelector<HTMLButtonElement>('[aria-label="Delete run 000001"]')!.disabled).toBe(true)
    expect(host.querySelector<HTMLInputElement>('[aria-label="Compare run 000001"]')!.disabled).toBe(true)
  })
  it("bins every observation, including equal and negative values", () => {
    expect(backtestHistogram([])).toEqual([])
    expect(backtestHistogram(["0", "0"])).toEqual([{ x: 0, value: 2 }])
    expect(backtestHistogram(["-20", "0", "20"]).reduce((sum, item) => sum + item.value, 0)).toBe(3)
  })
})
