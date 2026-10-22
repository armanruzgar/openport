import { useQuery } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { sameAccountPlaceholder } from "../api/trading"
import type { Hedge, HedgeTarget, NewOrder, Risk, TradingStatus } from "../api/trading-types"
import { fixed, pct } from "../lib/format"
import { addWhatIfCandidate, maxCandidates, useWhatIf, useWhatIfScope } from "../lib/what-if"
import { AnalyzeTicket } from "./AnalyzeTicket"
import { TradeSharesDialog } from "./StockActions"
import { TradingError } from "./TradingControls"
import { Segmented } from "./ui"

const dollars = (x: number | null | undefined) => x == null ? "—" : `${x < 0 ? "−" : ""}$${Math.abs(x).toLocaleString("en-US", { maximumFractionDigits: 0 })}`
const targets: { value: HedgeTarget; label: string }[] = [{ value: "delta", label: "Delta" }, { value: "vega", label: "Vega" }, { value: "grid", label: "Worst grid cell" }]

/** "Sell 3 SPX ATM call", "Buy 1,500 SPY shares" or "No trade needed". */
export function hedgeText(hedge: Hedge): string {
  if (!hedge.feasible || hedge.quantity == null) return hedge.reason ?? "Unavailable"
  if (!hedge.side || hedge.quantity === 0) return hedge.reason ?? "No trade needed"
  const what = hedge.kind === "shares" ? `${hedge.underlying} shares` : `${hedge.underlying} ${fixed(hedge.strike, hedge.strike != null && hedge.strike % 1 ? 2 : 0)} ${hedge.type} (${hedge.expiry})`
  return `${hedge.side === "buy" ? "Buy" : "Sell"} ${hedge.quantity.toLocaleString("en-US")} ${what}`
}
/** An option hedge as the what-if takes it. */
export function hedgeOrder(hedge: Hedge): NewOrder | null {
  return hedge.order ? { client_order_id: "", symbol: hedge.order.symbol, side: hedge.order.side, type: "limit", time_in_force: "day",
    quantity: hedge.order.quantity, limit_price: hedge.order.limit_price } as NewOrder : null
}

/**
 * Hedge suggestions for one risk bucket: the smallest share, ATM option or SPX/SPY
 * beta hedge that brings its delta, vega or worst scenario-grid cell back inside
 * the limit (or above the floor), ranked by spread cost and then residual risk.
 * An option hedge goes to the what-if to compare, then to its ticket.
 */
