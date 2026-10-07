import { ActionDestination, useActionDestination } from "../api/action-destination"
import { useActionApi } from "../api/action-client"
import { useState } from "react"

import { useOpenOrders } from "../api/trading"
import type { Bracket, NewOrder, Order, Position, Side, TradingStatus } from "../api/trading-types"
import { describeTrigger, stopDirection } from "../lib/ticket"
import { formatMoney, roundToTick, validMoney } from "../lib/trading"
import { Dialog } from "./Dialog"
import { EditOrderDialog, useWrite } from "./OrderActions"
import { useStopTrail } from "./TrailFields"
import { TradingError, WriteAccess } from "./TradingControls"

/** A held contract's open exits: the stop-loss and take-profit orders that close it. */
export function positionExits(position: Position, orders: readonly Order[]): Order[] {
  return orders.filter((o) => o.symbol === position.symbol && o.role != null &&
    (o.status === "working" || o.status === "partially_filled" || o.status === "armed"))
}

const positive = (value: string) => validMoney(value) && Number(value) > 0

/**
 * The held contract's exits as a request: a take-profit limit and a stop (a
 * stop-limit with a limit price) that cancel each other, sized to the holding.
 * The order itself is the take-profit, or the stop when there is no target.
 */
export function heldExitsOrder(position: Position, clientId: string, bracket: Bracket): NewOrder | null {
  const primary = bracket.take_profit ?? bracket.stop_loss
  if (!primary) return null
  const base = { client_order_id: clientId, symbol: position.symbol, side: (position.quantity > 0 ? "sell" : "buy") as Side,
    quantity: Math.abs(position.quantity), exits_only: true, bracket }
  return primary.limit_price != null
    ? { ...base, type: "limit", time_in_force: "gtc", limit_price: primary.limit_price, ...(primary.trigger ? { trigger: primary.trigger } : {}) }
    : { ...base, type: "market", time_in_force: "ioc", trigger: primary.trigger! }
}

