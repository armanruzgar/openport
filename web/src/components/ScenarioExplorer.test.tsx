// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { HistoricalRisk, ScenarioAnalysis } from "../api/trading-types"
import { waitForRender } from "../test/render"
import { status, trading } from "../test/trading-fixtures"
import { ScenarioExplorer } from "./ScenarioExplorer"
import { HistoricalRiskPanel } from "./StressPanel"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))

let root: Root
let host: HTMLDivElement
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  host = document.createElement("div")
  document.body.append(host)
  root = createRoot(host)
})
afterEach(async () => {
  await act(async () => root.unmount())
  host.remove()
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
})

const analysis = (dynamics: ScenarioAnalysis["dynamics"], fallback: string[] = []): ScenarioAnalysis => ({
  account_version: "1", time: "2026-10-21T15:00:00Z", mode: "parallel", benchmark: "SPY", betas: [], term_scaled: false,
  slices: [{ days: 0, spot_percent: [-5, 0, 5], vol_points: [0], pnl: [[-10], [0], [12]], clamped: [[false], [false], [false]], complete: true }],
  complete: true, dynamics, surface_fallback: fallback, model: "Derived Black-76",
})

it("asks for smile dynamics only when moved off sticky strike and names a missing surface", async () => {
  const scenarios = vi.spyOn(api, "scenarios").mockImplementation(async (query) =>
    analysis(query.includes("dynamics=sticky_delta") ? "sticky_delta" : "sticky_strike", query.includes("dynamics=") ? ["SPX"] : []))
  await act(async () => root.render(<QueryClientProvider client={new QueryClient()}><ScenarioExplorer /></QueryClientProvider>))
  await waitForRender(() => expect(host.textContent).toContain("Sticky strike: each strike keeps its IV"))
  expect(scenarios.mock.calls[0]![0]).not.toContain("dynamics=")
  const select = host.querySelector<HTMLSelectElement>('select[aria-label="Scenario smile dynamics"]')!
  await act(async () => { select.value = "sticky_delta"; select.dispatchEvent(new Event("change", { bubbles: true })) })
  await waitForRender(() => expect(host.textContent).toContain("No surface for SPX"))
  expect(scenarios.mock.calls.at(-1)![0]).toContain("dynamics=sticky_delta")
  expect(host.textContent).toContain("vol changes apply in delta space")
})

const tail = { complete: true, observations: 40, var95: 120, es95: 150, var99: null, es99: null,
  tail99_reason: "Needs at least 100 observations; 40 available", first: "2026-07-01", last: "2026-08-26" }
const risk: HistoricalRisk = { account_version: "1", time: "2026-10-21T15:00:00Z", model: "derived", sticky_strike: tail, atm_vol_change: tail, stress: [] }
const grid = () => host.querySelector('[aria-label="Spot and volatility scenario grid"]')
async function renderPanels(client: QueryClient, version: string) {
  vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"), trading: { ...trading, account_version: version } })
  await act(async () => root.render(<QueryClientProvider client={client}><ScenarioExplorer /><HistoricalRiskPanel /></QueryClientProvider>))
}

it("says the risk panels are loading before their first result", async () => {
  vi.spyOn(api, "scenarios").mockReturnValue(new Promise<ScenarioAnalysis>(() => {}))
  vi.spyOn(api, "riskHistory").mockReturnValue(new Promise<HistoricalRisk>(() => {}))
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
  await renderPanels(client, "1")
  expect(host.textContent).toContain("Loading scenarios…")
  expect(host.textContent).toContain("Loading historical risk…")
  client.clear()
})

it("keeps the scenario grid and historical risk up while the next account version's results load", async () => {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
  const scenarios = vi.spyOn(api, "scenarios").mockResolvedValueOnce(analysis("sticky_strike")).mockReturnValue(new Promise<ScenarioAnalysis>(() => {}))
  const history = vi.spyOn(api, "riskHistory").mockResolvedValueOnce(risk).mockReturnValue(new Promise<HistoricalRisk>(() => {}))
  await renderPanels(client, "1")
  await waitForRender(() => expect(grid()).not.toBeNull())
  await waitForRender(() => expect(host.textContent).toContain("Historical simulation"))
  // A fill moves the account version: both refetch, and both keep what they showed.
  await renderPanels(client, "2")
  await waitForRender(() => expect(scenarios.mock.calls.length).toBeGreaterThanOrEqual(2))
  await waitForRender(() => expect(history.mock.calls.length).toBeGreaterThanOrEqual(2))
  expect(grid()).not.toBeNull()
  expect(host.textContent).toContain("Historical simulation")
  expect(host.textContent).not.toContain("Loading")
  client.clear()
})
