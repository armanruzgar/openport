// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api, ApiError } from "../api/client"
import type { AnalysisPortfolio, AnalysisPortfolios } from "../api/types"
import { waitForRender } from "../test/render"
import { AnalysisView } from "./AnalysisView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(),
  useLive: () => ({ version: () => 1, status: { trading: { write: "open" } } }) }))

const portfolio: AnalysisPortfolio = { portfolio: "ira", id: "ira", name: "IRA", analysis_only: true, read_only: true,
  note: "Imported for analysis only.", as_of: null, unpriced: [], complete: true, format: "schwab", imported: "2026-10-21T14:00:00.000Z",
  skipped: [], value: 8400, cost: 1000, unrealised: 400, views: ["grid", "profile"],
  exposure: { aggregate: { dollar_delta: 1000, dollar_gamma_1pct: 10, vega: 50, theta: -5 }, underlyings: [], complete: true },
  holdings: [{ symbol: "SPXW  261120C05800000", quantity: 2, source: "line 4", cost: 40, underlying: "SPX", type: "call", expiry: "2026-11-20",
    strike: 5800, multiplier: 100, mark: 42, value: 8400, unrealised: 400, delta: 110, gamma: 1, vega: 1500, theta: -60, status: "priced" }] }
const list: AnalysisPortfolios = { portfolios: [{ id: "ira", name: "IRA", format: "schwab", imported: portfolio.imported, holdings: 1, analysis_only: true, read_only: true }],
  formats: ["generic", "tastytrade", "schwab", "ibkr"], note: "", error: null }

let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.stubGlobal("fetch", vi.fn(() => new Promise(() => {})))
  vi.spyOn(window, "confirm").mockReturnValue(true)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  client.setQueryData(["analysis-portfolios"], list)
  client.setQueryData(["analysis", "ira"], portfolio)
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); client.clear(); vi.restoreAllMocks(); vi.unstubAllGlobals() })
const deleteButton = () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Delete" || b.textContent === "Deleting…")!

it("shows a refused delete, and stays disabled while one is in flight", async () => {
  let refuse!: (error: unknown) => void
  const remove = vi.spyOn(api, "deletePortfolio").mockImplementation(() => new Promise((_, reject) => { refuse = reject }))
  await act(async () => root.render(<QueryClientProvider client={client}><AnalysisView /></QueryClientProvider>))
  await act(async () => deleteButton().click())
  expect(deleteButton().textContent).toBe("Deleting…")
  expect(deleteButton().disabled).toBe(true)
  await act(async () => deleteButton().click())
  expect(remove).toHaveBeenCalledOnce()
  await act(async () => refuse(new ApiError(403, "A valid bearer token is required", "WRITE_TOKEN_REQUIRED")))
  expect(host.querySelector('[role="alert"]')?.textContent).toBe("Could not delete IRA: A valid bearer token is required")
  expect(deleteButton().disabled).toBe(false)
  expect(host.textContent).toContain("Holdings · IRA")
})

it("takes a portfolio already gone as deleted", async () => {
  vi.spyOn(api, "deletePortfolio").mockRejectedValue(new ApiError(404, "no analysis portfolio ira"))
  const listing = vi.spyOn(api, "analysisPortfolios").mockResolvedValue({ ...list, portfolios: [] })
  await act(async () => root.render(<QueryClientProvider client={client}><AnalysisView /></QueryClientProvider>))
  await act(async () => deleteButton().click())
  await waitForRender(() => expect(host.textContent).toContain("Nothing imported yet."))
  expect(listing).toHaveBeenCalled()
  expect(host.querySelector('[role="alert"]')).toBeNull()
})
