// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { RiskProfile, RiskProfileQuery } from "../api/trading-types"
import { waitForRender } from "../test/render"
import { status } from "../test/trading-fixtures"
import { RiskProfilePanel } from "./RiskProfilePanel"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../charts/useSize", () => ({ useSize: () => [{ current: null }, { width: 640, height: 300 }] }))

const profile = (query: RiskProfileQuery): RiskProfile => ({
  account_version: "17", time: "2026-09-22T14:00:00Z", underlying: null, benchmark: "SPY", reference: "SPY", spot: 600,
  vol_points: 0, equity: "10000.00", room: "1000.00", soft_room: null, percent: [-5, 0, 5], prices: [570, 600, 630],
  curves: [{ days: 0, label: null, time: "2026-09-22T14:00:00Z", pnl: [-1200, 0, 900], clamped: false,
    horizon: { until: "2026-09-22T20:00:00Z", sigma: 0.01, one_sd: null, two_sd: null }, down: null, up: null, soft_down: null, soft_up: null }],
  betas: Object.entries(query.betas ?? {}).map(([underlying, beta]) => ({ underlying, beta, source: "given" as const, observations: 0,
    correlation: null, dollar_delta: 1000, weighted_dollar_delta: 1000 * beta })),
  weighted_dollar_delta: null, weighted_delta: null, complete: true, model: "Black-76",
})
let host: HTMLDivElement, root: Root, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity } } })
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})
const field = () => host.querySelector<HTMLInputElement>('input[aria-label="Beta overrides"]')!
async function type(value: string) {
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(field(), value)
    field().dispatchEvent(new Event("input", { bubbles: true }))
  })
}

it("sends only beta overrides that parse and says why one is not used", async () => {
  const request = vi.spyOn(api, "riskProfile").mockImplementation(async (query) => profile(query))
  await act(async () => root.render(<QueryClientProvider client={client}><RiskProfilePanel underlyings={["SPX", "QQQ"]} /></QueryClientProvider>))
  await waitForRender(() => expect(request).toHaveBeenCalledTimes(1))
  expect(request.mock.calls[0]![0].betas).toEqual({})
  await type("QQQ:1.2")
  await waitForRender(() => expect(request).toHaveBeenCalledTimes(2))
  expect(request.mock.calls[1]![0].betas).toEqual({ QQQ: 1.2 })
  await waitForRender(() => expect(host.textContent).toContain("Black-76"))
  // Out of range: not sent, not replaced by no overrides, and explained.
  await type("QQQ:12")
  expect(field().getAttribute("aria-invalid")).toBe("true")
  const hint = document.getElementById(field().getAttribute("aria-describedby")!)!
  expect(hint.textContent).toContain("Use SYMBOL:BETA pairs separated by commas, with betas from -10 to 10.")
  expect(request).toHaveBeenCalledTimes(2)
  expect(host.textContent).not.toContain("Loading profile")
  expect(host.textContent).toContain("Black-76")
})
