import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { Account, Order } from "../api/trading-types"
import { StrategyTicket, netText } from "../components/StrategyTicket"
import type { StrategyLeg } from "../lib/strategy"
import { account, expiry, order, portfolio, quote, risk, status } from "../test/trading-fixtures"
import { OrdersView, netLabel } from "./OrdersView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const clients: QueryClient[] = []
function render(node: ReactNode, value: Account = account, orders: Order[] = [], positions = portfolio.positions) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(client)
  const queries = tradingQueries(0, "17", true)
  client.setQueryData(queries.account.queryKey, value)
  client.setQueryData(queries.portfolio.queryKey, { ...portfolio, positions })
  client.setQueryData(queries.risk.queryKey, risk)
  client.setQueryData(queries.allOrders.queryKey, { account_version: "17", orders })
  client.setQueryData(queries.fills.queryKey, { account_version: "17", fills: [] })
  return renderToStaticMarkup(<QueryClientProvider client={client}>{node}</QueryClientProvider>)
}
const trading = { ...status.trading!, fee_per_contract: "0.65" }
const put = (strike: number, side: "buy" | "sell", bid: number, ask: number): StrategyLeg => ({
  symbol: `SPXW  261016P0${strike}000`, underlying: "SPX", side, ratio: 1, type: "put", strike, expiry: expiry.id,
  quote: { ...quote, symbol: `SPXW  261016P0${strike}000`, bid, ask, mid: (bid + ask) / 2 },
})
const spread = [put(6900, "sell", 5, 5.2), put(6890, "buy", 4, 4.2)]
const any: Account = { ...account, rules: { ...account.rules, buy_only: false } }
beforeEach(() => vi.mocked(useLive).mockReturnValue(liveState(status, null, "open")))
afterEach(() => { clients.splice(0).forEach((client) => client.clear()); vi.clearAllMocks() })

