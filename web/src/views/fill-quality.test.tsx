import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { Calibration, FillQuality, FillQualityGroup, Order } from "../api/trading-types"
import { FillModelPicker, fillProgramRules } from "../components/FillModelPicker"
import { OrderDetailDialog } from "../components/OrderDetail"
import { dataSource } from "../lib/data-source"
import { resetQuote } from "../lib/program-costs"
import { writeToken } from "../lib/write-token"
import { account, order, plans, status } from "../test/trading-fixtures"
import { CalibrationResult, FillQualityPanel, osiLabel } from "./FillQualityPanel"
import { ruleText } from "./RulesView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const clients: QueryClient[] = []
beforeEach(() => vi.mocked(useLive).mockReturnValue(liveState(status, null, "open")))
afterEach(() => { clients.splice(0).forEach((client) => client.clear()); vi.clearAllMocks(); vi.unstubAllGlobals(); dataSource.set("live"); writeToken.set("") })

const group = (key: string, label: string, contracts: number, natural = contracts): FillQualityGroup => ({
  key, label, fills: contracts ? 1 : 0, contracts,
  places: { mid_or_better: contracts - natural, mid_plus_one_tick: 0, inside: 0, natural, beyond_natural: 0 },
  from_mid_ticks: contracts ? 1 : null, vs_natural_ticks: contracts ? 0 : null, cost_vs_mid: contracts ? "20.00" : "0.00",
})
const suggestion = { basis: "heuristic" as const, fill_model: "conservative" as const, fill_latency_ms: 1000, slippage_ticks: 1, impact_ticks: 1,
  inside_fill_percent: 0, reasons: ["Conservative's 1,000 ms latency, 1 slippage tick and 1 impact tick are the starting point."] }
const quality: FillQuality = {
  attempt: "current", fills: 3, measured: 2, unmeasured: 1, overall: group("all", "All fills", 2),
  time_of_day: [group("open", "09:30-10:00", 0), group("morning", "10:00-12:00", 2)],
  spread: [group("2", "2 ticks", 2)], dte: [group("8-30", "8-30 DTE", 2)], suggestion, simulated: true,
}
function render(node: ReactNode) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(client)
  client.setQueryData(tradingQueries(0, "17", true).fillQuality("current").queryKey, quality)
  return renderToStaticMarkup(<QueryClientProvider client={client}>{node}</QueryClientProvider>)
}

