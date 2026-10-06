import { useQueries, useQuery } from "@tanstack/react-query"
import { useEffect, useRef, useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { useOpenOrders } from "../api/trading"
import type { NewOrder, Order, TradingStatus } from "../api/trading-types"
import type { Chain } from "../api/types"
import { days, expiryLabel, fixed } from "../lib/format"
import { closingPlan, rollPlan, rollSides, type StrategyGroup } from "../lib/positions"
import { matchingPayload } from "../lib/payload"
import { describeTrail } from "../lib/ticket"
import type { StrategyLeg } from "../lib/strategy"
import { formatMoney, signedMoney } from "../lib/trading"
import { comboTickCents } from "../lib/trading"
import { EditOrderDialog, useWrite } from "./OrderActions"
import { useSpreadExits } from "./SpreadExits"
import { Dialog } from "./Dialog"
import { StrategyTicket } from "./StrategyTicket"
import { TradingError, WriteAccess } from "./TradingControls"
import { Badge, toneOf, toneText } from "./ui"

/** Every chain the legs trade in, all strikes, for their live quotes. */
function useChains(underlying: string, ids: readonly string[]) {
  const { version } = useLive()
  // useQueries does not supply previous data to placeholder callbacks.
  const previous = useRef(new Map<string, Chain>())
  const results = useQueries({ queries: ids.map((id) => ({
    queryKey: ["chain", underlying, id, 0, version(underlying)],
    queryFn: ({ signal }: { signal: AbortSignal }) => api.chain(underlying, id, 0, signal),
    placeholderData: () => matchingPayload(previous.current.get(id), underlying, id),
  })) })
  const chains = results.map((r) => r.data).filter((c): c is Chain => c != null)
  useEffect(() => {
    previous.current = new Map(chains.map((chain) => [chain.expiry.id, chain]))
  }, [chains])
  return { chains, ready: chains.length === ids.length, error: results.find((r) => r.error)?.error }
}
const withQuotes = (legs: StrategyLeg[], chains: readonly Chain[]) => legs.map((leg) => ({ ...leg,
  quote: chains.find((c) => c.expiry.id === leg.expiry)?.strikes.find((r) => r.strike === leg.strike)?.[leg.type] ?? leg.quote }))

/** One multi-leg order that closes held positions together, each leg reversed. */
export function CloseStrategyDialog({ plan, underlying, title = "Close together", trading, onClose }: {
  plan: { legs: StrategyLeg[]; units: number }; underlying: string; title?: string; trading: TradingStatus; onClose: () => void
}) {
  const ids = [...new Set(plan.legs.map((l) => l.expiry))]
  const { chains, ready, error } = useChains(underlying, ids)
  const [edited, setEdited] = useState<StrategyLeg[] | null>(null)
  if (!ready) return error ? <Dialog title={title} onClose={onClose}><TradingError error={error} /></Dialog> : null
  return <StrategyTicket title={title} closing legs={withQuotes(edited ?? plan.legs, chains)} onLegs={setEdited} expiries={chains.map((c) => c.expiry)}
    underlying={underlying} spot={chains[0]?.spot} trading={trading} variant="dialog" units={plan.units} onClose={onClose} />
}

/** Close a strategy and open it again at a later expiry, as one order. */
export function RollDialog({ group, trading, onClose }: { group: StrategyGroup; trading: TradingStatus; onClose: () => void }) {
  const { version } = useLive()
  const summary = useQuery({
    queryKey: ["summary", group.underlying, version(group.underlying)],
    queryFn: ({ signal }) => api.summary(group.underlying, signal),
    placeholderData: (previous) => matchingPayload(previous, group.underlying),
  })
  // The same expiry rolls to new strikes; later ones keep them unless changed.
  const later = (summary.data?.expiries ?? []).filter((e) => e.id >= group.expiry)
  const sides = rollSides(group)
  const whole = group.legs.length === 4 && sides.length === 2
  const [side, setSide] = useState<"put" | "call" | undefined>(group.legs.length > 2 ? sides[0] : undefined)
  const [strikes, setStrikes] = useState<number[] | undefined>()
  const selected = side ? group.legs.filter(({ leg }) => leg.type === side) : group.legs
  const [choice, setChoice] = useState<string | null>(null)
  const target = choice ?? later.find((e) => e.id > group.expiry)?.id ?? later[0]?.id ?? null
  const { chains, ready, error } = useChains(group.underlying, target && target !== group.expiry ? [group.expiry, target] : [group.expiry])
  const [edited, setEdited] = useState<{ target: string; legs: StrategyLeg[] } | null>(null)
  const targetChain = chains.find((c) => c.expiry.id === target)
  const plan = targetChain ? rollPlan(group, { id: targetChain.expiry.id, strikes: targetChain.strikes }, side, strikes) : null
  const title = `Roll ${group.label.toLowerCase()}`
  return (
    <Dialog title={title} onClose={onClose}>
      <div className="text-sm">
        <div className="font-medium">{group.title}</div>
        <div className="text-xs text-muted">{group.units} unit{group.units === 1 ? "" : "s"}, opened at {group.cost == null ? "—" : `${formatMoney(Math.abs(group.cost).toFixed(2))} ${group.cost < 0 ? "credit" : "debit"}`}</div>
      </div>
      {group.legs.length > 2 && <label className="trade-label">Roll side
        <select className="trade-input" value={side ?? "both"} onChange={(e) => {
          setSide(e.target.value === "both" ? undefined : e.target.value as "put" | "call"); setStrikes(undefined); setEdited(null)
        }}>
          {sides.map((value) => <option key={value} value={value}>{value === "put" ? "Put vertical" : "Call vertical"}</option>)}
          {whole && <option value="both">Whole condor (eight legs)</option>}
        </select>
      </label>}
      <label className="flex items-center gap-2 text-sm">
        <span className="text-muted">Roll to</span>
        <select className="trade-input !w-auto !py-1" value={target ?? ""} disabled={!later.length}
          onChange={(e) => { setChoice(e.target.value); setStrikes(undefined); setEdited(null) }}>
          {later.map((e) => <option key={e.id} value={e.id}>{expiryLabel(e.id, true)} · {e.id === group.expiry ? "same expiry, new strikes" : days(e.days)}</option>)}
        </select>
      </label>
      {targetChain && selected.map(({ leg }, index) => <label className="trade-label" key={leg.symbol}>New {leg.type} strike ({leg.side})
        <select className="trade-input" value={strikes?.[index] ?? leg.strike} onChange={(e) => {
          const next = strikes ?? selected.map((member) => member.leg.strike)
          setStrikes(next.map((value, i) => i === index ? Number(e.target.value) : value)); setEdited(null)
        }}>
          {targetChain.strikes.filter((row) => row[leg.type]?.tradable).map((row) => <option key={row.strike} value={row.strike}>{row.strike}</option>)}
        </select>
      </label>)}
      <TradingError error={summary.error ?? error} />
      {summary.data && !later.length && <p className="text-sm text-muted">No expiry is listed.</p>}
      {plan && "reason" in plan && <p className="text-sm text-warn">{plan.reason}</p>}
      {ready && plan && "legs" in plan && target && (
        <StrategyTicket variant="bare" roll title={title} legs={withQuotes(edited?.target === target ? edited.legs : plan.legs, chains)}
          onLegs={(legs) => setEdited({ target, legs })} expiries={chains.map((c) => c.expiry)} underlying={group.underlying}
          spot={chains[0]?.spot} trading={trading} units={plan.units} onClose={onClose} />
      )}
    </Dialog>
  )
}

/** Held strategies with their net value, P&L and Greeks, and one-click close and roll. */
export function Strategies({ groups, trading, now = Date.now() }: { groups: readonly StrategyGroup[]; trading: TradingStatus
  /** The market's clock, for the time to expiry: a replay's while trading one. */
  now?: number }) {
  const [closing, setClosing] = useState<StrategyGroup | null>(null)
  const [exiting, setExiting] = useState<StrategyGroup | null>(null)
  const [rolling, setRolling] = useState<StrategyGroup | null>(null)
  const net = (value: number | null) => value == null ? "—" : `${formatMoney(Math.abs(value).toFixed(2))} ${value < 0 ? "cr" : "db"}`
  return <>
    <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Strategies">
      <table className="w-full text-right text-xs tabular whitespace-nowrap">
        <thead className="text-muted"><tr>{["Strategy", "Units", "Opened", "Now", "Open P&L before fees", "Max profit", "Max loss", "Dollar delta", "Gamma / 1%", "Vega", "Theta", "To expiry", ""].map((h, i) =>
          <th key={h} scope="col" className={`px-2 py-2 text-[11px] font-normal uppercase tracking-wide ${i === 0 ? "text-left" : ""}`}>{h}</th>)}</tr></thead>
        <tbody className="[&_td]:px-2 [&_td]:py-2 [&_tr]:border-t [&_tr]:border-border/40">
          {groups.map((group) => {
            const left = group.expires != null && Number.isFinite(group.expires) ? (group.expires - now) / 86_400_000 : null
            return <tr key={group.order.id}>
              <td className="text-left">
                <div className="flex items-center gap-1.5 font-medium">{group.label} <Badge tone="accent">{group.legs.length} legs</Badge></div>
                <div className="mt-0.5 text-[11px] text-faint">{group.title} · #{group.order.id}</div>
              </td>
              <td>{group.units}</td>
              <td>{net(group.cost)}</td>
              <td>{net(group.value)}</td>
              <td className={toneText[toneOf(group.pnl)]}>{group.pnl == null ? "—" : signedMoney(group.pnl.toFixed(2))}</td>
              <td className="text-bullish">{group.profile ? group.profile.maxProfit == null ? "Unlimited" : formatMoney(group.profile.maxProfit.toFixed(2)) : "—"}</td>
              <td className="text-bearish">{group.profile ? group.profile.maxLoss == null ? "Unlimited" : formatMoney(group.profile.maxLoss.toFixed(2)) : "—"}</td>
              <td>{fixed(group.greeks.dollar_delta, 2)}</td>
              <td>{fixed(group.greeks.dollar_gamma_1pct, 2)}</td>
              <td>{fixed(group.greeks.vega_dollars, 2)}</td>
              <td>{fixed(group.greeks.theta_dollars, 2)}</td>
              <td>{left == null ? "—" : days(Math.max(0, left))}</td>
              <td><div className="flex justify-end gap-1">
                <button type="button" className="trade-button" disabled={!trading.enabled} aria-label={`Close ${group.label} ${group.title}`} onClick={() => setClosing(group)}>Close</button>
                <button type="button" className="trade-button" disabled={!trading.enabled || (group.legs.length > 2 && !rollSides(group).length)} aria-label={`Roll ${group.label} ${group.title}`}
                  title={group.legs.length > 2 ? "Roll the put or call vertical, or the whole condor" : "Close and reopen at new strikes or a later expiry"} onClick={() => setRolling(group)}>Roll</button>
                <button type="button" className="trade-button" disabled={!trading.enabled} aria-label={`Exits for ${group.label} ${group.title}`} onClick={() => setExiting(group)}>Exits…</button>
              </div></td>
            </tr>
          })}
        </tbody>
      </table>
    </div>
    <p className="mt-2 text-[11px] text-muted">A strategy is the positions one multi-leg order opened, while they are still held together. P&L is before fees; max profit and loss are at expiry. Greeks are dollars: gamma per 1% spot move, vega per volatility point, theta per day.</p>
    {closing && <CloseStrategyDialog plan={closingPlan(closing)} underlying={closing.underlying} title={`Close ${closing.label.toLowerCase()}`}
      trading={trading} onClose={() => setClosing(null)} />}
    {exiting && <SpreadExitsDialog group={exiting} trading={trading} onClose={() => setExiting(null)} />}
    {rolling && <RollDialog group={rolling} trading={trading} onClose={() => setRolling(null)} />}
  </>
}

/** Attach a pair to held legs, or change/cancel each existing exit in place. */
export function SpreadExitsDialog({ group, trading, onClose }: { group: StrategyGroup; trading: TradingStatus; onClose: () => void }) {
  const write = useWrite(trading)
  const orders = useOpenOrders()
  const plan = closingPlan(group)
  const active = (orders.data?.orders ?? group.exits ?? []).filter((order) => order.role &&
    order.legs?.length === plan.legs.length && order.legs.every((leg) => plan.legs.some((held) =>
      held.symbol === leg.symbol && held.side === leg.side && held.ratio === leg.ratio)))
  const [editing, setEditing] = useState<Order | null>(null)
  const [confirmCancel, setConfirmCancel] = useState(false)
  const [clientId] = useState(() => crypto.randomUUID())
  const exits = useSpreadExits(group.cost, comboTickCents(plan.legs.map((leg) => leg.symbol.slice(0, 6).trim())), true)
  if (editing) return <EditOrderDialog order={editing} trading={trading} onClose={() => setEditing(null)} onDone={() => setEditing(null)} />
  async function submit() {
    const bracket = exits.bracket
    const primary = bracket?.take_profit ?? bracket?.stop_loss
    if (!primary || !bracket) return
    const request: NewOrder = {
      client_order_id: clientId, legs: plan.legs.map(({ symbol, side, ratio }) => ({ symbol, side, ratio })),
      quantity: plan.units, exits_only: true, bracket,
      // The primary is the target, or the stop when there is none: a stop-limit is a GTC limit with its trigger.
      ...(primary.limit_price != null ? { type: "limit", time_in_force: "gtc", limit_price: primary.limit_price, ...(primary.trigger ? { trigger: primary.trigger } : {}) }
        : { type: "market", time_in_force: "ioc", trigger: primary.trigger }),
    }
    await write.run(() => api.submitOrder(request, trading.write), onClose)
  }
  return <Dialog title={`Exits · ${group.label}`} onClose={onClose}>
    <p className="text-sm">{group.title} · {group.units} units</p>
    <WriteAccess trading={trading} />
    <TradingError error={write.error ?? orders.error} />
    {active.length ? <>
      {active.map((order) => <div className="flex items-center justify-between gap-3 text-sm" key={order.id}>
        <span>{order.role === "stop_loss" ? "Stop loss" : "Take profit"} · #{order.id} · {order.trigger
          ? `${order.trigger.level}${order.trigger.trail ? `, trailing ${describeTrail(order.trigger.trail)}` : ""}${order.limit_price != null ? `, limit ${order.limit_price}` : ""}` : order.limit_price}</span>
        <button className="trade-button" disabled={write.pending || write.blocked} onClick={() => setEditing(order)}>Change</button>
      </div>)}
      {confirmCancel ? <div className="space-y-2">
        <p className="text-sm text-warn">Cancel {active.length} exits? The position stays open without their protection.</p>
        <button className="trade-button mr-2" disabled={write.pending} onClick={() => setConfirmCancel(false)}>Keep exits</button>
        <button className="trade-button" disabled={write.pending || write.blocked} onClick={() => void write.run(() => api.cancelOrders(active.map((order) => order.id), trading.write), onClose)}>{write.pending ? "Cancelling…" : "Confirm cancel exits"}</button>
      </div> : <button className="trade-button" disabled={write.pending || write.blocked} onClick={() => setConfirmCancel(true)}>Cancel exits</button>}
    </> : <form className="space-y-3" onSubmit={(e) => { e.preventDefault(); void submit() }}>
      <fieldset disabled={write.pending}>{exits.fields}</fieldset>
      <button className="trade-button" disabled={!exits.valid || !exits.bracket || write.pending || write.blocked || orders.isPending || !!orders.error}>Set exits</button>
    </form>}
  </Dialog>
}
