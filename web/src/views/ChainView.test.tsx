// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot } from "react-dom/client"
import { expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { account, chain, portfolio, quote, selection, status, summary } from "../test/trading-fixtures"
import { ChainView } from "./ChainView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../components/PriceChart", () => ({ PriceChart: () => null }))
const held = vi.hoisted(() => ({ data: undefined as unknown }))
const planned = vi.hoisted(() => ({ data: undefined as unknown }))
vi.mock("../api/trading", () => ({ usePortfolio: () => held, useAccount: () => planned }))
vi.mock("../components/OrderTicket", async (original) => ({ ...await original<typeof import("../components/OrderTicket")>(),
  OrderTicket: ({ selection, quote }: { selection: { cell: string }; quote: { bid: number | null } | null }) =>
    <><p data-testid="ticket">{selection.cell} ticket</p><p>Ticket bid {quote?.bid ?? "none"}</p></> }))
vi.mock("../components/StrategyTicket", () => ({
  StrategyTicket: ({ legs }: { legs: { strike: number; quote?: { bid: number | null } }[] }) => <p>{legs.map((leg) => `Leg ${leg.strike} bid ${leg.quote?.bid}`).join("; ")}</p> }))

it("toggles liquidity columns independently of Greeks", async () => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, trading: undefined }, null, "open"))
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  client.setQueryData(["summary", "SPX", 1], summary)
  client.setQueryData(["chain", "SPX", selection.expiry.id, .05, 1], chain)
  const host = document.createElement("div")
  const root = createRoot(host)
  try {
    await act(async () => root.render(<QueryClientProvider client={client}><ChainView symbol="SPX" expiry={selection.expiry.id} onExpiry={() => {}} /></QueryClientProvider>))
    const headers = () => [...host.querySelectorAll("th")].map((cell) => cell.textContent)
    const toggle = [...host.querySelectorAll("button")].find((button) => button.textContent === "Liquidity columns")!
    expect(headers().filter((name) => name === "Vol")).toHaveLength(2)
    await act(async () => toggle.click())
    expect(toggle.getAttribute("aria-pressed")).toBe("false")
    expect(headers()).not.toContain("Vol")
    expect(headers()).not.toContain("Spr %")
    expect(headers()).not.toContain("Liq")
    expect(headers()).toContain("OI")
    const greeks = [...host.querySelectorAll("button")].find((button) => button.textContent === "Greeks")!
    await act(async () => greeks.click())
    expect(headers()).toContain("Vega")
    expect(headers()).not.toContain("Vol")
    await act(async () => toggle.click())
    expect(headers()).toContain("Vega")
    expect(headers()).toContain("Vol")
  } finally {
    await act(async () => root.unmount())
    client.clear()
    vi.unstubAllGlobals()
  }
})

it("marks quotes paper orders cannot fill on, and the displayed size the account already took", async () => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  held.data = { ...portfolio, liquidity_used: [{ symbol: quote.symbol, bid_size: 10, ask_size: 3, bid_left: 10, ask_left: 1 }] }
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  client.setQueryData(["summary", "SPX", 1], summary)
  const put = { ...quote, symbol: "SPXW  261016P07000000", bid: null, bid_size: null, executable: false, quote_issue: "no_bid" as const }
  client.setQueryData(["chain", "SPX", selection.expiry.id, .05, 1], { ...chain, strikes: [{ ...chain.strikes[0]!, call: { ...quote, executable: true, quote_issue: null }, put }] })
  const host = document.createElement("div")
  const root = createRoot(host)
  try {
    await act(async () => root.render(<QueryClientProvider client={client}><ChainView symbol="SPX" expiry={selection.expiry.id} onExpiry={() => {}} /></QueryClientProvider>))
    const cells = [...host.querySelectorAll("button[aria-pressed]")]
    const callAsk = cells.find((cell) => cell.getAttribute("aria-label")?.startsWith("buy 7000 call at ask"))!
    expect(callAsk.textContent).toContain("1 left")
    expect(callAsk.getAttribute("title")).toContain("1 of 3 displayed left for your paper orders")
    const callBid = cells.find((cell) => cell.getAttribute("aria-label")?.startsWith("sell 7000 call at bid"))!
    expect(callBid.textContent).not.toContain("left")
    const putBid = cells.find((cell) => cell.getAttribute("aria-label")?.startsWith("sell 7000 put at bid"))!
    expect(putBid.getAttribute("title")).toContain("Not executable: One-sided: no bid")
  } finally {
    await act(async () => root.unmount())
    held.data = undefined
    client.clear()
    vi.unstubAllGlobals()
  }
})

it("marks a non-whitelisted underlying above the chain", async () => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  planned.data = { ...account, rules: { ...account.rules, underlyings: ["SPY"] } }
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  client.setQueryData(["summary", "SPX", 1], summary)
  client.setQueryData(["chain", "SPX", selection.expiry.id, .05, 1], chain)
  const host = document.createElement("div")
  const root = createRoot(host)
  try {
    await act(async () => root.render(<QueryClientProvider client={client}><ChainView symbol="SPX" expiry={selection.expiry.id} onExpiry={() => {}} /></QueryClientProvider>))
    expect(host.textContent).toContain("INSTRUMENT_NOT_ALLOWED: SPX")
    expect(host.textContent).toContain("closing orders still work")
  } finally {
    await act(async () => root.unmount())
    planned.data = undefined; client.clear(); vi.unstubAllGlobals()
  }
})