describe("fill quality", () => {
  it("places the journal's fills on their books by time of day with a heuristic suggestion and the real-fill import", () => {
    const html = render(<FillQualityPanel scope="current" />)
    for (const text of ["Fill quality", "2 simulated fills placed on the book", "1 recorded without a two-sided book are left out",
      "All fills", "10:00-12:00", "100%", "+$20.00", "Suggested Conservative calibration: Conservative", "1,000 ms latency · 1 slippage tick · 1 impact tick",
      "Calibrate from real fills", "Broker fills CSV", "Analysis only: nothing is stored or traded"])
      expect(html).toContain(text)
    expect(html).not.toContain("2 ticks</td>")
  })
  it("shows each real fill against the same-time book and what each paper model would do", () => {
    const result: Calibration = {
      format: "tastytrade", matched: 1, unmatched: 1, real: group("real", "Real fills", 1, 0), paper_fills: { as_displayed: 0, conservative: 0, midpoint: 1 },
      suggestion: { ...suggestion, basis: "real_fills", fill_model: "midpoint", fill_latency_ms: null, slippage_ticks: 0, impact_ticks: 0, inside_fill_percent: 50,
        reasons: ["Broker exports carry no order-entry time, so fill latency is not measured; keep your current setting."] },
      skipped: [{ row: 4, reason: "Not a trade" }], stored: false,
      rows: [
        { row: 2, time: "2026-10-21T14:15:32Z", symbol: "SPXW  261022C05000000", side: "buy", quantity: 1, price: "4.10",
          quote: { bid: "4.00", ask: "4.20", bid_size: 5, ask_size: 5, time: "2026-10-21T14:15:30Z", source: "recording" }, place: "mid_or_better",
          from_mid_ticks: 0, vs_natural_ticks: -1, as_displayed: { fills: false, price: null }, conservative: { fills: false, price: null },
          midpoint: { fills: true, price: "4.10" }, note: null },
        { row: 3, time: null, symbol: "SPXW  261022P04900000", side: "sell", quantity: 2, price: "3.00", quote: null, place: null,
          from_mid_ticks: null, vs_natural_ticks: null, as_displayed: { fills: false, price: null }, conservative: { fills: false, price: null },
          midpoint: { fills: false, price: null }, note: "The export gives no execution time, so no quote can be matched" },
      ],
    }
    const html = renderToStaticMarkup(<CalibrationResult result={result} />)
    for (const text of ["Read tastytrade: 1 of 2 fills matched", "1 row skipped", "as displayed would have filled 0, Conservative 0",
      "inside at midpoint 1", "Nothing was stored, and the account is unchanged", "Suggested from your real fills: Inside at midpoint",
      "latency not measured", "inside fills at 50%", "SPXW Oct 22 5000C", "$4.00 / $4.20 · recording", "Mid or better", "-1.00", "rests", "date only",
      "no execution time"])
      expect(html).toContain(text)
    expect(osiLabel("SPXW  261022P04900500")).toBe("SPXW Oct 22 4900.5P")
    expect(osiLabel("SPY")).toBe("SPY")
  })
  it("posts a broker export for analysis to the live route with read credentials, even on a replay", async () => {
    const fetcher = vi.fn(async () => new Response(JSON.stringify({ stored: false }), { status: 200 }))
    vi.stubGlobal("fetch", fetcher)
    dataSource.set("replay")
    writeToken.set("reader")
    await api.calibrateFills("time,symbol\n", "ibkr")
    const [url, init] = fetcher.mock.calls[0] as unknown as [string, RequestInit]
    expect(url).toBe("/api/fills/calibration")
    expect(init.method).toBe("POST")
    expect(JSON.parse(String(init.body))).toEqual({ csv: "time,symbol\n", format: "ibkr" })
    expect(new Headers(init.headers).get("Authorization")).toBe("Bearer reader")
    dataSource.set("live")
    await api.fillQuality("all")
    expect((fetcher.mock.calls[1] as unknown as [string])[0]).toBe("/api/fills/quality?attempt=all")
  })
})

describe("queue fills and stop stress", () => {
  it("offers the queue model and stop stress, which are program rules", () => {
    const html = renderToStaticMarkup(<FillModelPicker value="queue" onChange={() => {}} stress={2} onStress={() => {}} />)
    for (const text of ["Queue position (trade prints)", "waits behind the size shown there", "not your real place", "Stop stress",
      "2 extra ticks in fast markets", "program rules"])
      expect(html).toContain(text)
    expect(renderToStaticMarkup(<FillModelPicker value="as_displayed" onChange={() => {}} />)).not.toContain("Stop stress")
    expect(fillProgramRules("queue", 0)).toEqual({ queue_fills: true })
    expect(fillProgramRules("conservative", 0)).toEqual({})
    const plan = plans.find((p) => p.id === "intraday-100k")!
    expect(resetQuote(plan, account, plan.id).kind).toBe("reset")
    expect(resetQuote({ ...plan, rules: { ...plan.rules, ...fillProgramRules("queue", 0) } }, account, plan.id).kind).toBe("evaluation")
    expect(resetQuote({ ...plan, rules: { ...plan.rules, ...fillProgramRules("as_displayed", 2) } }, account, plan.id).kind).toBe("evaluation")
  })
  it("explains a queue estimate, a stressed stop and the rules", () => {
    const resting: Order = { ...order, status: "working", stress_ticks: 2,
      queue: { price: "4.00", ahead: 7, earned: 0, displayed: 10, joined_at: order.accepted_at, updated_at: order.accepted_at, source: "prints", simulated: true } }
    const html = renderToStaticMarkup(<OrderDetailDialog order={resting} onClose={() => {}} />)
    expect(html).toContain("Simulated queue at $4.00: about 7 contracts ahead")
    expect(html).toContain("from trade prints")
    expect(html).toContain("slip 2 extra ticks (stop stress)")
    const rules = { ...account, rules: { ...account.rules, queue_fills: true, queue_seed: 3, stop_stress_ticks: 1, stop_stress_percent: 300 } }
    const fills = renderToStaticMarkup(<>{ruleText(rules, "0.65").find((r) => r.title === "Fills")!.body}</>)
    expect(fills).toContain("Queue fills are on (seed 3)")
    expect(fills).toContain("1 adverse tick to a stop reached while the spread is at least 300%")
  })
})
