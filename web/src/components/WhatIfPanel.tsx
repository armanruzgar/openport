import { useActionApi } from "../api/action-client"
import { useState } from "react"

import type { NewOrder, Position, TradingStatus, WhatIfAccount, WhatIfResponse } from "../api/trading-types"
import { legsLabel, osiLabel } from "../lib/journal"
import { formatMoney } from "../lib/trading"
import { addWhatIfCandidate, closingOrders, maxCandidates, updateWhatIf, useWhatIf, useWhatIfScope, whatIfNameError, type WhatIfDraft } from "../lib/what-if"
import { useWrite } from "./OrderActions"
import { TradingError, WriteAccess } from "./TradingControls"

/** "Buy 2 SPX Oct 22 5000C at market", or a multi-leg order's units and legs. */
export function whatIfOrderText(order: NewOrder) {
  const price = order.type === "limit" ? ` at ${formatMoney(order.limit_price)}` : " at market"
  return order.legs ? `${order.quantity} × ${legsLabel(order.legs, "")}${price}`
    : `${order.side === "buy" ? "Buy" : "Sell"} ${order.quantity} ${osiLabel(order.symbol, "")}${price}`
}
function signed(value: number | null | undefined) {
  return value == null ? "—" : `${value > 0 ? "+" : ""}${value.toFixed(0)}`
}
function change(after: number | null | undefined, now: number | null | undefined) {
  return after == null || now == null ? null : after - now
}
/** The grid's row at unchanged volatility (or its middle column), by spot move. */
function spotRow(account: WhatIfAccount) {
  const { vol_points: vols, pnl } = account.scenarios
  const column = vols.indexOf(0) >= 0 ? vols.indexOf(0) : Math.floor(vols.length / 2)
  return pnl.map((row) => row[column] ?? null)
}
function Results({ result }: { result: WhatIfResponse }) {
  const now = result.current
  const rows: { name: string; account: WhatIfAccount | null; checks: string; tone?: string }[] = [
    { name: "Now", account: now, checks: "" },
    ...result.candidates.map((c) => ({
      name: c.name, account: c.after,
      checks: c.decision === "ok" ? "Accepted" : `${c.decision}: ${c.reason?.message ?? ""}`,
      tone: c.decision === "ok" ? undefined : "text-warn",
    })),
  ]
  const spots = now.scenarios.spot_percent
  return <div className="space-y-3">
    <div className="overflow-x-auto">
      <table className="w-full min-w-[42rem] text-xs tabular">
        <thead className="text-left text-muted"><tr>
          <th className="py-1 pr-2 font-normal">Candidate</th><th className="pr-2 font-normal">Checks</th>
          <th className="pr-2 text-right font-normal">Equity</th><th className="pr-2 text-right font-normal">Buying power</th>
          <th className="pr-2 text-right font-normal">Grid max loss</th><th className="pr-2 text-right font-normal">Equity at max loss</th>
          <th className="pr-2 text-right font-normal">Room</th>
          <th className="pr-2 text-right font-normal">Δ dollar delta</th><th className="pr-2 text-right font-normal">Δ vega</th>
          <th className="text-right font-normal">Δ theta</th>
        </tr></thead>
        <tbody>{rows.map(({ name, account, checks, tone }, index) => <tr key={index} className="border-t border-border">
          <td className="py-1 pr-2 font-medium">{name}</td>
          <td className={`pr-2 ${tone ?? "text-muted"}`}>{checks}</td>
          {account ? <>
            <td className="pr-2 text-right">{formatMoney(account.equity)}</td>
            <td className="pr-2 text-right">{formatMoney(account.buying_power)}</td>
            <td className="pr-2 text-right">{formatMoney(account.max_loss)}</td>
            <td className={`pr-2 text-right ${account.breaches_floor || account.breaches_soft_floor ? "font-medium text-danger" : ""}`}>
              {formatMoney(account.equity_at_max_loss)}{account.breaches_floor ? " · floor" : account.breaches_soft_floor ? " · soft floor" : ""}</td>
            <td className="pr-2 text-right">{formatMoney(account.breach.room ?? account.breach.soft_room)}</td>
            {index === 0 ? <td colSpan={3} className="text-right text-muted">
              Δ {signed(now.exposure?.dollar_delta)} · vega {signed(now.exposure?.vega)} · theta {signed(now.exposure?.theta)}</td> : <>
              <td className="pr-2 text-right">{signed(change(account.exposure?.dollar_delta, now.exposure?.dollar_delta))}</td>
              <td className="pr-2 text-right">{signed(change(account.exposure?.vega, now.exposure?.vega))}</td>
              <td className="text-right">{signed(change(account.exposure?.theta, now.exposure?.theta))}</td>
            </>}
          </> : <td colSpan={8} className="text-muted">Cannot be projected without a contract and a quote for each order.</td>}
        </tr>)}</tbody>
      </table>
    </div>
    {now.scenarios.complete && spots.length > 0 && <div className="overflow-x-auto">
      <p className="text-muted">P&amp;L from today's equity by spot move, volatility unchanged</p>
      <table className="mt-1 w-full text-xs tabular">
        <thead className="text-muted"><tr><th className="py-1 pr-2 text-left font-normal">Candidate</th>
          {spots.map((spot) => <th key={spot} className="pr-2 text-right font-normal">{spot > 0 ? "+" : ""}{spot}%</th>)}</tr></thead>
        <tbody>{rows.map(({ name, account }, index) => account?.scenarios.complete && <tr key={index} className="border-t border-border">
          <td className="py-1 pr-2">{name}</td>
          {spotRow(account).map((pnl, i) => <td key={i} className={`pr-2 text-right ${pnl != null && pnl < 0 ? "text-bearish" : ""}`}>{signed(pnl)}</td>)}
        </tr>)}</tbody>
      </table>
    </div>}
    <p className="text-faint">Each candidate's orders are checked as submission checks them and filled in full at the preview's prices (slipped far sides, or the limit), one after another, on a private copy of the account. Max loss is the worst cell of the account's spot × volatility grid from today's equity, so a candidate's cost to trade counts. Simulated estimates, not execution promises.</p>
  </div>
}