export function HedgePanel({ risk, trading }: { risk: Risk; trading: TradingStatus }) {
  const { accountScope } = useLive()
  const whatIf = useWhatIfScope()
  const candidates = useWhatIf(whatIf).candidates
  const scopes = ["aggregate", ...Object.keys(risk.underlyings)]
  const [chosen, setScope] = useState("aggregate")
  const scope = scopes.includes(chosen) ? chosen : "aggregate"
  const [target, setTarget] = useState<HedgeTarget>("delta")
  const [utilisation, setUtilisation] = useState(1)
  const [added, setAdded] = useState<string | null>(null)
  const [ticket, setTicket] = useState<Hedge | null>(null)
  const hedges = useQuery({
    queryKey: ["trading", accountScope, "hedges", trading.account_version, scope, target, utilisation],
    queryFn: ({ signal }) => api.hedges(target, scope, utilisation, signal),
    enabled: trading.enabled, retry: false,
    placeholderData: (previous, prior) => sameAccountPlaceholder(previous, prior?.queryKey, accountScope),
  })
  const data = hedges.data
  const current = data?.current
  return <section aria-label="Hedge suggestions" className="space-y-3 text-sm">
    <div className="flex flex-wrap items-end gap-3 text-xs">
      <label className="flex flex-col gap-1 text-muted">Bucket
        <select aria-label="Hedge bucket" className="trade-input" value={scope} onChange={(e) => setScope(e.target.value)}>
          {scopes.map((s) => <option key={s} value={s}>{s === "aggregate" ? "Aggregate (SPY/SPX beta hedges)" : s}</option>)}</select></label>
      <div className="flex flex-col gap-1 text-muted">Bring back<Segmented label="Hedge target" value={target} onChange={setTarget} options={targets} /></div>
      <label className="flex flex-col gap-1 text-muted">To this share of the limit
        <select aria-label="Target share of the limit" className="trade-input" value={utilisation} onChange={(e) => setUtilisation(Number(e.target.value))}>
          {[1, 0.9, 0.75, 0.5].map((u) => <option key={u} value={u}>{pct(u, 0)}</option>)}</select></label>
    </div>
    <TradingError error={hedges.error} />
    {current && <p className="text-xs tabular text-muted">
      {target === "delta" ? `Dollar delta ${dollars(current.dollar_delta_low)} to ${dollars(current.dollar_delta_high)} against ±${dollars(current.delta_limit)}`
        : target === "vega" ? `Vega ${fixed(current.vega_low, 0)} to ${fixed(current.vega_high, 0)} against ±${fixed(current.vega_limit, 0)}`
        : `Worst grid cell ${dollars(current.worst_cell)} against ${dollars(current.grid_floor)}`}
      {target === "delta" && current.beta_weighted_dollar_delta_low != null && current.beta_weighted_dollar_delta_high != null
        ? ` · beta-weighted to SPY ${dollars(current.beta_weighted_dollar_delta_low)} to ${dollars(current.beta_weighted_dollar_delta_high)}`
        : current.beta_weighted_dollar_delta != null ? ` · beta-weighted to SPY ${dollars(current.beta_weighted_dollar_delta)}` : ""}</p>}
    {data?.beta_note && <p role="note" className="text-xs text-warn">{data.beta_note}</p>}
    {data && <div className="relative max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Hedges">
      <table className="w-full text-right text-xs tabular whitespace-nowrap">
        <thead className="text-muted"><tr>
          <th className="py-1 pr-2 text-left font-normal">Hedge</th><th className="pr-2 text-left font-normal">Trade</th>
          <th className="pr-2 font-normal">Premium</th><th className="pr-2 font-normal">Spread cost</th>
          <th className="pr-2 font-normal">Delta after</th><th className="pr-2 font-normal">Vega after</th><th className="pr-2 font-normal">Worst cell after</th>
          <th className="font-normal"><span className="sr-only">Actions</span></th>
        </tr></thead>
        <tbody>{data.hedges.map((hedge) => {
          const order = hedgeOrder(hedge)
          const residual = hedge.residual
          const trade = hedge.feasible && hedge.side != null && (hedge.quantity ?? 0) > 0
          return <tr key={hedge.name} className={`border-t border-border/40 ${hedge.feasible ? "" : "text-muted"}`}>
            <td className="py-1 pr-2 text-left">{hedge.name}</td>
            <td className="pr-2 text-left">{hedgeText(hedge)}{hedge.exceeds_order_limit ? " · above the order size limit; split it" : ""}
              {hedge.beta_hedge && hedge.sizing === "beta_weighted_dollar_delta" ? ` · sized on beta-weighted delta (β ${fixed(hedge.instrument_beta, 2)})` : ""}
              {hedge.beta_hedge && hedge.sizing === "dollar_delta" ? " · sized one for one: a beta is missing" : ""}</td>
            <td className="pr-2">{dollars(hedge.premium)}</td><td className="pr-2">{dollars(hedge.spread_cost)}</td>
            <td className="pr-2">{residual ? `${dollars(residual.dollar_delta_low)}…${dollars(residual.dollar_delta_high)} · ${pct(residual.delta_utilisation, 0)}` : "—"}
              {residual && hedge.sizing === "beta_weighted_dollar_delta" && residual.beta_weighted_dollar_delta_low != null
                ? <span className="block text-muted">β-weighted {dollars(residual.beta_weighted_dollar_delta_low)}…{dollars(residual.beta_weighted_dollar_delta_high)} · {pct(residual.beta_weighted_delta_utilisation, 0)}</span> : null}</td>
            <td className="pr-2">{residual ? `${fixed(residual.vega_high, 0)} · ${pct(residual.vega_utilisation, 0)}` : "—"}</td>
            <td className="pr-2">{residual ? dollars(residual.worst_cell) : "—"}</td>
            <td className="space-x-1">{trade && <>
              {order && <button type="button" className="trade-button" disabled={candidates.length >= maxCandidates}
                onClick={() => { if (addWhatIfCandidate(whatIf, `Hedge: ${hedge.name}`.slice(0, 64), [order])) setAdded(hedge.name) }}>Add to what-if</button>}
              <button type="button" className="trade-button" onClick={() => setTicket(hedge)}>Ticket</button>
            </>}</td>
          </tr>
        })}</tbody>
      </table>
    </div>}
    {added && <p role="status" className="text-xs text-muted">Added “Hedge: {added}” to the what-if below; compare it there, then open its ticket.</p>}
    {data && <p className="text-[11px] text-muted">{data.model}</p>}
    {ticket?.order && ticket.expiry && ticket.strike != null && ticket.type && <AnalyzeTicket symbol={ticket.underlying} trading={trading} units={ticket.order.quantity} shares={0}
      legs={[{ symbol: ticket.order.symbol, underlying: ticket.underlying, side: ticket.order.side, ratio: 1, type: ticket.type, strike: ticket.strike, expiry: ticket.expiry, quote: null }]}
      onClose={() => setTicket(null)} />}
    {ticket?.stock_trade && <TradeSharesDialog initial={ticket.stock_trade.symbol} initialSide={ticket.stock_trade.side}
      initialShares={ticket.stock_trade.shares} trading={trading} onClose={() => setTicket(null)} />}
  </section>
}
