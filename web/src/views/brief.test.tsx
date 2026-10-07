// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, type ReactNode } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import { CandleChart } from "../charts/CandleChart"
import { PriceChart } from "../components/PriceChart"
import { activeAccount } from "../lib/active-account"
import { loadBriefLevels, saveBriefLevels } from "../lib/brief-preferences"
import { dataSource } from "../lib/data-source"
import { formatRoute, navigableViews, parseRoute } from "../lib/route"
import { briefAccount, briefRisk, briefSummary, briefTime, briefVolatility, sessionMove } from "../test/brief-fixtures"
import { expiry, order, portfolio, status, trading } from "../test/trading-fixtures"
import { BriefView } from "./BriefView"

vi.mock("../api/live", async original => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../charts/useSize", () => ({ useSize: () => [{ current: null }, { width: 700, height: 250 }] }))
let host: HTMLDivElement, root: Root, client: QueryClient
const day = "2026-09-23"
const minute = Math.floor(Date.parse(briefTime) / 60000)
const daily = { symbol: "SPX", interval: "1d" as const, bars: [{ t: Date.parse("2026-09-22T13:30:00Z") / 1000, o: 7000, h: 7050, l: 6950, c: 7000 }] }
const intraday = { symbol: "SPX", interval: "1m" as const, bars: [{ t: Date.parse("2026-09-23T11:59:00Z") / 1000, o: 7060, h: 7080, l: 7050, c: 7070 }] }
const notes = { [day]: { plan: "Wait for the opening range", review: "Existing review" } }
function seed(scope = 0) {
  const queries = tradingQueries(scope, "17", true)
  client.setQueryData(queries.account.queryKey, briefAccount)
  client.setQueryData(queries.risk.queryKey, briefRisk)
  client.setQueryData(queries.portfolio.queryKey, { ...portfolio, positions: [{ ...portfolio.positions[0]!, expiry: day, expiry_time: "2026-09-23T20:00:00Z", last_trade_time: "2026-09-23T20:00:00Z" }], stocks: [] })
  client.setQueryData(queries.orders.queryKey, { account_version: "17", orders: [order] })
  client.setQueryData(queries.trades("current").queryKey, { account_version: "17", trades: [], attempt: 2, day_notes: notes })
  client.setQueryData(["playbooks", scope, "17"], { definitions: {}, modes: {}, staged: [], reasons: {}, auto_allowed: false })
  client.setQueryData(["summary", "SPX", 1], briefSummary)
  client.setQueryData(["volatility", "SPX", 1], briefVolatility)
  client.setQueryData(["brief-series", "SPX", "live/", minute], { symbol: "SPX", interval: "1d", fields: [], rows: [{ t: Date.parse("2026-09-22T20:00Z") / 1000, mfiv30: 18, atm30: 20, rr25: -3, sources: {} }] })
  client.setQueryData(["brief-candles", "SPX", "live/", "1d", minute], daily)
  client.setQueryData(["brief-candles", "SPX", "live/", "1m", minute], intraday)
}
const currentStatus = { ...status, underlyings: [{ ...status.underlyings[0]!, as_of: briefTime }] }
const render = async (node: ReactNode = <BriefView symbol="SPX" />) => act(async () => { root.render(<QueryClientProvider client={client}>{node}</QueryClientProvider>) })
const edit = async (value: string) => act(async () => {
  const textarea = host.querySelector("textarea")!
  Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, "value")!.set!.call(textarea, value)
  textarea.dispatchEvent(new Event("input", { bubbles: true }))
})
const submit = async () => act(async () => { host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })) })
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.stubGlobal("fetch", vi.fn(() => new Promise(() => {})))
  localStorage.clear()
  vi.mocked(useLive).mockReturnValue(liveState(currentStatus, null, "open"))
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity } } })
  seed()
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  activeAccount.set("main"); dataSource.set("live"); vi.restoreAllMocks(); vi.unstubAllGlobals()
})

