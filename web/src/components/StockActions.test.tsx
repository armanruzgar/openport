// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api, ApiError } from "../api/client"
import { waitForRender } from "../test/render"
import { liveState, useLive } from "../api/live"
import type { Position } from "../api/trading-types"
import { account, portfolio, status } from "../test/trading-fixtures"
import { AbandonDialog, ExerciseDialog, ExerciseInstructionDialog, SettleDialog, TradeSharesDialog, shareEffect } from "./StockActions"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))

let root: Root
let host: HTMLDivElement
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
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
const type = async (input: HTMLInputElement, value: string) => {
  const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!
  await act(async () => { setter.call(input, value); input.dispatchEvent(new Event("input", { bubbles: true })) })
}

describe("settling by hand", () => {
  it("explains why, previews intrinsic value and posts the official value", async () => {
    const expired = { ...portfolio.positions[1]!, settle_by: "manual" } as Position  // a short SPX 6800 put, AM-settled
    const fetcher = vi.fn(async () => new Response(JSON.stringify({ account_version: "18", position_closed: true }), { status: 200 }))
    vi.stubGlobal("fetch", fetcher)
    const closed = vi.fn()
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <SettleDialog position={expired} trading={status.trading!} onClose={closed} /></QueryClientProvider>))
    expect(host.textContent).toContain("special opening quotation")
    const settle = [...host.querySelectorAll("button")].find((b) => b.textContent === "Settle")!
    expect(settle.disabled).toBe(true)
    await type(host.querySelector("input")!, "6790.50")
    expect(host.textContent).toContain("Intrinsic value $9.50 a share, −$950.00 for 1 short contract.")
    await act(async () => settle.click())
    expect(fetcher).toHaveBeenCalledWith("/api/settlements", expect.objectContaining({ method: "POST",
      body: JSON.stringify({ symbol: expired.symbol, value: "6790.50" }) }))
    expect(closed).toHaveBeenCalled()
  })
})

describe("disposing of a worthless position", () => {
  it("forfeits cash settlement for an instructed index long", async () => {
    const long = { ...portfolio.positions[0]!, do_not_exercise: true, awaiting_settlement: true } as Position
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <ExerciseInstructionDialog position={long} trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
    expect(host.textContent).toContain("settles in cash at intrinsic value")
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <SettleDialog position={long} trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
    await type(host.querySelector("input")!, String(long.strike + 100))
    expect(host.textContent).toContain("do-not-exercise instruction closes this long at zero")
    expect(host.textContent).toContain("Settlement value $0.00")
    expect(host.textContent).not.toContain("delivering")
  })
  it("abandons a long nobody bids for at zero", async () => {
    const long = { ...portfolio.positions[0]!, no_bid: true, mark: "0.03", market_value: "6.00" } as Position
    const fetcher = vi.fn(async () => new Response(JSON.stringify({ ...portfolio, positions: [] }), { status: 200 }))
    vi.stubGlobal("fetch", fetcher)
    const closed = vi.fn()
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <AbandonDialog position={long} trading={status.trading!} onClose={closed} /></QueryClientProvider>))
    expect(host.textContent).toContain("2 contracts leave the account at $0.00, without a fee, since nobody bids for them.")
    expect(host.textContent).toContain("$920.00")
    const abandon = [...host.querySelectorAll("button")].find((b) => b.textContent === "Abandon 2")!
    await act(async () => abandon.click())
    expect(fetcher).toHaveBeenCalledWith("/api/positions/abandon", expect.objectContaining({ method: "POST",
      body: JSON.stringify({ symbol: long.symbol }) }))
    expect(closed).toHaveBeenCalled()
  })

  it("warns that an expired long in the money forfeits its settlement", async () => {
    const expired = { ...portfolio.positions[0]!, awaiting_settlement: true, no_bid: true, strike: 6800 } as Position
    vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"),
      underlyings: [{ ...status.underlyings[0]!, symbol: "SPX", spot: 6810 }] } as ReturnType<typeof useLive>)
    vi.stubGlobal("fetch", vi.fn())
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <AbandonDialog position={expired} trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
    expect(host.textContent).toContain("instead of waiting for the settlement")
    expect(host.textContent).toContain("it is $10.00 in the money: the settlement may still pay $2,000.00")
  })

  it("gives and withdraws a do-not-exercise instruction", async () => {
    const call = { ...portfolio.positions[0]!, symbol: "SPY   261022C00500000", underlying: "SPY", strike: 500, do_not_exercise: false } as Position
    const fetcher = vi.fn(async () => new Response(JSON.stringify(portfolio), { status: 200 }))
    vi.stubGlobal("fetch", fetcher)
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <ExerciseInstructionDialog position={call} trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
    expect(host.textContent).toContain("expire worthless however far in the money, and no SPY shares change hands")
    await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Do not exercise")!.click())
    expect(fetcher).toHaveBeenCalledWith("/api/positions/instruction", expect.objectContaining({ method: "POST",
      body: JSON.stringify({ symbol: call.symbol, do_not_exercise: true }) }))
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <ExerciseInstructionDialog position={{ ...call, do_not_exercise: true }} trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
    expect(host.textContent).toContain("Withdraw the instruction")
    expect(host.textContent).toContain("buy 200 SPY shares")
    await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Exercise at expiry")!.click())
    expect(fetcher).toHaveBeenLastCalledWith("/api/positions/instruction", expect.objectContaining({
      body: JSON.stringify({ symbol: call.symbol, do_not_exercise: false }) }))
  })
})


