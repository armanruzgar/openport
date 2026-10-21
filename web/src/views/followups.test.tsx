import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import type { Chain, Expiry, ExposureMatrix, OptionQuote, ReplayState, Status, Summary, Tick } from "../api/types"
import { Header } from "../components/Header"
import { ChainView } from "./ChainView"
import { EngineView } from "./EngineView"
import { ExposureView } from "./ExposureView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../lib/theme", () => ({ useTheme: () => ({ theme: "dark", toggleTheme: vi.fn() }) }))

const asOf = "2026-09-22T19:59:00Z"
const status: Status = {
  provider: { name: "test", realtime: true, realtime_plan_dependent: true, delay_seconds: 900, trades: false, open_interest: true, vendor_greeks: false },
  feed: { state: "delayed", message: "Connected", updated: asOf },
  engine: { contracts: 2, events: 1, events_per_second: null, analytics_ms: null, uptime_seconds: 60 },
  underlyings: [{ symbol: "SPY", spot: 700, as_of: asOf, version: 1, expiries: 1, options: 2 }],
  market: { open: false, note: "Session ended", next_open: "2026-09-23T13:30:00Z" },
}
const expiry: Expiry = {
  id: "2026-10-16PM", expiry: "2026-10-16", expiry_time: "2026-10-16T20:00:00Z", settlement: "PM",
  days: 24, forward: 700, discount: 1, rate: 0, rate_fitted: false, atm_iv: null, gex: null, vex: null, strikes: 1,
  coverage: { options: 2, quoted: 1, priced: 1, open_interest: 0 }, style: "european",
}
const summary: Summary = {
  symbol: "SPY", spot: 700, spot_source: "parity", as_of: asOf, version: 1, compute_ms: null,
  exposure: { gex: null, vex: null, gamma_flip: null, call_wall: null, put_wall: null, oi_coverage: 0.97 },
  expiries: [expiry], american_approximation: true,
}
const quote: OptionQuote = {
  bid: null, ask: null, mid: null, oi: null, iv: null, bid_iv: null, ask_iv: null,
  delta: null, gamma: null, vega: null, theta: null, vanna: null, vendor_iv: null,
}
const clients: QueryClient[] = []

function render(view: ReactNode, payloads: { summary?: Summary; chain?: Chain; exposure?: ExposureMatrix } = {}) {
  const client = new QueryClient({ defaultOptions: { queries: { staleTime: Infinity, gcTime: Infinity, retry: false } } })
  clients.push(client)
  client.setQueryData(["summary", "SPY", 1], payloads.summary ?? summary)
  if (payloads.chain) client.setQueryData(["chain", "SPY", expiry.id, 0.05, 1], payloads.chain)
  if (payloads.exposure) client.setQueryData(["exposure", "SPY", 8, 0.05, 1], payloads.exposure)
  return renderToStaticMarkup(<QueryClientProvider client={client}>{view}</QueryClientProvider>)
}

afterEach(() => {
  clients.splice(0).forEach((client) => client.clear())
  vi.clearAllMocks()
})

