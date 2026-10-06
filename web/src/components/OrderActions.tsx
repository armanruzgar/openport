import { useQuery } from "@tanstack/react-query"
import { useRef, useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { useRefreshTrading, useTradingSession } from "../api/trading"
import type { ClosePositionsResponse, FlattenPricing, FlattenPreview, FlattenResidual, Order, Position, StockHolding, TradingStatus } from "../api/trading-types"
import { contractLabel, orderLabel, osiLabel } from "../lib/journal"
import { closingAction, editableFields, flattenPlan, isOpen, openOrdersIn, orderChange, orderDraft, outcome, underlyingsOf } from "../lib/orders"
import { describeTrigger } from "../lib/ticket"
import { extendedSession, formatMoney, paperNotice } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { OrderPreviewPanel, useChangePreview } from "./OrderPreview"
import { TradingError, WriteAccess, writeBlocked } from "./TradingControls"
import { Badge } from "./ui"
import { useWalk } from "./WalkFields"

/** Runs one write at a time, keeping its error and pending state for this dialog. */
export function useWrite(trading: TradingStatus) {
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<unknown>()
  const busy = useRef(false)
  const blocked = writeBlocked(trading, token)
  async function run<T>(request: () => Promise<T>, done: (result: T) => void) {
    if (busy.current || blocked) return
    busy.current = true
    setPending(true)
    setError(undefined)
    try {
      const result = await request()
      if (sameSession()) done(result)
    } catch (failure) {
      if (sameSession()) setError(failure)
    } finally {
      busy.current = false
      if (sameSession()) setPending(false)
      void refresh()
    }
  }
  return { run, pending, error, blocked }
}

function ScopePicker({ value, options, onChange, what }: { value: string | null; options: string[]; onChange: (value: string | null) => void; what: string }) {
  if (options.length < 2) return null
  return (
    <label className="flex items-center gap-2 text-sm">
      <span className="text-muted">{what}</span>
      <select className="trade-input !w-auto !py-1" value={value ?? ""} onChange={(e) => onChange(e.target.value || null)}>
        <option value="">Every underlying</option>
        {options.map((symbol) => <option key={symbol} value={symbol}>{symbol} only</option>)}
      </select>
    </label>
  )
}

/**
 * Change a resting order in place: its size, limit price or trigger level. The
 * server checks the new terms like a new order and fills a limit that now
 * crosses the market.
 */
export function EditOrderDialog({ order, trading, onClose, onDone }: {
  order: Order; trading: TradingStatus; onClose: () => void; onDone: (order: Order) => void
}) {
  const write = useWrite(trading)
  const [draft, setDraft] = useState(() => orderDraft(order))
  const fields = editableFields(order)
  const walk = useWalk(order.type === "limit" && !order.trigger && !order.role && !order.reduce_only && order.origin !== "system" &&
    ["day", "gtc"].includes(order.time_in_force), draft.limit_price, order.side !== "sell", "0.10", draft.limit_price, order.walk)
  const base = orderChange(order, draft)
  const walkChanged = walk.walk?.step !== order.walk?.step || walk.walk?.seconds !== order.walk?.seconds || walk.walk?.limit !== order.walk?.limit
  const result = !walk.valid ? { error: "Check the walk settings" }
    : walkChanged && !("error" in base) ? { change: { ...("change" in base ? base.change : {}), walk: walk.walk ?? null } } : base
  const preview = useChangePreview(order.id, "change" in result ? result.change : null, trading)
  const side = order.side ?? "buy"
  const priceLabel = order.legs ? "Net limit per unit (negative for a credit)" : "Limit price"
  return (
    <Dialog title={`Edit order #${order.id}`} onClose={onClose}>
      <div className="text-sm">
        <div className="font-medium">{orderLabel(order)}</div>
        <div className="mt-0.5 text-xs text-muted">
          {order.side ? order.side.toUpperCase() : "NET"} · {order.type} · {order.time_in_force.toUpperCase()} · filled {order.filled_quantity} of {order.quantity}
          {order.role && <> · <Badge tone={order.role === "stop_loss" ? "negative" : "positive"}>{order.role === "stop_loss" ? "Stop" : "Target"}</Badge></>}
        </div>
      </div>
      <WriteAccess trading={trading} />
      <form className="space-y-3" onSubmit={(event) => {
        event.preventDefault()
        if ("change" in result) void write.run(() => api.modifyOrder(order.id, result.change, trading.write), (response) => onDone(response.order))
      }}>
        {fields.quantity && (
          <label className="block space-y-1 text-sm">
            <span className="text-muted">Quantity{order.filled_quantity ? `, including ${order.filled_quantity} filled` : ""}</span>
            <input className="trade-input" inputMode="numeric" value={draft.quantity} aria-label="Quantity"
              onChange={(e) => setDraft({ ...draft, quantity: e.target.value })} />
          </label>
        )}
        {fields.limit && (
          <label className="block space-y-1 text-sm">
            <span className="text-muted">{priceLabel}</span>
            <input className="trade-input" inputMode="decimal" value={draft.limit_price} aria-label={priceLabel}
              onChange={(e) => setDraft({ ...draft, limit_price: e.target.value })} />
          </label>
        )}
        {fields.trigger && order.trigger && (
          <label className="block space-y-1 text-sm">
            <span className="text-muted">Trigger level: activates when {describeTrigger({ ...order.trigger, level: draft.trigger_level || order.trigger.level }, side, order.underlying)}</span>
            <input className="trade-input" inputMode="decimal" value={draft.trigger_level} aria-label="Trigger level"
              onChange={(e) => setDraft({ ...draft, trigger_level: e.target.value })} />
          </label>
        )}
        {fields.tif && (
          <label className="block space-y-1 text-sm">
            <span className="text-muted">Time in force</span>
            <select className="trade-input" aria-label="Time in force" value={draft.time_in_force ?? order.time_in_force}
              onChange={(e) => setDraft({ ...draft, time_in_force: e.target.value as "day" | "gtc" })}>
              <option value="day">DAY: ends with this session</option><option value="gtc">GTC: good until expiry</option>
            </select>
          </label>
        )}
        {walk.fields}
        {order.reduce_only && <p className="text-xs text-muted">This close’s size follows its position. Changing its price stops automatic repricing.</p>}
        {order.role && <p className="text-xs text-muted">A bracket exit closes at most the position it protects: make it smaller to take part off, and the other exit keeps protecting the rest.</p>}
        {"error" in result && <p className="text-xs text-warn" role="status">{result.error}</p>}
        {"change" in result && <OrderPreviewPanel sizing={!order.reduce_only && !order.role} what="change" preview={preview} disabled={!fields.quantity}
          onSize={(size) => setDraft({ ...draft, quantity: String(order.filled_quantity + size) })} />}
        <p className="text-[11px] text-muted">
          The order keeps its number and fills. The new terms are checked like a new order, and a limit that crosses the market fills at once.
        </p>
        <TradingError error={write.error} />
        <button type="submit" className="trade-button" disabled={!("change" in result) || write.pending || write.blocked}>
          {write.pending ? "Saving…" : "unchanged" in result ? "No changes" : "Save changes"}
        </button>
      </form>
    </Dialog>
  )
}

/** Confirm a single cancellation, including the protection an exit removes. */
export function CancelOrderDialog({ order, trading, onClose, onDone }: {
  order: Order; trading: TradingStatus; onClose: () => void; onDone: (message: string) => void
}) {
  const write = useWrite(trading)
  return <Dialog title={`Cancel order #${order.id}`} onClose={onClose}>
    <p className="text-sm">Cancel the remaining {order.remaining_quantity} {order.remaining_quantity === 1 ? "unit" : "units"} of {orderLabel(order)}?</p>
    {order.role && <p className="text-sm text-warn">This bracket exit protects a position; the position stays open without it.</p>}
    <WriteAccess trading={trading} />
    <TradingError error={write.error} />
    <div className="flex flex-wrap gap-2">
      <button type="button" className="trade-button" disabled={write.pending} onClick={onClose}>Keep order</button>
      <button type="button" className="trade-button" disabled={write.pending || write.blocked || !isOpen(order)}
        onClick={() => void write.run(() => api.cancelOrder(order.id, trading.write),
          ({ order: result }) => onDone(`${orderLabel(result)}: ${result.status}`))}>
        {write.pending ? "Cancelling…" : "Cancel order"}
      </button>
    </div>
  </Dialog>
}

/** Cancel every working and armed order, or one underlying's. */
export function CancelAllDialog({ orders, trading, onClose, onDone }: {
  orders: readonly Order[]; trading: TradingStatus; onClose: () => void; onDone: (message: string) => void
}) {
  const write = useWrite(trading)
  const [scope, setScope] = useState<string | null>(null)
  const cancelling = openOrdersIn(orders, scope)
  const exits = cancelling.filter((o) => o.role != null).length
  return (
    <Dialog title="Cancel orders" onClose={onClose}>
      <ScopePicker value={scope} options={underlyingsOf(orders.filter(isOpen))} onChange={setScope} what="Cancel" />
      <p className="text-sm">
        This cancels {cancelling.length} open {cancelling.length === 1 ? "order" : "orders"}{scope ? ` on ${scope}` : ""}, armed ones included.
      </p>
      {exits > 0 && <p className="text-sm text-warn">{exits === 1 ? "1 bracket exit protects" : `${exits} bracket exits protect`} a position; the position stays open without it.</p>}
      <WriteAccess trading={trading} />
      <TradingError error={write.error} />
      <button type="button" className="trade-button" disabled={!cancelling.length || write.pending || write.blocked}
        onClick={() => void write.run(() => api.cancelAllOrders(scope, trading.write),
          (response) => onDone(`${response.cancelled_orders.length} ${response.cancelled_orders.length === 1 ? "order" : "orders"} cancelled`))}>
        {write.pending ? "Cancelling…" : `Cancel ${cancelling.length} ${cancelling.length === 1 ? "order" : "orders"}`}
      </button>
    </Dialog>
  )
}

/** What a residual is waiting for: "3 working on later quotes", or why it stays. */
export function residualNote(residual: FlattenResidual, closing: readonly Position[] = []): string {
  const position = closing.find((p) => p.symbol === residual.symbol)
  const name = position ? contractLabel(position) : orderLabel({ symbol: residual.symbol, underlying: residual.underlying })
  const held = Math.abs(residual.quantity)
  const why = residual.reason?.message.replace(/\.$/, "")
  if (!residual.working) return `${held} ${name} stay open${why ? `: ${why}` : ""}.`
  const working = `${residual.working === held ? "" : `${residual.working} `}working on later quotes until filled or the session ends`
  return `${held} ${name} still open: ${working}${why ? `; ${held - residual.working} stay: ${why}` : ""}.`
}

/**
 * What a flatten did: each closing order's outcome, the shares it traded and the shares
 * it left, and the positions still open, with what their closes are still working.
 */
export function FlattenOutcome({ done, closing = [] }: { done: ClosePositionsResponse; closing?: readonly Position[] }) {
  const stockFills = done.stock_fills ?? []
  const kept = done.kept_stocks ?? []
  const residuals = done.residuals
  // Older servers report no residuals and sell a long only as far as the shorts still
  // held leave it free, so a long that covers a short stays until a later flatten.
  const covering = residuals ? [] : closing.flatMap((position) => {
    if (position.quantity <= 0) return []
    const selling = done.orders.filter((o) => o.symbol === position.symbol && o.side === "sell").reduce((sum, o) => sum + o.quantity, 0)
    return selling < position.quantity ? [{ position, contracts: position.quantity - selling }] : []
  })
  return (
    <>
      <ul className="space-y-1 text-sm">
        {done.orders.map((order) => (
          <li key={order.id} className="flex flex-wrap justify-between gap-2">
            <span>{order.legs?.length ? "Close" : order.side === "buy" ? "Buy" : "Sell"} {order.quantity} {orderLabel(order)}</span>
            <span className={order.status === "filled" ? "text-bullish" : "text-warn"}>{outcome(order)}</span>
          </li>
        ))}
        {stockFills.map((fill) => (
          <li key={`shares-${fill.id}`} className="flex flex-wrap justify-between gap-2">
            <span>{fill.shares < 0 ? "Sell" : "Buy back"} {Math.abs(fill.shares)} {fill.symbol} shares</span>
            <span className="text-bullish">Filled at {formatMoney(fill.price)}</span>
          </li>
        ))}
      </ul>
      {kept.map((stock) => (
        <p key={stock.symbol} role="status" className="text-sm text-warn">
          {Math.abs(stock.shares)} {stock.symbol} shares stay open: {stock.reason.message}.
        </p>
      ))}
      {covering.map(({ position, contracts }) => (
        <p key={`covering-${position.symbol}`} role="status" className="text-sm text-warn">
          {contracts} {contractLabel(position)} stay open: they cover a short that is still held or being bought back. Flatten again once it is closed.
        </p>
      ))}
      {(residuals ?? []).map((residual) => (
        <p key={`residual-${residual.symbol}`} role="status" className={`text-sm ${residual.reason ? "text-warn" : "text-muted"}`}>
          {residualNote(residual, closing)}
        </p>
      ))}
      {(residuals ?? []).some((r) => r.working > 0) && <p className="text-xs text-muted">
        Working closes show on the Orders page, where they can be cancelled. Bracket exits keep protecting what is still open until it is flat.</p>}
      {!done.orders.length && !stockFills.length && !kept.length && !covering.length && !residuals?.length && <p className="text-sm text-muted">No position needed closing.</p>}
      <p className="text-xs text-muted">{done.cancelled_orders.length} {done.cancelled_orders.length === 1 ? "order" : "orders"} cancelled, with any bracket exit whose position it closed.</p>
    </>
  )
}

/**
 * Flatten: cancel the orders in scope, then close every position in it at market,
 * short positions first. Shows each closing order's outcome afterwards.
 */
/** A flatten's dry run for `scope`, refetched when the account changes. */
export function useFlattenPreview(scope: string | null, trading: TradingStatus, enabled: boolean, pricing?: FlattenPricing) {
  const { accountScope } = useLive()
  const token = useWriteToken()
  return useQuery({
    queryKey: ["trading", accountScope, "preview-flatten", scope, pricing, trading.account_version, trading.write, token],
    queryFn: () => api.previewFlatten(scope, trading.write, pricing),
    enabled: enabled && trading.enabled,
    retry: false,
  })
}
function moneyChange(after: string, before: string) {
  const change = Number(after) - Number(before)
  return `${change >= 0 ? "+" : "−"}${formatMoney(Math.abs(change).toFixed(2))}`
}
/** What the flatten would do now, from its dry run: fills, what waits or is refused, and the account after it. */
export function FlattenDryRun({ preview }: { preview: { data?: FlattenPreview; error: unknown; isFetching: boolean } }) {
  const p = preview.data
  if (preview.isFetching && !p) return <p className="text-xs text-muted">Checking the flatten at the current quotes…</p>
  if (preview.error || !p) return preview.error ? <p role="status" className="text-xs text-warn">The dry run failed. You can still flatten for the server's checks.</p> : null
  if (p.decision !== "ok") return <p role="status" className="text-sm text-warn">The flatten would be refused: {p.reason?.message ?? p.decision}.</p>
  const filled = p.orders.filter((o) => o.filled_quantity > 0)
  const waiting = p.orders.filter((o) => o.status === "working" || o.status === "partially_filled")
  const refused = p.orders.filter((o) => o.status === "rejected" || (o.status === "cancelled" && o.filled_quantity < o.quantity))
  return <section aria-label="Flatten dry run" className="space-y-1 rounded-md border border-border p-2 text-xs">
    <p className="font-medium">Simulated dry run at the current quotes</p>
    {filled.length > 0 && <ul className="space-y-0.5 tabular">{filled.map((o, index) => <li key={index}>
      {o.legs?.length ? "Close" : o.side === "buy" ? "Buy" : "Sell"} {o.filled_quantity} {orderLabel(o)}{o.average_fill_price != null ? ` at ${formatMoney(o.average_fill_price)}${o.legs?.length ? " net" : ""}` : ""}</li>)}</ul>}
    {waiting.length > 0 && <p className="text-muted">{waiting.length} {waiting.length === 1 ? "close waits" : "closes wait"} for a later quote.</p>}
    {refused.map((o, index) => <p key={index} className="text-warn">{orderLabel(o)}: {o.reason?.message ?? o.status}</p>)}
    {p.current && p.after && <p>Equity after {formatMoney(p.after.equity)} ({moneyChange(p.after.equity, p.current.equity)}),
      buying power {formatMoney(p.after.buying_power)}</p>}
    {(p.remaining.length > 0 || p.remaining_shares.length > 0) && <p className="text-muted">Still held: {[
      ...p.remaining.map((r) => `${r.quantity} ${osiLabel(r.symbol, "")}`), ...p.remaining_shares.map((r) => `${r.shares} ${r.symbol} shares`)].join(", ")}</p>}
  </section>
}
export function FlattenDialog({ positions, stocks = [], orders, trading, initial = null, onClose }: {
  positions: readonly Position[]; stocks?: readonly StockHolding[]; orders: readonly Order[]; trading: TradingStatus; initial?: string | null; onClose: () => void
}) {
  const write = useWrite(trading)
  const { underlyings } = useLive()
  const [scope, setScope] = useState<string | null>(initial)
  const [done, setDone] = useState<ClosePositionsResponse | null>(null)
  const [type, setType] = useState<"market" | "limit">("market")
  const [limitTicks, setLimitTicks] = useState("0")
  const ticksValid = /^\d+$/.test(limitTicks) && Number(limitTicks) <= 10
  const pricing: FlattenPricing | undefined = type === "limit" ? { type, limit_ticks: Number(limitTicks) } : undefined
  // The positions it set out to close, as they were when Close was pressed.
  const [closed, setClosed] = useState<readonly Position[]>([])
  const plan = flattenPlan(positions, orders, scope)
  const shares = stocks.filter((s) => s.shares !== 0 && (scope == null || s.symbol === scope))
  // Flattening sends market orders, which the overnight and curb sessions refuse.
  const limitOnly = [...new Set(plan.closing.map((p) => p.underlying))]
    .filter((symbol) => extendedSession(underlyings.find((u) => u.symbol === symbol)) != null)
  const waiting = positions.filter((p) => p.awaiting_settlement && (scope == null || p.underlying === scope)).length
  // An underlying whose paper orders are refused (a stalled feed, a halt) cannot close,
  // so the flatten leaves its positions and orders alone.
  const closingUnderlyings = [...new Set([...plan.closing.map((p) => p.underlying), ...shares.map((s) => s.symbol)])]
  const refused = closingUnderlyings.flatMap((symbol) => {
    const underlying = underlyings.find((u) => u.symbol === symbol)
    return underlying?.paper && !underlying.paper.accepting ? [{ symbol, notice: (paperNotice(symbol, underlying) ?? "").replace(/\.$/, "") }] : []
  })
  const cancelling = plan.cancelling.filter((o) => !refused.some((r) => r.symbol === o.underlying))
  const dryRun = useFlattenPreview(scope, trading, done == null && plan.closing.length + shares.length > 0 && !write.pending && (type === "market" || ticksValid), pricing)
  const exits = openOrdersIn(orders, scope).filter((o) => o.role != null && !refused.some((r) => r.symbol === o.underlying)).length
  return (
    <Dialog title={scope ? `Flatten ${scope}` : "Close all positions"} onClose={onClose}>
      {done ? (
        <div className="space-y-3">
          <FlattenOutcome done={done} closing={closed} />
          <button type="button" className="trade-button" onClick={onClose}>Done</button>
        </div>
      ) : (
        <>
          <ScopePicker value={scope} options={[...new Set([...underlyingsOf(positions.filter((p) => p.quantity !== 0)), ...stocks.map((s) => s.symbol)])].sort()}
            onChange={setScope} what="Close" />
          <label className="trade-label">Flatten order type
            <select className="trade-input" value={type} onChange={(e) => setType(e.target.value as "market" | "limit")}>
              <option value="market">Market</option><option value="limit">Limit</option>
            </select>
          </label>
          {type === "limit" && <label className="trade-label">Ticks through the touch
            <input className="trade-input" type="number" min="0" max="10" step="1" value={limitTicks} onChange={(e) => setLimitTicks(e.target.value)} />
            <span className="text-xs text-muted">0–10 ticks from each leg’s bid to sell or ask to buy. Reprices on fresh quotes through this trading date’s last session. Changing an order’s price stops automatic repricing.</span>
          </label>}
          {plan.closing.length || shares.length ? (<>
            <ul className="space-y-1 text-sm">
              {plan.closing.map((position) => (
                <li key={position.symbol} className="flex justify-between gap-2">
                  <span>{contractLabel(position)}</span>
                  <span className={position.quantity > 0 ? "text-bearish" : "text-bullish"}>{closingAction(position)} at {type === "market" ? "market" : "a working limit"}</span>
                </li>
              ))}
            </ul>
            {shares.length > 0 && <ul className="space-y-1 text-sm">
              {shares.map((s) => <li key={s.symbol} className="flex justify-between gap-2">
                <span>{Math.abs(s.shares)} {s.symbol} shares</span>
                <span className={s.shares > 0 ? "text-bearish" : "text-bullish"}>{s.shares > 0 ? "Sell" : "Buy back"} at the price</span>
              </li>)}
            </ul>}
          </>) : <p className="text-sm text-muted">No position{scope ? ` on ${scope}` : ""} can trade now.</p>}
          <p className="text-xs text-muted">
            {cancelling.length ? `${cancelling.length} working ${cancelling.length === 1 ? "order is" : "orders are"} cancelled first. ` : ""}
            {exits ? `${exits === 1 ? "1 bracket exit stays" : `${exits} bracket exits stay`} until the position ${exits === 1 ? "it protects" : "they protect"} is flat. ` : ""}
            Each position closes with a {type} order at the displayed quote; a short and the long that covers it close together as one order,
            so a spread never leaves a naked short. What a thin quote cannot fill keeps working on later quotes until it fills or the session ends,
            and a position larger than the order size limit closes in several orders.
            An order the account's rules refuse is reported and the rest still close.
          </p>
          {refused.map(({ symbol, notice }) => <p key={symbol} role="status" className="text-sm text-warn">
            {notice}. Its positions and orders stay as they are.</p>)}
          {waiting > 0 && <p className="text-xs text-muted">{waiting === 1 ? "1 expired position waits" : `${waiting} expired positions wait`} for settlement.</p>}
          {type === "market" && limitOnly.length > 0 && <p role="status" className="text-sm text-warn">
            {limitOnly.join(", ")} {limitOnly.length === 1 ? "is" : "are"} outside the regular session, which takes limit orders only.
            Choose Limit to flatten in this session.</p>}
          {plan.closing.length + shares.length > 0 && <FlattenDryRun preview={dryRun} />}
          <WriteAccess trading={trading} />
          <TradingError error={write.error} />
          <button type="button" className="trade-button" disabled={!(plan.closing.length + shares.length) || (type === "market" ? limitOnly.length > 0 : !ticksValid) || refused.length === closingUnderlyings.length || write.pending || write.blocked}
            onClick={() => {
              const closing = plan.closing
              void write.run(() => api.closePositions(scope, trading.write, pricing), (result) => { setClosed(closing); setDone(result) })
            }}>
            {write.pending ? "Closing…" : `Close ${plan.closing.length + shares.length} ${plan.closing.length + shares.length === 1 ? "position" : "positions"}`}
          </button>
        </>
      )}
    </Dialog>
  )
}
