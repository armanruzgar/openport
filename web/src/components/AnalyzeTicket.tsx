import { useQueries } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { TradingStatus } from "../api/trading-types"
import type { Chain } from "../api/types"
import { matchingPayload } from "../lib/payload"
import type { StrategyLeg } from "../lib/strategy"
import { CoveredStrategyDialog } from "./CoveredStrategyDialog"
import { Dialog } from "./Dialog"
import { OrderTicket } from "./OrderTicket"
import { StrategyTicket } from "./StrategyTicket"

/** Each expiry's whole chain, by expiry ID, with the latest quotes. */
export function useExpiryChains(symbol: string, ids: readonly string[]) {
  const { version } = useLive()
  const unique = [...new Set(ids)]
  const results = useQueries({ queries: unique.map((id) => ({
    queryKey: ["chain", symbol, id, 0, version(symbol)],
    queryFn: ({ signal }: { signal: AbortSignal }) => api.chain(symbol, id, 0, signal),
    placeholderData: (previous: Chain | undefined) => matchingPayload(previous, symbol, id),
  })) })
  const chains = new Map<string, Chain>()
  unique.forEach((id, i) => { const chain = matchingPayload(results[i]?.data, symbol, id); if (chain) chains.set(id, chain) })
  return { chains, loading: unique.some((id, i) => !chains.has(id) && !results[i]?.isError), failed: results.some((r) => r.isError) }
}

/** Legs with their chain's latest quote. */
export function quotedLegs(legs: readonly StrategyLeg[], chains: ReadonlyMap<string, Chain>): StrategyLeg[] {
  return legs.map((leg) => ({ ...leg, quote: chains.get(leg.expiry)?.strikes.find((row) => row.strike === leg.strike)?.[leg.type] ?? leg.quote }))
}

/**
 * Sends a strategy to the ticket that trades it: a single option to the order
 * ticket, two to four legs to the strategy ticket, and a covered call to the
 * shares-then-call dialog. Nothing is sent until the ticket's own review.
 */
export function AnalyzeTicket({ symbol, legs, units, shares, trading, onClose }: {
  symbol: string; legs: readonly StrategyLeg[]; units: number; shares: number; trading: TradingStatus; onClose: () => void
}) {
  const { chains, loading, failed } = useExpiryChains(symbol, legs.map((leg) => leg.expiry))
  const [edited, setEdited] = useState<StrategyLeg[] | null>(null)
  if (loading) return <Dialog title="Strategy order" onClose={onClose}><p className="text-sm text-muted">Loading the legs' quotes…</p></Dialog>
  if (failed || !legs.length) return <Dialog title="Strategy order" onClose={onClose}><p className="text-sm text-warn">The legs' chains could not be loaded.</p></Dialog>
  const live = quotedLegs(edited ?? legs, chains)
  const first = live[0]!
  const chain = chains.get(first.expiry)!
  if (shares > 0 && live.length === 1 && first.type === "call" && first.side === "sell")
    return <CoveredStrategyDialog kind="covered-call" chain={chain} trading={trading} onClose={onClose} initialCall={first.symbol} initialQuantity={units} />
  if (live.length === 1) {
    const cell = first.side === "buy" ? "ask" : "bid"
    const price = first.quote?.[cell]
    return <OrderTicket trading={trading} quote={first.quote} onClose={onClose} smile={chain.strikes} selection={{
      symbol: first.symbol, underlying: symbol, expiry: chain.expiry, strike: first.strike, optionType: first.type, cell,
      price: price != null ? String(price) : "", spot: chain.spot, quantity: units * first.ratio,
    }} />
  }
  const smiles = new Map([...chains].map(([id, c]) => [id, c.strikes]))
  return <StrategyTicket legs={live} onLegs={(next) => { if (!next.length) onClose(); else setEdited(next) }}
    expiries={[...chains.values()].map((c) => c.expiry)} underlying={symbol} spot={chain.spot} trading={trading}
    variant="dialog" units={units} smiles={smiles} onClose={onClose} />
}
