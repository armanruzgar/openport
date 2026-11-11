// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { EquityHistory } from "../api/trading-types"
import { account, status, trading } from "../test/trading-fixtures"
import { IntradayEquity } from "./IntradayEquity"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../charts/useSize", () => ({ useSize: () => [{ current: null }, { width: 640, height: 280 }] }))

const sample = (time: string, equity: string) => ({ time, day: account.evaluation.day, attempt: account.evaluation.attempt, equity,
  floor: "95300.00", peak: "100300.00", target: "110000.00", tomorrow_floor: null, fill: null })
const history: EquityHistory = { samples: [sample("2026-09-23T14:00:00Z", "100100.00"), sample("2026-09-23T14:01:00Z", "100150.00")], error: null }
let host: HTMLDivElement, root: Root, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity } } })
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})
async function renderAt(version: string) {
  vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"), trading: { ...trading, account_version: version } })
  await act(async () => root.render(<QueryClientProvider client={client}><IntradayEquity account={account} /></QueryClientProvider>))
}

it("keeps the equity chart up while the next account version's history loads", async () => {
  const equity = vi.spyOn(api, "equity").mockReturnValue(new Promise<EquityHistory>(() => {}))
  client.setQueryData(["trading", 0, "equity", "7"], history)
  await renderAt("7")
  const chart = () => host.querySelector('svg[aria-label^="Equity"]')
  expect(chart()).not.toBeNull()
  await renderAt("8")
  expect(equity).toHaveBeenCalled()
  expect(host.textContent).not.toContain("Loading equity history")
  expect(chart()).not.toBeNull()
  expect(host.querySelector<HTMLSelectElement>('select[aria-label="Equity day"]')!.value).toBe(account.evaluation.day)
})