describe("morning brief", () => {
  it("renders parity, changes, basis, backwardation, deadlines and modelling caveats", async () => {
    await render()
    for (const text of ["≈", "parity-inferred spot", "+70.00 pts (+1.00%)", "overnight / pre-market", "20.00 †", "+2.00 Δ", "42 of 252 sessions", "20 VIX proxy sessions", "backwardation", "modelling convention", "auto-close 15:55 ET", "$4,267.50", "Day plan"])
      expect(host.textContent).toContain(text)
    expect(host.textContent).not.toContain("NaN")
    expect(fetch).not.toHaveBeenCalled()
  })
  it.each(["2026-09-24T12:00:00Z", "2026-09-23T21:00:00Z"])("does not let a shared breaker at %s advance a stalled snapshot", async marketTime => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...currentStatus, circuit_breaker: { symbol: "SPY", day: "2026-09-24", market_time: marketTime, previous_close: { date: "2026-09-23", price: 700 }, level: 0, halts: [], active: false, error: null } }, null, "open"))
    await render()
    expect(host.textContent).toContain("SPX · 2026-09-23")
    expect(host.textContent).toContain("Prior close · 2026-09-22")
    expect(host.textContent).toContain("Plan before the open · 2026-09-23")
  })
  it("renders missing candles, series and exposure with reasons instead of zeroes", async () => {
    client.setQueryData(["summary", "SPX", 1], { ...briefSummary, spot: null, exposure: { gex: null, gamma_flip: null, call_wall: null, put_wall: null } })
    client.setQueryData(["brief-candles", "SPX", "live/", "1d", minute], { ...daily, bars: [] })
    client.setQueryData(["brief-candles", "SPX", "live/", "1m", minute], { ...intraday, bars: [] })
    client.setQueryData(["brief-series", "SPX", "live/", minute], { symbol: "SPX", interval: "1d", rows: [] })
    await render()
    for (const text of ["no prior-session bar", "no overnight candles", "no series history yet", "no usable exposure", "no levels available"]) expect(host.textContent).toContain(text)
    expect(host.textContent).not.toContain("+0.00")
  })
  it("does not fetch unsubscribed symbols or invent a market date", async () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, trading: undefined, underlyings: [] }, null, "open"))
    await render(<BriefView symbol={null} />)
    expect(host.textContent).toContain("not subscribed")
    expect(host.textContent).toContain("Market date unavailable")
    expect(host.textContent).toContain("paper trading is not enabled")
    expect(fetch).not.toHaveBeenCalled()
  })
  it("shows event labels as escaped text and pending limits and guardrails", async () => {
    client.setQueryData(["volatility", "SPX", 1], { ...briefVolatility, implied_moves: { ...briefVolatility.implied_moves, sessions: [{ ...sessionMove(day), label: "<Event> at 14:00" }] } })
    client.setQueryData(tradingQueries(0, "17", true).risk.queryKey, { ...briefRisk, pending_limits: { ...briefRisk.limits, max_daily_loss: "6000" }, pending_guardrails: { ...briefRisk.guardrails, max_opening_trades: 10 } })
    await render()
    expect(host.textContent).toContain("<Event> at 14:00")
    expect(host.querySelector("event")).toBeNull()
    expect(host.textContent).toContain("changes pending")
    expect(host.textContent).toContain("Pending limits · next trading day")
    expect(host.textContent).toContain("6000")
    expect(host.textContent).toContain("Pending guardrails")
  })
  it("saves the plan before the close and preserves the existing review", async () => {
    const save = vi.spyOn(api, "annotateDay").mockImplementation(async (date, note) => ({ account_version: "18", day: date, note }))
    await render(); await edit("Wait for a pullback"); await submit()
    expect(save).toHaveBeenCalledWith(day, { plan: "Wait for a pullback", review: "Existing review" }, "open")
    expect(host.textContent).toContain("Saved")
    expect(host.querySelector("textarea")?.value).toBe("Wait for a pullback")
  })
  it("prompts for a review at the close, preserves an unsaved plan, and saves both", async () => {
    const save = vi.spyOn(api, "annotateDay").mockImplementation(async (date, note) => ({ account_version: "18", day: date, note }))
    await render(); await edit("New plan draft")
    vi.mocked(useLive).mockReturnValue(liveState({ ...currentStatus, underlyings: [{ ...currentStatus.underlyings[0]!, as_of: "2026-09-23T20:00:00Z" }] }, null, "open"))
    await render()
    expect(host.textContent).toContain("Review after the close")
    expect(host.textContent).toContain("Plan: New plan draft")
    await edit("Kept the limit"); await submit()
    expect(save).toHaveBeenCalledWith(day, { plan: "New plan draft", review: "Kept the limit" }, "open")
  })
  it("uses session rows to switch to review on an early close", async () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...currentStatus, underlyings: [{ ...currentStatus.underlyings[0]!, as_of: "2026-11-27T18:00:00Z" }] }, null, "open"))
    client.setQueryData(["volatility", "SPX", 1], { ...briefVolatility, as_of: "2026-11-27T18:00:00Z", implied_moves: { ...briefVolatility.implied_moves, sessions: [sessionMove("2026-11-30")] } })
    await render()
    expect(host.textContent).toContain("Review after the close · 2026-11-27")
  })
  it("keeps a failed draft and shows errors; read-only mode prevents saving", async () => {
    const save = vi.spyOn(api, "annotateDay").mockRejectedValue(new Error("Note rejected"))
    await render(); await edit("Keep this draft"); await submit()
    expect(host.querySelector('[role="alert"]')?.textContent).toContain("Note rejected")
    expect(host.querySelector("textarea")?.value).toBe("Keep this draft")
    vi.mocked(useLive).mockReturnValue(liveState({ ...currentStatus, trading: { ...trading, write: "disabled" } }, null, "open"))
    await render(); await submit()
    expect(host.querySelector<HTMLButtonElement>('button[type="submit"]')?.disabled).toBe(true)
    expect(save).toHaveBeenCalledTimes(1)
  })
  it("drops the previous account draft and ignores its late save response", async () => {
    let resolve!: (value: Awaited<ReturnType<typeof api.annotateDay>>) => void
    vi.spyOn(api, "annotateDay").mockReturnValue(new Promise(done => { resolve = done }))
    await render(); await edit("Account A draft"); await submit()
    seed(1)
    client.setQueryData(tradingQueries(1, "17", true).trades("current").queryKey, { account_version: "17", trades: [], attempt: 2, day_notes: { [day]: { plan: "Account B plan", review: "" } } })
    vi.mocked(useLive).mockReturnValue(liveState(currentStatus, null, "open", 1))
    await render()
    await act(async () => resolve({ account_version: "18", day, note: { plan: "Account A draft", review: "" } }))
    expect(host.querySelector("textarea")?.value).toBe("Account B plan")
    expect(host.querySelector('[role="status"]')).toBeNull()
  })
  it("routes note writes to the selected account and to replay", async () => {
    vi.mocked(fetch).mockImplementation(async (_url, init) => init?.method === "PUT" ? { ok: true, json: async () => ({ account_version: "18", day, note: notes[day] }) } as Response : new Promise(() => {}))
    activeAccount.set("second")
    await render(); await submit()
    expect(fetch).toHaveBeenCalledWith(`/api/days/${day}/note?account=second`, expect.objectContaining({ method: "PUT" }))
    dataSource.set("replay")
    // A changed destination requires reopening the editor.
    await act(async () => root.render(null))
    await render()
    await submit()
    expect(fetch).toHaveBeenCalledWith(`/api/replay/days/${day}/note`, expect.objectContaining({ method: "PUT" }))
  })
  it("isolates replay data and labels simulated prices even while snapshots are loading", async () => {
    const live = liveState({ ...currentStatus, provider: { ...currentStatus.provider, simulated: true } }, null, "open", 1)
    vi.mocked(useLive).mockReturnValue({ ...live, source: "replay", replay: { id: "run-two", file: "scenario", provider: "demo", demo: true, symbols: ["SPX"], started: briefTime, delay_seconds: 0, speed: 1, paused: true, finished: false, time: briefTime } })
    dataSource.set("replay")
    await render()
    expect(host.textContent).toContain("Simulated prices")
    expect(host.textContent).not.toContain("7,070.00")
    const urls = vi.mocked(fetch).mock.calls.map(call => String(call[0]))
    expect(urls.every(url => url.startsWith("/api/replay/"))).toBe(true)
  })
  it("fetches only the selected symbol and refreshes snapshot data on a version change", async () => {
    const live = liveState({ ...currentStatus, underlyings: [...currentStatus.underlyings, { ...currentStatus.underlyings[0]!, symbol: "QQQ" }] }, null, "open")
    vi.mocked(useLive).mockReturnValue(live)
    await render()
    expect(fetch).not.toHaveBeenCalled()
    vi.mocked(useLive).mockReturnValue({ ...live, version: () => 2 })
    await render()
    const urls = vi.mocked(fetch).mock.calls.map(call => String(call[0]))
    expect(urls).toEqual(expect.arrayContaining(["/api/underlyings/SPX/summary", "/api/underlyings/SPX/volatility"]))
    expect(urls.some(url => url.includes("QQQ"))).toBe(false)
    expect(urls.some(url => url.includes("candles") || url.includes("series"))).toBe(false)
  })
  it("remembers chart levels in the browser and draws them through the existing chart", async () => {
    await render()
    await act(async () => host.querySelector<HTMLInputElement>('input[type="checkbox"]')!.click())
    expect(loadBriefLevels()).toBe(true)
    client.setQueryData(["candles", "SPX", "5m", 1], { ...intraday, interval: "5m" })
    await render(<PriceChart symbol="SPX" spot={7070} expiry={expiry} />)
    expect(host.textContent).toContain("Brief levels")
    expect(host.querySelector('svg[aria-label="SPX 5-minute candles"]')?.textContent).toContain("Prior high")
    await act(async () => saveBriefLevels(false))
    expect(host.querySelector('svg[aria-label="SPX 5-minute candles"]')?.textContent).not.toContain("Prior high")
  })
  it("uses existing chart lines for coincident and off-screen brief levels", async () => {
    await render(<CandleChart bars={intraday.bars} interval="1m" height={220} width={700} label="Brief lines" formatPrice={value => value.toFixed(2)} levels={[
      { kind: "brief", price: 7070, label: "Prior high / Call wall" }, { kind: "brief", price: 6950, label: "Gamma flip" },
    ]} />)
    expect(host.querySelector('[aria-label="Prior high / Call wall"]')).not.toBeNull()
    expect(host.textContent).toContain("↓ Gamma flip")
  })
  it("keeps Brief in sidebar number-key order for trading and analytics-only servers", () => {
    expect(navigableViews(true, false)[7]).toBe("brief")
    expect(navigableViews(false, false)[1]).toBe("brief")
    expect(parseRoute(formatRoute({ symbol: "SPX", view: "brief", expiry: null })).view).toBe("brief")
  })
})