it("expands and collapses additional expiries without replacing the selected chain", async () => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  localStorage.clear()
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, trading: undefined }, null, "open"))
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  const later = { ...selection.expiry, id: "2026-11-20PM", expiry: "2026-11-20" }
  client.setQueryData(["summary", "SPX", 1], { ...summary, expiries: [...summary.expiries, later] })
  client.setQueryData(["chain", "SPX", selection.expiry.id, .05, 1], chain)
  client.setQueryData(["chain", "SPX", later.id, 0, 1], { ...chain, expiry: later })
  const host = document.createElement("div"), root = createRoot(host)
  try {
    await act(async () => root.render(<QueryClientProvider client={client}><ChainView symbol="SPX" expiry={selection.expiry.id} onExpiry={() => {}} /></QueryClientProvider>))
    await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Stack expiries")!.click())
    expect(host.querySelectorAll('table[aria-label="Option chain"]')).toHaveLength(1)
    const expand = [...host.querySelectorAll("button")].find((b) => b.textContent?.includes(later.id))!
    await act(async () => expand.click())
    expect(host.querySelectorAll('table[aria-label="Option chain"]')).toHaveLength(2)
    await act(async () => expand.click())
    expect(host.querySelectorAll('table[aria-label="Option chain"]')).toHaveLength(1)
  } finally {
    await act(async () => root.unmount()); client.clear(); localStorage.clear(); vi.unstubAllGlobals()
  }
})

it("keeps picked legs on Escape and clears the ticket and legs when the saved mode changes elsewhere", async () => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  localStorage.clear()
  localStorage.setItem("openport.chain.SPX", JSON.stringify({ mode: "strategy" }))
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  client.setQueryData(["summary", "SPX", 1], summary)
  client.setQueryData(["chain", "SPX", selection.expiry.id, .05, 1], chain)
  const host = document.createElement("div"), root = createRoot(host)
  const ask = () => [...host.querySelectorAll("button[aria-pressed]")].find((cell) => cell.getAttribute("aria-label")?.startsWith("buy 7000 call at ask")) as HTMLButtonElement
  const legs = () => host.querySelector('[aria-label="Strategy legs"]')
  const saveMode = async (mode: string, event: string) => {
    localStorage.setItem("openport.chain.SPX", JSON.stringify({ mode }))
    await act(async () => { window.dispatchEvent(new Event(event)) })
  }
  try {
    await act(async () => root.render(<QueryClientProvider client={client}><ChainView symbol="SPX" expiry={selection.expiry.id} onExpiry={() => {}} /></QueryClientProvider>))
    await act(async () => ask().click())
    expect(legs()?.textContent).toContain("1 of 4 legs")
    await act(async () => ask().dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true, cancelable: true })))
    expect(legs()?.textContent).toContain("1 of 4 legs")
    await saveMode("single", "storage")
    expect(legs()).toBeNull()
    await act(async () => ask().click())
    expect(host.querySelector('[data-testid="ticket"]')?.textContent).toBe("ask ticket")
    await saveMode("strategy", "openport:workspace")
    expect(host.querySelector('[data-testid="ticket"]')).toBeNull()
    await saveMode("single", "storage")
    expect(host.querySelector('[data-testid="ticket"]')).toBeNull()
    expect(legs()).toBeNull()
  } finally {
    await act(async () => root.unmount()); client.clear(); localStorage.clear(); vi.unstubAllGlobals()
  }
})

it("keeps quoting a ticket and strategy legs from the whole chain once the strike window leaves them out", async () => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  client.setQueryData(["summary", "SPX", 1], summary)
  const row = (strike: number, bid: number) => ({ ...chain.strikes[0]!, strike, call: { ...quote, symbol: `SPXW  261016C0${strike}000`, bid },
    put: { ...quote, symbol: `SPXW  261016P0${strike}000`, bid } })
  // The ±5% window holds both strikes; ±2% only the nearer one. The whole chain has moved on since.
  client.setQueryData(["chain", "SPX", selection.expiry.id, .05, 1], { ...chain, strikes: [row(7000, 4.5), row(7300, 1.5)] })
  client.setQueryData(["chain", "SPX", selection.expiry.id, .02, 1], { ...chain, strikes: [row(7000, 4.5)] })
  client.setQueryData(["chain", "SPX", selection.expiry.id, 0, 1], { ...chain, strikes: [row(7000, 4.5), row(7300, 1.25)] })
  const host = document.createElement("div")
  const root = createRoot(host)
  const press = (find: (button: HTMLButtonElement) => boolean) => act(async () => [...host.querySelectorAll("button")].find(find)!.click())
  const cell = (label: string) => press((button) => button.getAttribute("aria-label")?.startsWith(label) ?? false)
  const text = (label: string) => press((button) => button.textContent === label)
  try {
    await act(async () => root.render(<QueryClientProvider client={client}><ChainView symbol="SPX" expiry={selection.expiry.id} onExpiry={() => {}} /></QueryClientProvider>))
    await cell("buy 7300 call at ask")
    expect(host.textContent).toContain("Ticket bid 1.5")
    await text("±2%")
    expect(host.textContent).toContain("Ticket bid 1.25")
    await text("±5%")
    await text("Strategy")
    await cell("buy 7300 call at ask")
    await cell("sell 7000 call at bid")
    await text("Review")
    expect(host.textContent).toContain("Leg 7300 bid 1.5; Leg 7000 bid 4.5")
    await text("±2%")
    expect(host.textContent).toContain("Leg 7300 bid 1.25; Leg 7000 bid 4.5")
  } finally {
    await act(async () => root.unmount())
    client.clear(); vi.unstubAllGlobals()
  }
})
