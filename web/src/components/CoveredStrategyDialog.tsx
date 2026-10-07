import { useActionApi } from "../api/action-client"
import { useQuery } from "@tanstack/react-query"
import { useRef, useState } from "react"

import { useLive } from "../api/live"
import { useAccount } from "../api/trading"
import type { NewOrder, Order, TradingStatus } from "../api/trading-types"
import type { Chain } from "../api/types"
import { formatMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { useWrite } from "./OrderActions"
import { OrderPreviewPanel, useOrderPreview } from "./OrderPreview"
import { OrderResult } from "./OrderTicket"
import { TradingError, WriteAccess } from "./TradingControls"

/** Two explicit orders: shares first, then a covered call or the collar's option pair. */
export function CoveredStrategyDialog({ kind, chain, trading, onClose }: {
  kind: "covered-call" | "collar"; chain: Chain; trading: TradingStatus; onClose: () => void
}) {
  const api = useActionApi()
  const { accountScope } = useLive()
  const token = useWriteToken()
  const write = useWrite(trading)
  const rules = useAccount().data?.rules
  const calls = chain.strikes.filter((r) => r.call?.tradable && r.call.symbol)
  const puts = chain.strikes.filter((r) => r.put?.tradable && r.put.symbol)
  const spot = chain.spot ?? chain.expiry.forward ?? 0
  const [call, setCall] = useState(() => (calls.find((r) => r.strike >= spot) ?? calls.at(-1))?.call?.symbol ?? "")
  const [put, setPut] = useState(() => (puts.filter((r) => r.strike <= spot).at(-1) ?? puts[0])?.put?.symbol ?? "")
  const [quantity, setQuantity] = useState("1")
  const [bought, setBought] = useState(false)
  const [attempted, setAttempted] = useState(false)
  const [order, setOrder] = useState<Order>()
  const request = useRef<NewOrder | null>(null)
  const q = Number(quantity)
  const valid = /^\d+$/.test(quantity) && Number.isSafeInteger(q) && q > 0 && q <= 100_000 &&
    calls.some((r) => r.call?.symbol === call) && (kind === "covered-call" || puts.some((r) => r.put?.symbol === put))
  const draft: NewOrder | null = valid ? {
    client_order_id: "preview:covered", quantity: q, type: "market", time_in_force: "ioc",
    ...(kind === "covered-call" ? { symbol: call, side: "sell" } : { legs: [
      { symbol: call, side: "sell", ratio: 1 }, { symbol: put, side: "buy", ratio: 1 },
    ] }), tags: [kind],
  } : null
  const preview = useOrderPreview(bought ? draft : null, trading)
  const shares = useQuery({
    queryKey: ["trading", accountScope, "covered-shares", trading.account_version, chain.symbol, q, token],
    queryFn: () => api.previewStock(chain.symbol, "buy", q * 100, trading.write),
    enabled: valid && !bought && trading.enabled && !write.blocked,
    retry: false,
  })
  const planRefusal = rules?.buy_only || rules?.defined_risk
  const stopRequired = rules?.require_stop_loss
  return <Dialog title={kind === "collar" ? "Collar" : "Covered call"} onClose={onClose}>
    <p className="text-sm">{chain.symbol} · {chain.expiry.expiry}. Buy {valid ? q * 100 : "…"} shares, then
      {kind === "collar" ? " buy puts and sell calls" : " sell calls"}. These are two separate orders, not atomic.
      Shares remain if the options are rejected, cancel or fill only partly; review Positions before retrying.</p>
    <fieldset disabled={attempted || write.pending} className="space-y-3">
      <label className="trade-label">Contracts<input className="trade-input" type="number" min="1" max="100000" step="1" value={quantity} onChange={(e) => setQuantity(e.target.value)} /></label>
      <label className="trade-label">Sell call<select className="trade-input" value={call} onChange={(e) => setCall(e.target.value)}>
        {calls.map((r) => <option key={r.call!.symbol} value={r.call!.symbol!}>{r.strike} call</option>)}</select></label>
      {kind === "collar" && <label className="trade-label">Buy put<select className="trade-input" value={put} onChange={(e) => setPut(e.target.value)}>
        {puts.map((r) => <option key={r.put!.symbol} value={r.put!.symbol!}>{r.strike} put</option>)}</select></label>}
    </fieldset>
    {planRefusal && <p role="status" className="text-warn text-sm">This plan refuses this strategy: buy-only plans prohibit short calls,
      and defined-risk plans require long calls to cover short calls. Shares cover calls for margin, but do not satisfy that plan rule.</p>}
    {stopRequired && <p role="status" className="text-warn text-sm">Stop-loss required by this plan. These share and option entries cannot attach a protective stop in this ticket.</p>}
    {!bought && <section aria-label="Share purchase preview" className="text-xs space-y-1">
      <p>Share cost: {formatMoney(shares.data?.cost)}</p>
      <p>Buying power after shares: {formatMoney(shares.data?.after.buying_power)}</p>
      {shares.data?.reason && <p className="text-warn">{shares.data.reason.message}</p>}
      <TradingError error={shares.error} />
    </section>}
    {bought && <>
      <p role="status" className="text-sm">Bought {q * 100} shares. Review and send the options next; closing this dialog keeps the shares.</p>
      <OrderPreviewPanel preview={preview} onSize={() => {}} disabled />
    </>}
    {order && <OrderResult order={order} />}
    <WriteAccess trading={trading} />
    <TradingError error={write.error} />
    {!bought && attempted && !write.pending && <p role="status" className="text-xs text-warn">Check Positions for the share purchase result before starting another strategy.</p>}
    {!bought ? <button type="button" className="trade-button" disabled={!valid || !!planRefusal || !!stopRequired || attempted || write.pending || write.blocked || trading.kill_latched}
      onClick={() => { setAttempted(true); void write.run(() => api.tradeStock(chain.symbol, "buy", q * 100, trading.write), () => setBought(true)) }}>
      {write.pending ? "Buying shares…" : `1. Buy ${valid ? q * 100 : "…"} shares`}</button>
      : <button type="button" className="trade-button" disabled={!draft || !!stopRequired || !!order || write.pending || write.blocked || trading.kill_latched}
        onClick={() => { if (!draft) return; request.current ??= { ...draft, client_order_id: crypto.randomUUID() }
          void write.run(() => api.submitOrder(request.current!, trading.write), (result) => setOrder(result.order)) }}>
        {write.pending ? "Sending options…" : "2. Send options at market"}</button>}
  </Dialog>
}
