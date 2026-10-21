// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { ChainTable } from "./ChainTable"
import { CandleChart } from "../charts/CandleChart"
import { ChartDrawings } from "./ChartDrawings"
import { HotkeyHelp, Popout } from "./TerminalTools"
import { searchSymbols, SymbolSearch, Watchlist } from "./Watchlist"
import { liveState, useLive } from "../api/live"
import { Positions } from "../views/PositionsView"
import { portfolio, quote, status, summary } from "../test/trading-fixtures"
import { defaultStudies } from "../lib/studies"
import { reviewTicketKey } from "../lib/keyboard"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
let host: HTMLDivElement, root: Root
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  HTMLDialogElement.prototype.showModal = function () { this.open = true }
  HTMLDialogElement.prototype.close = function () { this.open = false }
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.restoreAllMocks(); vi.unstubAllGlobals() })
const key = async (target: Element, key: string) => { await act(async () => target.dispatchEvent(new KeyboardEvent("keydown", { key, bubbles: true, cancelable: true }))) }
const rows = Array.from({ length: 5000 }, (_, i) => ({ strike: 5000 + i, iv: .2, gex: null, vex: null, call: { ...quote, symbol: `call${i}` }, put: { ...quote, symbol: `put${i}` } }))
it("windows thousands of rows and navigates to unmounted contracts before buying or selling", async () => {
  const choose = vi.fn()
  await act(async () => root.render(<ChainTable rows={rows} forward={7000} atmStrike={7000} showGreeks={false} showLiquidity={false} provider="demo" onQuote={choose} />))
  expect(host.querySelectorAll("tbody tr").length).toBeLessThan(30)
  const start = host.querySelector<HTMLButtonElement>('button[data-strike="7000"][data-side="call"][data-cell="ask"]')!
  expect(start).not.toBeNull()
  start.focus()
  await key(start, "End")
  expect((document.activeElement as HTMLElement).dataset.strike).toBe("9999")
  await key(document.activeElement!, "ArrowRight")
  expect((document.activeElement as HTMLElement).dataset.side).toBe("put")
  await key(document.activeElement!, "s")
  expect(choose).toHaveBeenLastCalledWith(expect.objectContaining({ symbol: "put4999" }), rows[4999], "put", "bid")
  await key(document.activeElement!, "b")
  expect(choose).toHaveBeenLastCalledWith(expect.anything(), rows[4999], "put", "ask")
})
it("shows only calls or puts and compact quote columns on a phone", async () => {
  vi.stubGlobal("matchMedia", vi.fn(() => ({ matches: true, addEventListener: vi.fn(), removeEventListener: vi.fn() })))
  await act(async () => root.render(<ChainTable rows={rows.slice(0, 3)} forward={5000} atmStrike={5000} showGreeks showLiquidity provider="demo" onQuote={() => {}} />))
  expect(host.querySelector('[data-side="put"]')).toBeNull()
  expect([...host.querySelectorAll("th")].map((e) => e.textContent)).not.toContain("Vega")
  await act(async () => host.querySelector<HTMLButtonElement>('[role="radio"]:last-child')!.click())
  expect(host.querySelector('[data-side="call"]')).toBeNull()
  expect(host.querySelector('[data-side="put"]')).not.toBeNull()
})
it("Enter on a focused quote opens the ticket for that cell's side", async () => {
  const choose = vi.fn()
  await act(async () => root.render(<ChainTable rows={rows.slice(0, 3)} forward={5001} atmStrike={5001} showGreeks={false} showLiquidity={false} provider="demo" onQuote={choose} />))
  const bid = host.querySelector<HTMLButtonElement>('button[data-strike="5001"][data-side="put"][data-cell="bid"]')!
  bid.focus()
  await key(bid, "Enter")
  expect(choose).toHaveBeenLastCalledWith(expect.objectContaining({ symbol: "put1" }), rows[1], "put", "bid")
  const ask = host.querySelector<HTMLButtonElement>('button[data-strike="5001"][data-side="call"][data-cell="ask"]')!
  ask.focus()
  await key(ask, "Enter")
  expect(choose).toHaveBeenLastCalledWith(expect.objectContaining({ symbol: "call1" }), rows[1], "call", "ask")
  expect(choose).toHaveBeenCalledTimes(2)
})
it("Enter reviews a ticket from non-editable content, leaving buttons and fields their own Enter", async () => {
  const submit = vi.fn((event: Event) => event.preventDefault())
  await act(async () => root.render(<form onKeyDownCapture={reviewTicketKey} onSubmit={(event) => submit(event.nativeEvent)}>
    <button type="button">Side</button><div role="radio" aria-checked tabIndex={0}>Buy</div><input /><p tabIndex={0}>Legs</p>
    <div data-ticket-review tabIndex={-1}>Preview</div><button type="submit">Send</button></form>))
  const enter = async (target: HTMLElement) => {
    target.focus()
    const event = new KeyboardEvent("keydown", { key: "Enter", bubbles: true, cancelable: true })
    await act(async () => { target.dispatchEvent(event) })
    return event
  }
  for (const target of [host.querySelector<HTMLElement>("button")!, host.querySelector<HTMLElement>('[role="radio"]')!, host.querySelector("input")!]) {
    expect((await enter(target)).defaultPrevented).toBe(false)
    expect(document.activeElement).toBe(target)
  }
  expect((await enter(host.querySelector("p")!)).defaultPrevented).toBe(true)
  expect(document.activeElement).toBe(host.querySelector("[data-ticket-review]"))
  expect(submit).not.toHaveBeenCalled()
})
it("describes Enter in a ticket as the docs do", async () => {
  await act(async () => root.render(<HotkeyHelp onClose={() => {}} />))
  expect(document.body.textContent).toContain("From a non-editable control, focus the order review; in a field, submit")
  expect(document.body.textContent).not.toContain("explicit submit still required")
})
it("searches only subscribed symbols and jumps with Enter", async () => {
  expect(searchSymbols(["SPX", "SPY", "QQQ"], " sp ")).toEqual(["SPX", "SPY"])
  const select = vi.fn(), close = vi.fn()
  await act(async () => root.render(<SymbolSearch onSymbol={select} onClose={close} />))
  await key(host.querySelector("input")!, "Enter")
  expect(select).toHaveBeenCalledWith("SPX")
  expect(close).toHaveBeenCalledOnce()
})
it("opens panels by hash route in a same-origin window that keeps its opener and write token", async () => {
  const open = vi.spyOn(window, "open").mockReturnValue(null)
  await act(async () => root.render(<Popout route={{ symbol: "SPX", view: "orders", expiry: null }} label="orders" />))
  const link = host.querySelector("a")!
  expect(link.getAttribute("href")).toBe("#/SPX/orders")
  expect(link.getAttribute("rel")).not.toMatch(/noopener|noreferrer/)
  await act(async () => link.click())
  expect(open).toHaveBeenCalledWith("#/SPX/orders", "_blank", expect.stringContaining("popup"))
  // noreferrer implies noopener, and either drops the sessionStorage copy holding the token.
  expect(open.mock.calls[0]![2]).not.toMatch(/noopener|noreferrer/)
  expect(open.mock.calls[0]![0]).not.toContain("token")
})
it("renders studies and drawings from actual candle anchors and offers a line as an alert", async () => {
  const bars = Array.from({ length: 25 }, (_, i) => ({ t: 1792157400 + i * 60, o: 100 + i, h: 102 + i, l: 99 + i, c: 101 + i }))
  const drawings = [{ id: "line", kind: "horizontal" as const, price: 110 }, { id: "trend", kind: "trend" as const, start: { time: bars[0]!.t, price: 101 }, end: { time: bars[24]!.t, price: 125 } }]
  const html = renderToStaticMarkup(<CandleChart bars={bars} interval="1m" width={800} height={300} label="Chart" formatPrice={String} drawings={drawings}
    studies={{ ...defaultStudies, sma: true, ema: true, bollinger: true }} />)
  for (const label of ["sma study", "ema study", "upper study", "lower study", "Drawn level 110", "Drawn trendline"]) expect(html).toContain(label)
  const alert = vi.fn()
  await act(async () => root.render(<ChartDrawings bars={bars} interval="1m" drawings={drawings} onChange={() => {}} onAlert={alert} />))
  await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Create price alert")!.click())
  expect(alert).toHaveBeenCalledWith(110)
})