describe("web follow-up rendering", () => {
  it("shows the series directory, market write time, row count and storage errors", () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, series: { enabled: true, directory: "/tmp/series", rows_today: 391, last_write: asOf, last_error: "disk full" } }, null, "open"))
    const html = render(<EngineView />)
    for (const text of ["Volatility history store", "Enabled", "/tmp/series", "391", "Last write · market time", "disk full"]) expect(html).toContain(text)
    vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
    expect(render(<EngineView />)).toContain("Disabled")
  })

  it("labels a matching parity snapshot, the closed session, and the actual delayed plan", () => {
    vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
    const html = render(<Header symbol="SPY" onSymbol={() => {}} />)
    expect(html).toContain("≈700.00")
    expect(html).toContain("No underlying quote from this provider; spot inferred from put-call parity")
    expect(html).toContain("market closed")
    expect(html).toContain("Session ended · Next open: Wed, Sep 23, 2026, 09:30 ET")
    expect(html).toContain("15-min delay")
    expect(html).not.toContain(">stale<")
  })

  it("does not attach another snapshot's source to the header spot and handles pending underlyings", () => {
    vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
    const header = <Header symbol="SPY" onSymbol={() => {}} />
    expect(render(header, { summary: { ...summary, version: 0 } })).not.toContain("≈")
    expect(render(header, { summary: { ...summary, symbol: "SPX" } })).not.toContain("≈")
    const pending = { ...status, underlyings: [{ ...status.underlyings[0]!, spot: null, as_of: null }] }
    vi.mocked(useLive).mockReturnValue(liveState(pending, null, "open"))
    const html = render(header)
    expect(html).not.toContain("≈")
    expect(html).toContain(">—</span>")
    expect(html).toContain("date / age unavailable")
  })

  it("shows null quotes as em dashes, received zeros as zero, selected-expiry coverage and style", () => {
    vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
    const chain: Chain = {
      symbol: "SPY", spot: null, as_of: null, version: 1, expiry,
      strikes: [{ strike: 700, iv: null, gex: null, vex: null, call: quote, put: { ...quote, bid: 0, ask: 0, mid: 0, oi: 0 } }],
    }
    const html = render(<ChainView symbol="SPY" expiry={expiry.id} onExpiry={() => {}} />, { chain })
    const cells = [...html.matchAll(/<td\b[^>]*>(.*?)<\/td>/g)].map((match) => match[1]!.replace(/<[^>]+>/g, ""))
    expect(cells.slice(0, 8)).toEqual(["—", "—", "none", "—", "—", "—", "—", "—"])
    expect(cells.slice(-8)).toEqual(["0.00", "0.00", "—", "—", "0", "none", "—", "—"])
    expect(html).toContain("priced 1/2 · OI 0/2 · low coverage")
    expect(html).not.toContain("IVs and Greeks use a European model")
  })

  it("shows curve provenance and a neutral de-Americanisation note without flagging zero bids", () => {
    vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
    const chain: Chain = {
      symbol: "SPY", spot: 700, as_of: asOf, version: 1,
      expiry: { ...expiry, style: "american", rate_source: "curve", rate_curve_symbol: "SPX", deamericanized: true,
        coverage: { options: 366, quoted: 366, priced: 247, open_interest: 366 } },
      strikes: [],
    }
    const html = render(<ChainView symbol="SPY" expiry={expiry.id} onExpiry={() => {}} />, { chain })
    expect(html).toContain("From the SPX parity curve: American-style parity is distorted by early exercise")
    expect(html).toContain("0.00%*")
    expect(html).toContain("IVs de-Americanised: early-exercise premium removed with a Leisen-Reimer tree")
    expect(html).toContain("priced 247/366")
    expect(html).not.toContain("low coverage")
    expect(html).not.toContain("IVs and Greeks use a European model")
  })

  it("keeps market quotes and explains the positive EEP in the IV tooltip", () => {
    vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
    const chain: Chain = { symbol: "SPY", spot: 700, as_of: asOf, version: 1, expiry,
      strikes: [{ strike: 700, iv: .2, gex: 0, vex: 0,
        call: { ...quote, bid: 1.1, ask: 1.3, mid: 1.2, iv: .2, eep: .12, vendor_iv: .201 },
        put: { ...quote, eep: null } }] }
    const html = render(<ChainView symbol="SPY" expiry={expiry.id} onExpiry={() => {}} />, { chain })
    expect(html).toContain("IV after removing a $0.12 early-exercise premium")
    expect(html).toContain("test 20.10")
    expect(html).toContain("1.10")
    expect(html).toContain("1.30")
    expect(html).not.toContain("$0.00 early-exercise premium")
  })

  it("uses the selected underlying session instead of the top-level regular market", () => {
    const clock = vi.spyOn(Date, "now").mockReturnValue(Date.parse(asOf) + 15 * 60_000)
    try {
      const header = <Header symbol="SPY" onSymbol={() => {}} />
      for (const name of ["global", "curb", "closed"] as const) {
        const selected = { ...status, underlyings: [{ ...status.underlyings[0]!,
          session: { name, open: name !== "closed", note: "Product session" } }] }
        vi.mocked(useLive).mockReturnValue(liveState(selected, null, "open"))
        const label = name === "global" ? "overnight session" : name === "curb" ? "curb session" : "market closed"
        expect(render(<EngineView />)).toContain(label)
        const html = render(header)
        expect(html).toContain(label)
        expect(html).not.toContain(">stale<")
        if (name !== "closed") expect(html).not.toContain("market closed")
      }
      const tick: Tick = { type: "tick", feed: { state: "delayed", message: "Receiving" }, engine: status.engine,
        underlyings: [{ ...status.underlyings[0]!, session: { name: "global", open: true, note: "Overnight" } }] }
      clock.mockReturnValue(Date.parse(asOf) + 25 * 60_000 + 1)
      vi.mocked(useLive).mockReturnValue(liveState(status, tick, "open"))
      expect(render(header)).toContain(">stale<")
      tick.underlyings[0]!.session = { name: "closed", open: false, note: "Closed" }
      vi.mocked(useLive).mockReturnValue(liveState(status, tick, "open"))
      const closedHtml = render(header)
      expect(closedHtml).toContain("market closed")
      expect(closedHtml).not.toContain(">stale<")
    } finally {
      clock.mockRestore()
    }
  })

  it.each([0.97, 0.89, null])("renders exposure OI coverage %s, including unknown coverage", (ratio) => {
    vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
    const exposure: ExposureMatrix = {
      symbol: "SPY", spot: null, as_of: null, version: 1, strikes: [], expiries: [], total_gex: [],
      exposure: { ...summary.exposure, oi_coverage: ratio },
    }
    const html = render(<ExposureView symbol="SPY" />, { exposure })
    expect(html).toContain(ratio == null ? "OI coverage —" : `OI coverage ${Math.round(ratio * 100)}%`)
    expect(html.includes("low coverage")).toBe(ratio != null && ratio < 0.9)
  })

  it("shows tick health, error time and queue overload without losing nulls to REST fallbacks", () => {
    const tick: Tick = {
      type: "tick", feed: { ...status.feed, message: "cboe QQQ: 10826 options, parsed in 502 ms; cboe SPX: 9000 options, parsed in 450 ms" },
      engine: { ...status.engine, queue_depth: 3, coalesced_events: 4, dropped_events: 5, overloaded: true },
      underlyings: [{ symbol: "SPY", spot: null, as_of: null, version: 1, state: "error", message: "Retrying", last_success: asOf, last_error: "Rate limited", last_error_time: "2026-09-22T20:00:00Z" }],
    }
    vi.mocked(useLive).mockReturnValue(liveState(status, tick, "open"))
    const html = render(<EngineView />)
    expect(html).toContain("Underlying health")
    expect(html).toContain("Retrying")
    expect(html).toContain("Rate limited")
    expect(html).toContain("Tue, Sep 22, 2026, 16:00 ET")
    expect(html).toContain("old</time>")
    expect(html).toContain("overloaded")
    expect(html).toContain('<span class="block">cboe QQQ: 10826 options, parsed in 502 ms</span>')
    expect(html).toContain('<span class="block">cboe SPX: 9000 options, parsed in 450 ms</span>')
    for (const [label, value] of [["Queue depth", 3], ["Coalesced events", 4], ["Dropped events", 5]]) {
      expect(html.replace(/<[^>]+>/g, "")).toContain(`${label}${value}`)
    }
    expect(html).not.toContain("700.00")
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, market: undefined }, null, "open"))
    expect(render(<EngineView />)).not.toContain("overloaded")
  })

  it("ages a replay's underlying health by the replay clock", () => {
    const tick: Tick = { type: "tick", feed: status.feed, engine: status.engine,
      underlyings: [{ symbol: "SPY", spot: 700, as_of: asOf, version: 1, state: "live", message: null, last_success: asOf, last_error: null, last_error_time: null }] }
    vi.mocked(useLive).mockReturnValue(liveState(status, tick, "open", 0, undefined, undefined, "replay", { time: "2026-09-22T19:59:30Z" } as ReplayState))
    const html = render(<EngineView />)
    const health = html.slice(html.indexOf("Underlying health"))
    expect(health).toContain("15:59 ET, &lt;1 min old")
    expect(health).not.toMatch(/\dd \d+h old/)
  })
})