describe("strategy ticket", () => {
  it("blocks flat-time openings and permits reducing every leg", () => {
    const value = { ...any, time: "2026-09-23T19:45:00Z", rules: { ...any.rules, flat_time: "15:45" } }
    const held = spread.map((leg) => ({ ...portfolio.positions[0]!, symbol: leg.symbol, quantity: leg.side === "buy" ? -1 : 1 }))
    const ticket = <StrategyTicket legs={spread} onLegs={() => {}} expiries={[expiry]} underlying="SPX" spot={7000}
      trading={trading} onClose={() => {}} />
    const opening = render(ticket, value, [], [])
    expect(opening).toContain("FLAT_TIME")
    expect(opening).toMatch(/aria-label="Submit strategy order"[^>]*disabled=""/)
    const closing = render(ticket, value, [], held)
    expect(closing).not.toContain("FLAT_TIME")
    expect(closing).not.toMatch(/aria-label="Submit strategy order"[^>]*disabled=""/)
  })
  it("offers walking limits and describes inside fills", () => {
    const custom = { ...any, rules: { ...any.rules, inside_fill_percent: 50 } }
    const html = render(<StrategyTicket legs={spread} onLegs={() => {}} expiries={[expiry]} underlying="SPX" spot={7000} trading={trading} onClose={() => {}} />, custom)
    expect(html).toContain('aria-label="Walk limit"')
    expect(html).toContain("Inside fills enabled at 50%")
  })
  it("allows reducing every leg while latched and blocks new or oversized spreads", () => {
    const latched = { ...trading, kill_latched: true }
    const held = spread.map((leg) => ({ ...portfolio.positions[0]!, symbol: leg.symbol, quantity: leg.side === "buy" ? -1 : 1 }))
    const ticket = (units: number) => <StrategyTicket legs={spread} onLegs={() => {}} expiries={[expiry]} underlying="SPX" spot={7000}
      trading={latched} onClose={() => {}} units={units} />
    const close = render(ticket(1), any, [], held)
    expect(close).toContain("reduce-only: closing orders and exits still work")
    expect(close).not.toMatch(/aria-label="Submit strategy order"[^>]*disabled=""/)
    expect(render(ticket(1), account, [], held)).not.toMatch(/aria-label="Submit strategy order"[^>]*disabled=""/)
    expect(render(ticket(2), any, [], held)).toMatch(/aria-label="Submit strategy order"[^>]*disabled=""/)
    expect(render(ticket(1), any, [], [])).toMatch(/aria-label="Submit strategy order"[^>]*disabled=""/)
  })
  it.each(["passed", "failed"] as const)("allows closing a %s attempt and blocks opening or oversized legs", (status) => {
    const decided = { ...any, evaluation: { ...any.evaluation, status } }
    const held = spread.map((leg) => ({ ...portfolio.positions[0]!, symbol: leg.symbol, quantity: leg.side === "buy" ? -1 : 1 }))
    const ticket = (units: number) => <StrategyTicket legs={spread} onLegs={() => {}} expiries={[expiry]} underlying="SPX" spot={7000}
      trading={trading} onClose={() => {}} units={units} />
    const close = render(ticket(1), decided, [], held)
    expect(close).toContain("Closing orders are allowed")
    expect(close).not.toMatch(/aria-label="Submit strategy order"[^>]*disabled=""/)
    expect(render(ticket(2), decided, [], held)).toMatch(/aria-label="Submit strategy order"[^>]*disabled=""/)
    expect(render(ticket(1), decided, [], [])).toMatch(/aria-label="Submit strategy order"[^>]*disabled=""/)
  })
  it("leaves buying power to the server for portfolio margin or slippage", () => {
    for (const optional of [{}, { margin: "portfolio" as const }, { slippage_ticks: 2 }]) {
      // No buying power left: only the server's preview and checks may hold the order back.
      const custom = { ...any, rules: { ...any.rules, ...optional }, buying_power: { ...any.buying_power, available: "0" } }
      const html = render(<StrategyTicket legs={spread} onLegs={() => {}} expiries={[expiry]} underlying="SPX" spot={7000} trading={trading} onClose={() => {}} />, custom)
      expect(html).toContain("Simulated order preview")
      expect(html).toMatch(/aria-label="Submit strategy order">/)
      expect(html.includes("the server preview includes it")).toBe("slippage_ticks" in optional)
    }
  })
  it("labels net prices as estimates under latency and simulated size impact", () => {
    // Legs without a spread: the default limit, the mid, is marketable, so plain rules promise a fill now.
    const crossing = [put(6900, "sell", 5, 5), put(6890, "buy", 4, 4)]
    const ticket = <StrategyTicket legs={crossing} onLegs={() => {}} expiries={[expiry]} underlying="SPX" spot={7000} trading={trading} onClose={() => {}} />
    expect(render(ticket, any)).toContain("Marketable: fills now at $1.00 credit per unit")
    for (const optional of [{ fill_latency_ms: 1000 }, { impact_ticks: 1 }, { fill_latency_ms: 1000, impact_ticks: 1 }]) {
      const html = render(ticket, { ...any, rules: { ...any.rules, ...optional } })
      expect(html).toContain("Displayed net prices are estimates")
      expect(html).toContain("each leg’s size impact")
      expect(html).toContain("all legs still fill together within the net limit")
      expect(html).toContain("cannot predict the later quote")
      expect(html).not.toContain("fills now")
    }
  })
  it("prices the legs net, shows the expiry risk and places one order", () => {
    const html = render(<StrategyTicket legs={spread} onLegs={() => {}} expiries={[expiry]} underlying="SPX" spot={7000} trading={trading} onClose={() => {}} />, any)
    for (const text of ["Strategy ticket", "SPX <span", "Bull put spread", "2 of 4 legs", "SELL", "BUY", "6900 P", "6890 P",
      "Net bid", "$1.20 credit", "$1.00 credit", "$0.80 credit", 'value="1.00"', "$0.05 tick", "receive at least",
      "Rests: the legs trade now at $0.80 credit; fills when that reaches $1.00 credit.", "Max profit", "$100.00", "Max loss", "$900.00",
      "6899.00", "$100.00 received", "Estimated fees", "$1.30", "Simulated order preview", "Bull put spread · 1 × $1.00 credit",
      // Above the 6899 breakeven by the Oct 16 expiry: forward 7,010, 23 days, 20% volatility.
      "Probability of profit", "≈ 62%", "1σ move by Oct 16", "±351.94"])
      expect(html).toContain(text)
    expect(html).toMatch(/aria-label="Submit strategy order">/)
    expect(html).not.toContain("NaN")
  })
  it("asks for a second leg and blocks buy-only plans", () => {
    expect(render(<StrategyTicket legs={[spread[0]!]} onLegs={() => {}} expiries={[expiry]} underlying="SPX" spot={7000} trading={trading} onClose={() => {}} />, any))
      .toContain("Click another bid or ask on the chain to add a leg")
    const buyOnly = render(<StrategyTicket legs={spread} onLegs={() => {}} expiries={[expiry]} underlying="SPX" spot={7000} trading={trading} onClose={() => {}} />, account)
    expect(buyOnly).toContain("is buy-only. Multi-leg orders may only close held positions.")
    expect(buyOnly).toMatch(/aria-label="Submit strategy order" disabled=""/)
  })
  it("formats nets and lists multi-leg orders with their legs", () => {
    expect(netText(1.2)).toBe("$1.20 debit")
    expect(netText(-0.8)).toBe("$0.80 credit")
    expect(netText(0)).toBe("even")
    expect(netLabel("-0.80")).toBe("$0.80 cr")
    expect(netLabel("1.20")).toBe("$1.20 db")
    const combo: Order = { ...order, id: "9", symbol: null, side: null, status: "working", limit_price: "-1.00", average_fill_price: null, filled_quantity: 0, remaining_quantity: 2, quantity: 2,
      legs: [{ symbol: "SPXW  261016P06900000", side: "sell", ratio: 1 }, { symbol: "SPXW  261016P06890000", side: "buy", ratio: 1 }] }
    const html = render(<OrdersView />, any, [combo])
    for (const text of ["SPX Oct 16 −6900P +6890P", "2 legs", "NET", "$1.00 cr"]) expect(html).toContain(text)
  })
  it("estimates a calendar at its first expiry", () => {
    const later = { ...expiry, id: "2026-10-23PM", expiry: "2026-10-23", expiry_time: "2026-10-23T20:00:00Z", days: 30 }
    const far: StrategyLeg = { ...put(6900, "buy", 7, 7.4), symbol: "SPXW  261023P06900000", expiry: later.id,
      quote: { ...quote, symbol: "SPXW  261023P06900000", bid: 7, ask: 7.4, mid: 7.2, iv: 0.2 } }
    const html = render(<StrategyTicket legs={[spread[0]!, far]} onLegs={() => {}} expiries={[later, expiry]} underlying="SPX" spot={7000} trading={trading} onClose={() => {}} />, any)
    for (const text of ["Calendar spread", "Oct 16 / Oct 23", "· Oct 23", "≈ $", "Estimated P&amp;L at the Oct 16 expiry", "later legs at today&#x27;s implied volatility"])
      expect(html).toContain(text)
    expect(html).not.toContain("NaN")
  })
  it("shows what a closing order closes instead of its payoff", () => {
    const close = spread.map((l) => ({ ...l, side: l.side === "buy" ? "sell" as const : "buy" as const }))
    const html = render(<StrategyTicket closing title="Close together" legs={close} onLegs={() => {}} expiries={[expiry]} underlying="SPX" spot={7000}
      trading={trading} onClose={() => {}} units={3} variant="dialog" />, any, [], spread.map((leg) => ({
        ...portfolio.positions[0]!, symbol: leg.symbol, quantity: leg.side === "buy" ? 3 : -3 })))
    for (const text of ["Close together", "SPX <span", ">Close</span>", "Closes", "6 contracts, all legs together", 'value="3"']) expect(html).toContain(text)
    expect(html).not.toContain("Max profit")
    expect(html).not.toContain("Profit and loss at expiry")
  })
})