it("F opens the focused position's existing close flow and ignores its pick input", async () => {
  const close = vi.fn()
  await act(async () => root.render(<Positions positions={portfolio.positions} onClose={close} selected={new Set()} onSelect={() => {}} />))
  const row = host.querySelector("tr[tabindex]")!
  await key(row, "f")
  expect(close).toHaveBeenCalledWith(portfolio.positions[0])
  await key(row.querySelector("input")!, "f")
  expect(close).toHaveBeenCalledTimes(1)
  const settling = host.querySelectorAll("tr[tabindex]")[1]!
  expect(settling.getAttribute("aria-label")).not.toContain("F to close")
  await key(settling, "f")
  expect(close).toHaveBeenCalledTimes(1)
})

it("labels watchlist IV rank as a percentage and derives change from the prior daily bar", async () => {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  client.setQueryData(["summary", "SPX", 1], summary)
  client.setQueryData(["volatility", "SPX", 1], { symbol: "SPX", iv_rank: .25, proxy: { used: true, name: "VIX" } })
  client.setQueryData(["watch-close", "SPX", 1], { symbol: "SPX", bars: [{ t: Date.parse("2026-09-22T20:00:00Z") / 1000, c: 6900 }] })
  try {
    await act(async () => root.render(<QueryClientProvider client={client}><Watchlist onSymbol={() => {}} /></QueryClientProvider>))
    await act(async () => host.querySelector("button")!.click())
    expect(host.textContent).toContain("25.0%")
    expect(host.textContent).toContain("proxy")
    expect(host.textContent).toContain("+1.45%")
    expect(host.textContent).toContain("351.44")
    expect(host.textContent).toContain(summary.as_of)
  } finally { client.clear() }
})
