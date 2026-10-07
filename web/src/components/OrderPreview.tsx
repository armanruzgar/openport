import { useActionApi } from "../api/action-client"
import { FeeAmount } from "./FeeAmount"
import { useQuery } from "@tanstack/react-query"
import { useEffect, useState } from "react"

import { useLive } from "../api/live"
import type { NewOrder, OrderChange, OrderPreview, PreviewExecution, PreviewFill, TradingStatus } from "../api/trading-types"
import { osiLabel } from "../lib/journal"
import { formatMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"
import { BreachPanel } from "./BreachPanel"

/** A debounced preview of `body`, refetched when the account changes. */
function usePreview<T>(kind: string, body: T | null, trading: TradingStatus, fetch: (body: T) => Promise<OrderPreview>) {
  const { accountScope } = useLive()
  const token = useWriteToken()
  const signature = JSON.stringify(body)
  const [ready, setReady] = useState("")
  useEffect(() => {
    const timer = window.setTimeout(() => setReady(signature), 300)
    return () => window.clearTimeout(timer)
  }, [signature])
  const query = useQuery({
    queryKey: ["trading", accountScope, kind, trading.account_version, ready, trading.write, token],
    queryFn: () => fetch(JSON.parse(ready) as T),
    enabled: body != null && signature === ready && trading.enabled,
    retry: false,
  })
  return { data: signature === ready && body ? query.data : undefined, error: signature === ready ? query.error : null,
    loading: body != null && (signature !== ready || query.isFetching) }
}
export function useOrderPreview(order: NewOrder | null, trading: TradingStatus) {
  const api = useActionApi()
  return usePreview("preview", order, trading, (body) => api.previewOrder(body, trading.write))
}
/** The preview of a change to resting order `id`, as saving it would make it; null previews nothing. */
export function useChangePreview(id: string, change: OrderChange | null, trading: TradingStatus) {
  const api = useActionApi()
  return usePreview(`preview-change-${id}`, change, trading, (body) => api.previewChange(id, body, trading.write))
}
// exposure_change is the order's change to the book's Greeks, not the book after it.
const greekChanges = [["dollar_delta", "Dollar delta"], ["dollar_gamma_1pct", "Dollar gamma per 1%"], ["vega", "Vega"], ["theta", "Theta"]] as const
function signed(value: number) { return `${value > 0 ? "+" : ""}${value.toFixed(2)}` }
function units(value: number | null | undefined) { return value == null ? "Unavailable" : `${value} ${value === 1 ? "unit" : "units"}` }
function fillRow(fill: PreviewFill) {
  return `${fill.side === "buy" ? "Buy" : "Sell"} ${fill.quantity} ${osiLabel(fill.symbol, "")} at ${formatMoney(fill.price)}`
}
/** What submitting now would fill at once, what happens to the rest, and the full size block by block. */
export function ExecutionSummary({ execution: e }: { execution: PreviewExecution }) {
  if (e.status === "rejected") return null
  const rest = e.remaining_quantity
  const fills = e.filled_quantity > 0
    ? `Fills ${e.filled_quantity} at once${e.average_fill_price != null ? ` at ${formatMoney(e.average_fill_price)} on average` : ""}.`
    : "Nothing fills at once."
  const after = rest <= 0 ? "" : e.status === "cancelled" ? ` ${rest} would cancel: ${e.reason?.message ?? "no liquidity left"}.`
    : e.status === "armed" ? " It waits armed until its trigger is reached."
    : ` ${rest} ${rest === 1 ? "works" : "work"} as a resting order.`
  return <section aria-label="Expected execution" className="space-y-1">
    <p className={e.status === "cancelled" && rest > 0 ? "text-warn" : undefined}>{fills}{after}</p>
    {e.schedule.length > 0 && <details><summary className="cursor-pointer text-muted">
      Full size at the current quotes{e.average_price != null ? `: ${formatMoney(e.average_price)} per unit` : ""}</summary>
      <ul className="mt-1 space-y-0.5 tabular">{e.schedule.map((fill, index) => <li key={index}>{fillRow(fill)}</li>)}</ul>
      <p className="mt-1 text-faint">Far sides with the account's slippage, each further displayed-size block at its impact price; a limit order pays its limit instead.</p>
    </details>}
  </section>
}
/** `what` names what is previewed: a new order, or a change to a resting one, whose sizes count the units still to work. */
export function OrderPreviewPanel({ preview, onSize, disabled = false, what = "order", onWhatIf, sizing = true }: {
  preview: { data?: OrderPreview; error: unknown; loading: boolean }; onSize: (size: number) => void; disabled?: boolean; what?: "order" | "change"; sizing?: boolean
  /** Adds the order to Positions' what-if comparison; false when there is no room. */
  onWhatIf?: () => boolean
}) {
  const p = preview.data
  const [added, setAdded] = useState<string | null>(null)
  // Without a plan or soft floor, size follows buying power and the limits alone.
  const floorless = p != null && p.max_units_floor == null && p.breach.room == null && p.breach.soft_room == null
  const basis = p?.max_units_basis ?? (p?.max_units_floor != null && p.max_units_buying_power != null &&
    p.max_units_floor < p.max_units_buying_power ? "floor" : null)
  const sizeLabel = basis === "floor" && !floorless ? "Size at 50% of floor room"
    : basis === "buying_power" ? "Size within buying power" : basis === "limits" ? "Size within limits" : "Suggested size"
  const sizeButton = basis === "floor" && !floorless ? "Size to floor"
    : basis === "buying_power" ? "Size to buying power" : basis === "limits" ? "Size to limits" : "Use suggested size"
  return <section aria-label="Order preview" className="space-y-2 rounded-md border border-border p-3 text-xs">
    <div className="flex flex-wrap items-center justify-between gap-2"><span className="font-medium">Simulated order preview</span>
      {onWhatIf && <button type="button" className="trade-button ml-auto" disabled={!p}
        onClick={() => setAdded(onWhatIf() ? "Added to What-if on Positions." : "What-if is full: six candidates of up to four orders.")}>Add to what-if</button>}
      {sizing && <button type="button" className="trade-button" disabled={disabled || preview.loading || !p || p.max_units == null || p.max_units < 1 || !!preview.error}
        onClick={() => { if (p?.max_units) onSize(p.max_units) }}>{sizeButton}</button>}</div>
    {added && <p role="status" className="text-muted">{added}</p>}
    {preview.loading ? <p className="text-muted">Checking order…</p>
      : preview.error ? <p role="status" className="text-warn">Preview failed. Buying power and floor risk are unavailable. You can still submit for the server's checks.</p>
      : !p ? <p className="text-muted">{what === "change" ? "Change a term to preview the order on its new terms." : "Complete the order to preview its risk."}</p> : <>
        <dl className="grid grid-cols-2 gap-2 tabular">
          {p.fee !== undefined && <><dt className="text-muted">Fees for remaining size</dt><dd className="text-right"><FeeAmount fee={p.fee} fees={p.fees} /></dd></>}
          <dt className="text-muted">Buying power after</dt><dd className="text-right">{formatMoney(p.buying_power.after)}</dd>
          <dt className="text-muted">{p.max_loss_basis === "scenario_grid" ? "Scenario-grid loss" : "Max loss at expiry"}</dt><dd className="text-right">{formatMoney(p.max_loss)}</dd>
          {p.trade_risk_basis && <>
            <dt className="text-muted">Trade risk before fees</dt><dd className="text-right">{p.trade_risk == null ? "Unbounded or unknown" : formatMoney(p.trade_risk)}</dd>
            <dt className="text-muted">Plan trade-risk limit</dt><dd className="text-right">{p.trade_risk_limit == null ? "No applicable cap" : formatMoney(p.trade_risk_limit)}</dd>
          </>}
          <dt className="text-muted">Room after this {what}</dt><dd className="text-right">{formatMoney(p.breach.room)}</dd>
          {sizing && <>
            <dt className="text-muted">{sizeLabel}</dt><dd className="text-right">{units(p.max_units)}</dd>
            {p.max_units_buying_power !== undefined && <>
              <dt className="text-muted">Fits buying power and limits</dt><dd className="text-right">{units(p.max_units_buying_power)}</dd>
            </>}
            {!floorless && p.max_units_floor !== undefined && <>
              <dt className="text-muted">Fits 50% of floor room</dt><dd className="text-right">{units(p.max_units_floor)}</dd>
            </>}
          </>}
        </dl>
        {p.decision !== "ok" && <p role="status" className="text-warn">{p.decision}: {p.reason?.message}</p>}
        {p.decision === "ok" && p.execution && <ExecutionSummary execution={p.execution} />}
        {p.next_walk && <p>Next walk: {formatMoney(p.next_walk.limit_price)} at {p.next_walk.time} (market time)</p>}
        {p.warnings?.map((warning) => <p key={warning.code} role="status" className="text-warn">Check: {warning.message}</p>)}
        {(p.breaches_floor || p.breaches_soft_floor) && <p role="alert" className="font-medium text-danger">This {what} could breach {p.breaches_floor ? "the plan floor" : "your soft floor"}.</p>}
        <details><summary className="cursor-pointer text-muted">After this {what} · breach risk, and the change in Greeks</summary>
          <BreachPanel breach={p.breach} />
          {p.exposure_change && <section aria-label="Change in Greeks" className="mt-2">
            <p className="text-muted">{what === "change" ? "Change once the rest fills on the new terms" : "Change from this order"}</p>
            <dl className="mt-1 grid grid-cols-2 gap-1 tabular">{greekChanges.map(([key, label]) =>
              <div key={key}><dt className="text-muted">{label}</dt><dd>{signed(p.exposure_change![key])}</dd></div>)}</dl>
          </section>}
        </details>
        <p className="text-faint">Full-size projection including fees. {sizing && <>{floorless ? "Size uses buying power and limits: the account has no plan or soft floor." : "Size uses buying power, limits and 50% of the nearer floor's room."} Sizing is unavailable when the order is refused at any size or its loss cannot be projected. </>} Scenario losses are model estimates.</p>
      </>}
  </section>
}
