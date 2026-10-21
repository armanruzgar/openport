import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { Candle, Candles } from "../api/types"
import { CandleChart, stackTags } from "../charts/CandleChart"
import { PriceChart } from "../components/PriceChart"
import { chartLevels } from "../lib/candles"
import { ChartView } from "./ChartView"
import { expiry, portfolio, status, summary } from "../test/trading-fixtures"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))

/** 2026-09-22 09:30 ET, then one bar every five minutes drifting up from 6,990. */
const open = Date.UTC(2026, 8, 22, 13, 30) / 1000
const bars: Candle[] = Array.from({ length: 60 }, (_, i) => ({ t: open + i * 300, o: 6990 + i / 2, h: 6991 + i / 2, l: 6989.5 + i / 2, c: 6990.25 + i / 2 }))
const text = (x: number) => x.toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 })

const clients: QueryClient[] = []
function render(node: ReactNode, candles?: Candles) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(client)
  client.setQueryData(tradingQueries(0, "17", true).portfolio.queryKey, portfolio)
  client.setQueryData(tradingQueries(0, "17", true).orders.queryKey, { account_version: "17", orders: [] })
  if (candles) client.setQueryData(["candles", candles.symbol, candles.interval, 1], candles)
  return renderToStaticMarkup(<QueryClientProvider client={client}>{node}</QueryClientProvider>)
}
beforeEach(() => vi.mocked(useLive).mockReturnValue(liveState(status, null, "open")))
afterEach(() => { clients.splice(0).forEach((client) => client.clear()); vi.clearAllMocks(); vi.unstubAllGlobals() })

describe("candle chart", () => {
  it("draws every bar in view, the last price, levels in range and pins the rest to the edge", () => {
    const html = renderToStaticMarkup(<CandleChart bars={bars} interval="5m" height={300} width={800} formatPrice={text}
      label="SPX 5-minute candles" levels={chartLevels("SPX", portfolio.positions, [])}
      band={{ lo: 6995, hi: 7005, label: "±1σ by Oct 16: ±5.00" }} />)
    expect(html).toContain('aria-label="SPX 5-minute candles"')
    // Every body, plus the plot clip, the band and axis tags for the last price, the 7000 strike and both band edges.
    expect(html.match(/<rect x=/g)?.length).toBe(bars.length + 6)
    expect(html).toContain("6,995.00")
    expect(html).toContain("7,005.00")
    // Ends in an up bar: close 7,019.75 on the axis and in the readout.
    expect(html).toContain("7,019.75")
    expect(html).toContain("Sep 22 14:25")
    expect(html).toContain("+0.50 (+0.01%)")
    // 7000 is in range and tagged on the axis; the 6800 put sits below it.
    expect(html).toContain('aria-label="7000 +2C Oct 16"')
    expect(html).toContain("7,000.00")
    expect(html).toContain("↓ 6800 −1P Sep 18")
    expect(html).toContain("±1σ by Oct 16: ±5.00")
    // The first labelled times fall on round hours.
    expect(html).toContain(">10:00<")
  })

  it("pins a band far wider than the bars to the edges instead of flattening them", () => {
    const html = renderToStaticMarkup(<CandleChart bars={bars} interval="5m" height={300} width={800} formatPrice={text}
      label="SPX" levels={chartLevels("SPX", portfolio.positions, [])} band={{ lo: 6000, hi: 8000, label: "±1σ by Dec 18: ±1,000" }} />)
    expect(html).toContain("↑ +1σ 8,000.00")
    // Below the chart, nearest first: the 6800 put, then the band's lower edge a line further down.
    const put = html.indexOf("↓ 6800 −1P Sep 18")
    const edge = html.indexOf("↓ −1σ 6,000.00")
    expect(put).toBeGreaterThan(0)
    expect(edge).toBeGreaterThan(put)
    expect(html).toContain('y="274"')
    expect(html).toContain('y="262"')
    expect(html).not.toContain("±1σ by Dec 18")
  })

  it("marks where a session resumes after a gap", () => {
    const overnight = [...bars, { t: bars[bars.length - 1]!.t + 7 * 3600, o: 7020, h: 7021, l: 7019, c: 7020.5 }]
    const html = renderToStaticMarkup(<CandleChart bars={overnight} interval="5m" height={300} width={800} formatPrice={text} label="SPX" />)
    expect(html).toContain(">21:25<")
    expect(html).toContain("fill-foreground")
  })

  it("moves axis tags apart just enough, inside the plot", () => {
    const tag = (y: number) => ({ price: y, y, text: String(y), color: "", filled: false })
    expect(stackTags([tag(50), tag(45), tag(200)], 8, 278).map((t) => t.y)).toEqual([45, 61, 200])
    expect(stackTags([tag(275), tag(270)], 8, 278).map((t) => t.y)).toEqual([262, 278])
    expect(stackTags([tag(2), tag(4)], 8, 278).map((t) => t.y)).toEqual([8, 24])
    expect(stackTags([], 8, 278)).toEqual([])
  })

  it("renders nothing to measure without a width", () => {
    const html = renderToStaticMarkup(<CandleChart bars={bars} interval="5m" height={300} formatPrice={text} label="SPX" />)
    expect(html).not.toContain("<svg")
    expect(html).toContain("C <span")  // the readout still shows the latest bar
  })
})

describe("price chart panel", () => {
  it("shows the underlying's candles with a legend for what is drawn over them", () => {
    const html = render(<PriceChart symbol="SPX" spot={7000} expiry={expiry} />, { symbol: "SPX", interval: "5m", bars })
    for (const expected of ["SPX <span", "5-minute candles", 'aria-label="Chart interval"', ">1D<", "Hide",
      "Long strike", "Short strike", "Expected move", "ET, market-data time"])
      expect(html).toContain(expected)
    expect(html).not.toContain("Trigger<")
    expect(html).not.toContain("No bars yet")
  })

  it("explains an empty history and can stay hidden", () => {
    expect(render(<PriceChart symbol="SPX" spot={7000} expiry={null} />, { symbol: "SPX", interval: "5m", bars: [] }))
      .toContain("No bars yet. They build from the feed")
    vi.stubGlobal("localStorage", { getItem: () => JSON.stringify({ interval: "1h", hidden: true }), setItem: () => {} })
    const hidden = render(<PriceChart symbol="SPX" spot={7000} expiry={expiry} />)
    expect(hidden).toContain("Show chart")
    expect(hidden).toContain("hourly candles")
    expect(hidden).not.toContain("Chart interval")
    expect(hidden).not.toContain("Loading bars")
  })
})

it("keeps the selected expiry's expected move when the chart is opened by hash route", () => {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(client)
  const later = { ...expiry, id: "2026-11-20PM", expiry: "2026-11-20", days: 58 }
  client.setQueryData(["summary", "SPX", 1], { ...summary, expiries: [expiry, later] })
  const html = renderToStaticMarkup(<QueryClientProvider client={client}><ChartView symbol="SPX" expiry={later.id} /></QueryClientProvider>)
  expect(html).toContain("One standard deviation by 2026-11-20")
  expect(html).not.toContain("One standard deviation by 2026-10-16")
})