/** Set a held contract's stop-loss and take-profit, or change and cancel the ones it has. */
export function PositionExitsDialog({ position, trading, onClose }: { position: Position; trading: TradingStatus; onClose: () => void }) {
  const destination = useActionDestination()
  const api = useActionApi()
  const write = useWrite(trading)
  const orders = useOpenOrders()
  const active = positionExits(position, orders.data?.orders ?? [])
  const [editing, setEditing] = useState<Order | null>(null)
  const [clientId] = useState(() => crypto.randomUUID())
  const long = position.quantity > 0
  const opened: Side = long ? "buy" : "sell"
  const exitSide: Side = long ? "sell" : "buy"
  const root = position.symbol.slice(0, 6).trim()
  const basis = Number(position.average_price)
  const [targetOn, setTargetOn] = useState(true)
  const [target, setTarget] = useState(() => roundToTick(root, basis * (long ? 1.5 : 0.5)) ?? "")
  const [stopOn, setStopOn] = useState(true)
  const [source, setSource] = useState<"option" | "underlying">("option")
  const [level, setLevel] = useState(() => roundToTick(root, basis * (long ? 0.75 : 1.25)) ?? "")
  const [limitOn, setLimitOn] = useState(false)
  const [limit, setLimit] = useState("")
  const stopTrail = useStopTrail(source)
  if (editing) return <ActionDestination value={destination}><EditOrderDialog order={editing} trading={trading} onClose={() => setEditing(null)} onDone={() => setEditing(null)} /></ActionDestination>
  const trigger = { source, direction: stopDirection(source, opened, position.type), level, ...stopTrail.terms }
  const stopValid = positive(level) && (!limitOn || positive(limit)) && stopTrail.valid
  const valid = (targetOn || stopOn) && (!targetOn || positive(target)) && (!stopOn || stopValid)
  const bracket: Bracket | null = valid ? {
    ...(targetOn ? { take_profit: { limit_price: target } } : {}),
    ...(stopOn ? { stop_loss: { trigger, ...(limitOn ? { limit_price: limit } : {}) } } : {}),
  } : null
  const request = bracket ? heldExitsOrder(position, clientId, bracket) : null
  const stopText = describeTrigger(trigger, exitSide, position.underlying)
  return <Dialog title={`Exits · ${position.symbol}`} onClose={onClose}>
    <p className="text-sm">{long ? `${position.quantity} long` : `${-position.quantity} short`} {position.type}s at {formatMoney(position.average_price)}</p>
    <WriteAccess trading={trading} />
    <TradingError error={write.error ?? orders.error} />
    {active.length ? <>
      {active.map((order) => <div className="flex items-center justify-between gap-3 text-sm" key={order.id}>
        <span>{order.role === "stop_loss" ? "Stop loss" : "Take profit"} · #{order.id} · {order.trigger
          ? `${describeTrigger(order.trigger, order.side ?? exitSide, position.underlying)}${order.limit_price != null ? `, limit ${formatMoney(order.limit_price)}` : ""}`
          : formatMoney(order.limit_price)}</span>
        <button type="button" className="trade-button" disabled={write.pending || write.blocked} onClick={() => setEditing(order)}>Change</button>
      </div>)}
      <button type="button" className="trade-button" disabled={write.pending || write.blocked}
        onClick={() => void write.run(() => api.cancelOrders(active.map((order) => order.id), trading.write), onClose)}>Cancel exits</button>
    </> : <form className="space-y-3 text-xs" onSubmit={(e) => { e.preventDefault(); if (request) void write.run(() => api.submitOrder(request, trading.write), onClose) }}>
      <fieldset className="space-y-3" disabled={write.pending}>
        <label className="flex items-center gap-2"><input type="checkbox" checked={targetOn} onChange={(e) => setTargetOn(e.target.checked)} />Take profit</label>
        {targetOn && <label className="trade-label">Limit price ($)<input className="trade-input" inputMode="decimal" value={target} onChange={(e) => setTarget(e.target.value)} />
          <span className="text-muted">{positive(target) ? `Rests as a ${formatMoney(target)} GTC limit to ${exitSide}.` : "Enter a target price."}</span></label>}
        <label className="flex items-center gap-2"><input type="checkbox" checked={stopOn} onChange={(e) => setStopOn(e.target.checked)} />Stop loss</label>
        {stopOn && <>
          <label className="trade-label">Stop source<select className="trade-input" value={source} onChange={(e) => { setSource(e.target.value as "option" | "underlying"); setLevel("") }}>
            <option value="option">Option {exitSide === "sell" ? "bid" : "ask"}</option><option value="underlying">{position.underlying}</option>
          </select></label>
          <label className="trade-label">{source === "option" ? "Stop price ($)" : `${position.underlying} level`}<input className="trade-input" inputMode="decimal" value={level} onChange={(e) => setLevel(e.target.value)} /></label>
          <label className="flex items-center gap-2"><input type="checkbox" checked={limitOn} onChange={(e) => { setLimitOn(e.target.checked); if (e.target.checked && !limit && source === "option") setLimit(level) }} />Stop-limit</label>
          {limitOn && <label className="trade-label">Stop limit price ($)<input className="trade-input" inputMode="decimal" value={limit} onChange={(e) => setLimit(e.target.value)} /></label>}
          {stopTrail.fields}
          <span className="text-muted">{!positive(level) ? "Enter a stop level." : !limitOn ? `${long ? "Sells" : "Buys"} at market when ${stopText}.`
            : positive(limit) ? `When ${stopText}, rests as a ${formatMoney(limit)} limit to ${exitSide}, and keeps waiting if the market gaps through it.` : "Enter the stop-limit's price."}</span>
        </>}
        <p className="text-muted">Exits close the {Math.abs(position.quantity)} held, wait outside the regular session, and last until the contract’s last trade or auto-close. One filling completely cancels the other; a stop that fills only in part re-arms for the rest. Closing the position cancels them.</p>
      </fieldset>
      <button className="trade-button" disabled={!request || write.pending || write.blocked || orders.isPending || !!orders.error}>Set exits</button>
    </form>}
  </Dialog>
}