describe("trading shares", () => {
  it.each([
    { flat_time: "10:00", code: "FLAT_TIME" },
    { underlyings: ["SPX"], code: "INSTRUMENT_NOT_ALLOWED" },
    { trading_start: "10:30", trading_end: "16:00", code: "OUTSIDE_PLAN_HOURS" },
  ])("keeps share reductions available under $code", async ({ code, ...restrictions }) => {
    const time = "2026-09-22T14:00:00Z"
    vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"), underlyings: [
      { ...status.underlyings[0]!, symbol: "SPY", spot: 510, as_of: time },
    ] } as ReturnType<typeof useLive>)
    vi.spyOn(api, "portfolio").mockResolvedValue({ ...portfolio, stocks: [{ symbol: "SPY", shares: 100 }] } as typeof portfolio)
    vi.spyOn(api, "account").mockResolvedValue({ ...account, time, rules: { ...account.rules, ...restrictions } })
    vi.spyOn(api, "previewStock").mockRejectedValue(new Error("fixture preview"))
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <TradeSharesDialog trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
    await waitForRender(() => expect(host.textContent).toContain(code))
    expect([...host.querySelectorAll("button")].find((b) => b.textContent === "Buy 100 SPY")!.disabled).toBe(true)
    await waitForRender(() => expect(host.textContent).toContain("You hold 100 long"))
    await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Sell")!.click())
    expect(host.textContent).not.toContain(code)
    expect([...host.querySelectorAll("button")].find((b) => b.textContent === "Sell 100 SPY")!.disabled).toBe(false)
  })
  it("blocks unprotected share entries but keeps reductions available", async () => {
    vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"), underlyings: [
      { ...status.underlyings[0]!, symbol: "SPY", spot: 510 },
    ] } as ReturnType<typeof useLive>)
    vi.spyOn(api, "portfolio").mockResolvedValue({ ...portfolio, stocks: [{ symbol: "SPY", shares: 100 }] } as typeof portfolio)
    vi.spyOn(api, "account").mockResolvedValue({ ...account, rules: { ...account.rules, require_stop_loss: true } })
    vi.spyOn(api, "previewStock").mockRejectedValue(new Error("fixture preview"))
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <TradeSharesDialog trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
    await waitForRender(() => expect(host.textContent).toContain("Stop-loss required by this plan"))
    expect([...host.querySelectorAll("button")].find((b) => b.textContent === "Buy 100 SPY")!.disabled).toBe(true)
    await waitForRender(() => expect(host.textContent).toContain("You hold 100 long"))
    await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Sell")!.click())
    expect(host.textContent).not.toContain("Stop-loss required by this plan")
    expect([...host.querySelectorAll("button")].find((b) => b.textContent === "Sell 100 SPY")!.disabled).toBe(false)
  })
  it("names opens, adds, reductions, closes and reversals", () => {
    expect(shareEffect(0, "buy", 100)).toBe("opens")
    expect(shareEffect(100, "buy", 50)).toBe("adds to")
    expect(shareEffect(100, "sell", 40)).toBe("reduces")
    expect(shareEffect(-100, "buy", 100)).toBe("closes")
    expect(shareEffect(100, "sell", 150)).toBe("reverses")
  })
  it("filters indexes, previews the account and posts a whole-share purchase", async () => {
    vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"), underlyings: [
      { ...status.underlyings[0]!, symbol: "SPY", spot: 510 }, status.underlyings[0]!,
    ] } as ReturnType<typeof useLive>)
    vi.spyOn(api, "portfolio").mockResolvedValue({ ...portfolio, stocks: [] })
    vi.spyOn(api, "account").mockResolvedValue(account)
    const fetcher = vi.fn(async (url: string) => new Response(JSON.stringify(url.includes("/preview") ? {
      account_version: "17", simulated: true, decision: "ok", reason: null, price: "510.00", cost: "51000.00",
      current: { buying_power: "100000.00", exposure: { dollar_delta: 0 } },
      after: { buying_power: "49000.00", exposure: { dollar_delta: 51000 } },
    } : portfolio), { status: 200 }))
    vi.stubGlobal("fetch", fetcher)
    const closed = vi.fn()
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <TradeSharesDialog trading={status.trading!} onClose={closed} /></QueryClientProvider>))
    expect([...host.querySelector("select")!.options].map((o) => o.value)).toEqual(["SPY"])
    await waitForRender(() => expect(host.textContent).toContain("$100,000.00 → $49,000.00"))
    expect(host.textContent).toContain("Share delta change: +100")
    const button = [...host.querySelectorAll("button")].find((b) => b.textContent === "Buy 100 SPY")!
    await act(async () => button.click())
    expect(fetcher).toHaveBeenCalledWith("/api/stocks/trade", expect.objectContaining({ method: "POST",
      body: JSON.stringify({ symbol: "SPY", side: "buy", shares: 100 }) }))
    expect(closed).toHaveBeenCalled()
  })
  it.each(["passed", "failed"] as const)("allows only reducing shares after an attempt %s", async (evaluationStatus) => {
    vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"), underlyings: [
      { ...status.underlyings[0]!, symbol: "SPY", spot: 510 },
    ] } as ReturnType<typeof useLive>)
    vi.spyOn(api, "portfolio").mockResolvedValue({ ...portfolio, stocks: [{ symbol: "SPY", shares: 100, basis: "50000", average_price: "500", mark: "510",
      mark_time: portfolio.time, fresh: true, market_value: "51000", unrealised: "1000", realised: "0", fees: "0",
      attribution: { delta: 0, gamma: 0, vega: 0, theta: 0, other: 0, costs: 0, total: 0 } }] })
    vi.spyOn(api, "account").mockResolvedValue({ ...account, evaluation: { ...account.evaluation, status: evaluationStatus } })
    vi.spyOn(api, "previewStock").mockRejectedValue(new Error("fixture preview"))
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <TradeSharesDialog trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
    await waitForRender(() => expect(host.textContent).toContain("Only trades that reduce shares"))
    expect([...host.querySelectorAll("button")].find((b) => b.textContent === "Buy 100 SPY")!.disabled).toBe(true)
    await act(async () => (host.querySelector('[role="radio"][aria-checked="false"]') as HTMLButtonElement).click())
    expect([...host.querySelectorAll("button")].find((b) => b.textContent === "Sell 100 SPY")!.disabled).toBe(false)
    await type(host.querySelector("input")!, "101")
    expect([...host.querySelectorAll("button")].find((b) => b.textContent === "Sell 101 SPY")!.disabled).toBe(true)
  })
  it("places a working bracketed limit and cancels working share orders", async () => {
    vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"), underlyings: [
      { ...status.underlyings[0]!, symbol: "SPY", spot: 510 },
    ] } as ReturnType<typeof useLive>)
    const working = { id: "4", client_order_id: "x", actor: "web", symbol: "SPY", side: "sell" as const, type: "stop" as const, time_in_force: "gtc" as const,
      shares: 50, limit_price: null, stop_price: "490.00", extended_hours: false, bracket: null, role: "stop_loss" as const, parent: "3", oco: null,
      stop_loss: null, take_profit: null, status: "armed" as const, reason: null, accepted_at: portfolio.time, day_end: null, triggered_at: null,
      ended_at: null, filled_shares: 0, fill_price: null, stock_fill: null }
    vi.spyOn(api, "portfolio").mockResolvedValue({ ...portfolio, stocks: [], stock_orders: [working] })
    vi.spyOn(api, "account").mockResolvedValue({ ...account, rules: { ...account.rules, require_stop_loss: true } })
    const preview = vi.spyOn(api, "previewStockOrder").mockResolvedValue({ account_version: "17", simulated: true, decision: "ok", reason: null,
      price: "510.00", cost: "50500.00", current: { buying_power: "100000.00" }, after: { buying_power: "49500.00" } } as never)
    vi.spyOn(api, "previewStock").mockRejectedValue(new Error("fixture preview"))
    const place = vi.spyOn(api, "placeStockOrder").mockResolvedValue({} as never)
    const cancel = vi.spyOn(api, "cancelStockOrder").mockResolvedValue({} as never)
    vi.stubGlobal("crypto", { randomUUID: () => "share-order-1" })
    const closed = vi.fn()
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <TradeSharesDialog trading={status.trading!} onClose={closed} /></QueryClientProvider>))
    await waitForRender(() => expect(host.textContent).toContain("Working share orders"))
    expect(host.textContent).toContain("Sell 50 SPY · stop · stop $490.00 · GTC · stop-loss · waiting for its stop")
    // A plan requiring a stop opens shares only with a bracket stop.
    await waitForRender(() => expect(host.textContent).toContain("Stop-loss required by this plan"))
    const kind = host.querySelector<HTMLSelectElement>('select[aria-label="Share order type"]')!
    await act(async () => { kind.value = "limit"; kind.dispatchEvent(new Event("change", { bubbles: true })) })
    const input = (label: string) => [...host.querySelectorAll("label")].find((l) => l.textContent?.startsWith(label))!.querySelector("input")!
    await type(input("Limit price"), "505.00")
    await type(input("Bracket stop-loss"), "495.00")
    await type(input("Bracket take-profit"), "530.00")
    await act(async () => input("Extended hours").click())
    expect(host.textContent).not.toContain("Stop-loss required by this plan")
    await waitForRender(() => expect(host.textContent).toContain("$100,000.00 → $49,500.00"))
    expect(preview).toHaveBeenLastCalledWith(expect.objectContaining({ type: "limit", limit_price: "505.00", extended_hours: true,
      bracket: { stop_loss: "495.00", take_profit: "530.00" } }), status.trading!.write)
    const button = [...host.querySelectorAll("button")].find((b) => b.textContent === "Buy 100 SPY · limit")!
    await act(async () => button.click())
    expect(place).toHaveBeenCalledWith({ client_order_id: "share-order-1", symbol: "SPY", side: "buy", type: "limit", shares: 100, time_in_force: "day",
      limit_price: "505.00", extended_hours: true, bracket: { stop_loss: "495.00", take_profit: "530.00" } }, status.trading!.write)
    expect(closed).toHaveBeenCalled()
    await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Cancel 4")!.click())
    expect(cancel).toHaveBeenCalledWith("4", status.trading!.write)
  })
  it("blocks invalid share counts and shows server refusals", async () => {
    vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"), underlyings: [
      { ...status.underlyings[0]!, symbol: "SPY", spot: 510 },
    ] } as ReturnType<typeof useLive>)
    vi.spyOn(api, "portfolio").mockResolvedValue({ ...portfolio, stocks: [] })
    vi.spyOn(api, "account").mockResolvedValue(account)
    vi.stubGlobal("fetch", vi.fn(async () => new Response(JSON.stringify({
      decision: "SESSION_CLOSED", reason: { code: "SESSION_CLOSED", message: "Stock trades in the regular session" },
      current: { buying_power: "100000.00" }, after: { buying_power: "100000.00" },
    }), { status: 200 })))
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <TradeSharesDialog trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
    await waitForRender(() => expect(host.textContent).toContain("SESSION_CLOSED"))
    await type(host.querySelector("input")!, "1.5")
    expect([...host.querySelectorAll("button")].find((b) => b.textContent?.startsWith("Buy "))!.disabled).toBe(true)
  })
})


it("shows the server's buy-only put exercise refusal", async () => {
  const put = { ...portfolio.positions[0]!, symbol: "SPY   261022P00520000", underlying: "SPY", strike: 520, type: "put" } as Position
  vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"), underlyings: [
    { ...status.underlyings[0]!, symbol: "SPY", spot: 510 },
  ] } as ReturnType<typeof useLive>)
  const message = "This plan is buy-only: put exercise would sell shares short; hold enough long shares first"
  vi.spyOn(api, "exercise").mockRejectedValue(new ApiError(422, message, "BUY_ONLY"))
  const closed = vi.fn()
  await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
    <ExerciseDialog position={put} trading={status.trading!} onClose={closed} /></QueryClientProvider>))
  await act(async () => [...host.querySelectorAll("button")].find((button) => button.textContent?.startsWith("Exercise "))!.click())
  expect(host.textContent).toContain(message)
  expect(host.textContent).toContain("BUY_ONLY")
  expect(closed).not.toHaveBeenCalled()
})