/**
 * What-if: candidate adjustments, each one or more orders, compared side by side
 * against the held book before any is sent. Orders come from a ticket's preview
 * (Add to what-if) or from closing picked positions.
 */
export function WhatIfPanel({ positions, picked = [], trading }: { positions: readonly Position[]; picked?: readonly Position[]; trading: TradingStatus }) {
  const api = useActionApi()
  const scope = useWhatIfScope()
  const state = useWhatIf(scope)
  const write = useWrite(trading)
  const [result, setResult] = useState<WhatIfResponse | null>(null)
  const candidates = state.candidates
  const nameErrors = candidates.map((c) => whatIfNameError(c.name))
  const update = (change: (candidates: WhatIfDraft[]) => WhatIfDraft[]) => {
    updateWhatIf(scope, (s) => ({ ...s, candidates: change(s.candidates) }))
    setResult(null)
  }
  const closing = picked.length ? picked : positions
  return <section aria-label="What-if" className="space-y-3 text-sm">
    <div className="flex flex-wrap items-center gap-2">
      <button type="button" className="trade-button" disabled={!closing.length || closing.length > 4 || candidates.length >= maxCandidates}
        onClick={() => { addWhatIfCandidate(scope, picked.length ? "Close picked" : "Close all", closingOrders(closing)); setResult(null) }}>
        {picked.length ? `Add closing the ${picked.length} picked` : "Add closing every position"}</button>
      {candidates.length > 1 && <label className="flex items-center gap-2 text-xs">
        <span className="text-muted">Add orders from tickets to</span>
        <select className="trade-input !w-auto !py-1" value={state.target ?? ""} onChange={(e) => updateWhatIf(scope, (s) => ({ ...s, target: e.target.value || null }))}>
          <option value="">A new candidate</option>
          {candidates.map((c) => <option key={c.id} value={c.id}>{c.name}</option>)}
        </select>
      </label>}
      {candidates.length === 1 && <label className="flex items-center gap-2 text-xs">
        <input type="checkbox" checked={state.target === candidates[0]!.id}
          onChange={(e) => updateWhatIf(scope, (s) => ({ ...s, target: e.target.checked ? candidates[0]!.id : null }))} />
        Add orders from tickets to {candidates[0]!.name}
      </label>}
    </div>
    {!candidates.length && <p className="text-xs text-muted">Build each candidate from a ticket's preview with Add to what-if, or close positions here; a candidate can hold up to four orders, such as closing the tested side and opening a new one.</p>}
    <ol className="space-y-2">{candidates.map((c, index) => <li key={c.id} className="rounded-md border border-border p-2">
      <div className="flex items-center gap-2">
        <input className="trade-input !py-1 text-xs" aria-label="Candidate name" value={c.name}
          aria-invalid={!!nameErrors[index]} aria-describedby={nameErrors[index] ? `what-if-name-${c.id}` : undefined}
          onChange={(e) => update((list) => list.map((x) => x.id === c.id ? { ...x, name: e.target.value } : x))} />
        <button type="button" className="trade-button" aria-label={`Remove ${c.name}`} onClick={() => update((list) => list.filter((x) => x.id !== c.id))}>Remove</button>
      </div>
      {nameErrors[index] && <p id={`what-if-name-${c.id}`} className="mt-1 text-xs text-warn">{nameErrors[index]}</p>}
      <ul className="mt-1 space-y-0.5 text-xs">{c.orders.map((order, index) => <li key={index} className="flex justify-between gap-2">
        <span>{whatIfOrderText(order)}</span>
        <button type="button" className="text-muted hover:text-foreground" aria-label={`Remove order ${index + 1} from ${c.name}`}
          onClick={() => update((list) => list.map((x) => x.id === c.id ? { ...x, orders: x.orders.filter((_, i) => i !== index) } : x))}>×</button>
      </li>)}</ul>
    </li>)}</ol>
    <WriteAccess trading={trading} />
    <TradingError error={write.error} />
    <button type="button" className="trade-button" disabled={!candidates.length || nameErrors.some(Boolean) || write.pending || write.blocked}
      onClick={() => void write.run(() => api.whatIf(candidates.map((c) => ({ name: c.name.trim(), orders: c.orders })), trading.write), setResult)}>
      {write.pending ? "Comparing…" : `Compare ${candidates.length || ""} ${candidates.length === 1 ? "candidate" : "candidates"}`.replace("  ", " ")}</button>
    {result && <Results result={result} />}
  </section>
}
