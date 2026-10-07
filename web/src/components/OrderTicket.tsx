import { ActionDestination, useActionDestination } from "../api/action-destination"
import { useActionApi } from "../api/action-client"
import { useWalk } from "./WalkFields"
import { FeeAmount } from "./FeeAmount"
import { useJoinTrade } from "./JoinTradeField"
import { useQuery } from "@tanstack/react-query"
import { useEffect, useMemo, useRef, useState, type ReactNode } from "react"
import { LiquidityWarning } from "./LiquidityWarning"
import { ApiError } from "../api/client"
import { useLive } from "../api/live"
import { useSmileSurface } from "../api/smiles"
import { useAccount, usePortfolio, useRefreshTrading, useTradingSession } from "../api/trading"
import type { Bracket, NewOrder, Order, Side, TimeInForce, TriggerRequest, TriggerStudy, TradingStatus } from "../api/trading-types"
import type { Expiry, OptionQuote, Surface } from "../api/types"
import { count, days, fixed, isNum, price } from "../lib/format"
import { dayLockNotice, flatRuleNotice, planEntryNotice, planMarketTime } from "../lib/plan-rules"
import { OrderPreviewPanel, useOrderPreview } from "./OrderPreview"
import { addWhatIfOrder, useWhatIfScope } from "../lib/what-if"
import { whatIfOrderText } from "./WhatIfPanel"
import { probabilityOfProfit, probabilitySource, singleLeg, smileDistribution } from "../lib/probability"
import { crossDirection, describeTrigger, marketability, opposite, split, stopDirection, strategyName } from "../lib/ticket"
import { deliversShares, extendedSession, formatMoney, limitOnlyNotice, limitPriceText, limitPriceTick, paperNotice, roundToTick, sideFromCell, stepLimitPrice, ticketEstimate, validMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { TradingError, WriteAccess, writeBlocked } from "./TradingControls"
import { useStopTrail } from "./TrailFields"
import { useChainedOrder } from "./ChainFields"
import { Segmented } from "./ui"
import { goodTillTimestamp, TimeInForceField } from "./TimeInForceField"

export interface TicketSelection {
  symbol: string
  underlying: string
  expiry: Expiry
  strike: number
  optionType: "call" | "put"
  cell: "bid" | "ask"
  price: string
  /** Underlying spot for the naked-short buying-power estimate. */
  spot?: number | null
  /** Initial contract count, e.g. the whole position when closing. */
  quantity?: number
  /** A position close: keep its side and cap the quantity at the holding. */
  closing?: boolean
}

function rejected(error: unknown) {
  return error instanceof ApiError && [400, 403, 404, 409, 422].includes(error.status)
}
function retryable(error: unknown) {
  return error instanceof ApiError ? [408, 503, 504].includes(error.status)
    : error instanceof TypeError || ((error instanceof Error || error instanceof DOMException) && ["AbortError", "TimeoutError"].includes(error.name))
}

export function OrderResult({ order, error, children }: { order?: Order; error?: unknown; children?: ReactNode }) {
  const ref = useRef<HTMLDivElement>(null)
  useEffect(() => {
    if (!order && !error) return
    ref.current?.focus({ preventScroll: true })
    ref.current?.scrollIntoView({ block: "start" })
  }, [order, error])
  return <div ref={ref} role="region" tabIndex={-1} aria-label="Order result" aria-live="polite" className="space-y-2 outline-none">
    {order && <div className="rounded-md border border-border bg-raised p-3 text-sm">
      <strong className="capitalize">{order.status === "partially_filled" ? "Partial fill" : order.status}</strong>
      <div className="mt-1 tabular">{order.filled_quantity}/{order.quantity} filled · {order.remaining_quantity} remaining · Avg {formatMoney(order.average_fill_price)}</div>
      {order.reason && <p className="mt-1 text-warn">{order.reason.code}: {order.reason.message}</p>}
    </div>}
    {rejected(error) && <p className="text-sm font-medium text-danger">Rejected</p>}
    <TradingError error={error} />
    {children}
  </div>
}

const quickSizes = [1, 5, 10, 25, 50]

/** A dialog by default; `panel` docks it beside the chain. */
export function OrderTicket({ selection, quote, trading, onClose, variant = "dialog", smile, surface, tags, note }: {
  selection: TicketSelection; quote: OptionQuote | null; trading: TradingStatus; onClose: () => void; variant?: "dialog" | "panel"
  /** The expiry's smile, for the probability of profit; the option's own volatility otherwise. */
  tags?: string[]; note?: string
  smile?: readonly { strike: number; iv: number | null }[]
  surface?: Surface
}) {
  const title = "Paper order"
  const destination = useActionDestination()
  const body = <ActionDestination value={destination}><TicketBody selection={selection} quote={quote} trading={trading} onClose={onClose} variant={variant} smile={smile} surface={surface} tags={tags} note={note} /></ActionDestination>
  if (variant === "dialog") return <Dialog title={title} onClose={onClose}>{body}</Dialog>
  return (
    <aside aria-label="Order ticket" className="flex max-h-[calc(100dvh-7rem)] min-w-0 flex-col overflow-hidden rounded-lg border border-border bg-panel shadow-chart">
      <header className="flex items-center justify-between gap-3 border-b border-border px-4 py-2.5">
        <h2 className="text-xs font-medium uppercase tracking-wide text-muted">Order ticket</h2>
        <button type="button" className="trade-button" onClick={onClose} aria-label="Close order ticket">Close</button>
      </header>
      <div className="min-h-0 flex-1 space-y-4 overflow-y-auto p-4">{body}</div>
    </aside>
  )
}

function TicketBody({ selection, quote, trading, onClose, variant, smile, surface, tags, note }: {
  selection: TicketSelection; quote: OptionQuote | null; trading: TradingStatus; onClose: () => void; variant: "dialog" | "panel"
  tags?: string[]; note?: string
  smile?: readonly { strike: number; iv: number | null }[]
  surface?: Surface
}) {
  const api = useActionApi()
  const { accountScope, underlyings, source, replay, status: feedStatus } = useLive()
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const account = useAccount().data
  const portfolio = usePortfolio().data
  const join = useJoinTrade(selection.underlying, portfolio?.strategies)
  const [side, setSide] = useState<Side>(sideFromCell(selection.cell))
  const underlying = underlyings.find((u) => u.symbol === selection.underlying)
  // Extended protection runs as simulator-managed limits overnight and in curb.
  const extended = extendedSession(underlying)
  const [chosenType, setType] = useState<"limit" | "market">("limit")
  const [tif, setTif] = useState<TimeInForce>("day")
  const [goodTill, setGoodTill] = useState("")
  const good_till = goodTillTimestamp(goodTill)
  const [enteredQuantity, setQuantity] = useState(String(selection.quantity ?? 1))
  const [limitPrice, setLimitPrice] = useState(() => limitPriceText(selection.price))
  const [fee, setFee] = useState("")
  // Conditional entry and bracket exits.
  const spot = selection.spot != null && Number.isFinite(selection.spot) ? selection.spot : null
  const [chosenCondition, setCondition] = useState<"now" | "cross" | "watch" | "time">("now")
  const allSessions = tif === "exto" || tif === "gtc_exto"
  const conditional = !extended || tif === "gtc" || tif === "gtd" || allSessions
  const condition = conditional ? chosenCondition : "now"
  const marketAllowed = !extended || (allSessions && condition !== "now")
  const type = marketAllowed ? chosenType : "limit"
  const effectiveTif = type === "market" && (condition === "now" || (!allSessions && tif !== "gtd")) ? "ioc" : tif
  // No default level: one at spot would sit on the boundary, so the trader picks it.
  const [crossLevel, setCrossLevel] = useState("")
  // Another underlying's price or a study, or the time of day.
  const [watchKind, setWatchKind] = useState<"price" | TriggerStudy>("price")
  const [watchSymbol, setWatchSymbol] = useState("VIX")
  const [watchDirection, setWatchDirection] = useState<"at_or_above" | "at_or_below">("at_or_above")
  const [watchLevel, setWatchLevel] = useState("")
  const [timeAt, setTimeAt] = useState("15:30")
  const [chosenProtect, setProtect] = useState(false)
  const protect = conditional && chosenProtect
  const [stopOn, setStopOn] = useState(true)
  const [stopSource, setStopSource] = useState<"option" | "underlying">("option")
  const [stopLevel, setStopLevel] = useState("")
  // A stop-limit: once triggered, a GTC limit at this price instead of a market order.
  const [stopLimitOn, setStopLimitOn] = useState(false)
  const [stopLimit, setStopLimit] = useState("")
  // What the stop reads, and whether it trails.
  const stopTrail = useStopTrail(stopSource)
  const [targetOn, setTargetOn] = useState(true)
  const [targetSource, setTargetSource] = useState<"option" | "underlying">("option")
  const [targetLevel, setTargetLevel] = useState("")
  const [pending, setPending] = useState(false)
  const [submitted, setSubmitted] = useState(false)
  const [order, setOrder] = useState<Order>()
  const [error, setError] = useState<unknown>()
  const request = useRef<NewOrder | null>(null)
  const busy = useRef(false)
  const first = useRef<HTMLDivElement>(null)
  useEffect(() => {
    if (!submitted) first.current?.querySelector<HTMLButtonElement>('[role="radio"][aria-checked="true"]')?.focus()
  }, [submitted])
  const latest = useQuery({
    queryKey: ["trading", accountScope, "ticket-order", order?.id, trading.account_version],
    queryFn: ({ signal }) => api.orders("all", signal),
    enabled: order != null && trading.enabled,
  })
  const result = latest.data?.orders.find((item) => item.id === order?.id) ?? order
  const held = portfolio?.positions.find((p) => p.symbol === selection.symbol)?.quantity ?? 0
  const maxQuantity = selection.closing ? (portfolio ? ((held > 0) !== (side === "buy") ? Math.abs(held) : 0) : selection.quantity ?? 0) : undefined
  const quantity = maxQuantity != null && Number(enteredQuantity) > maxQuantity ? String(maxQuantity) : enteredQuantity
  const q = Number(quantity)
  // Displayed size this account's orders already took on the current quote.
  const taken = portfolio?.liquidity_used?.find((u) => u.symbol === selection.symbol)
  const leftNote = (side: "bid" | "ask") => taken && taken[`${side}_left`] < taken[`${side}_size`] ? ` · ${taken[`${side}_left`]} left` : ""
  const marketPrice = quote?.[side === "buy" ? "ask" : "bid"]
  const estimatedPrice = type === "limit" ? limitPrice : marketPrice != null && Number.isFinite(marketPrice) ? String(marketPrice) : null
  const serverFee = trading.fee_per_contract
  const effectiveFee = serverFee ?? (fee || null)
  const estimate = ticketEstimate(quote, side, q, estimatedPrice, effectiveFee)
  // An opening order's odds at expiry; a closing one ends the position, so it has none.
  const closes = held !== 0 && (held > 0) !== (side === "buy")
  const premium = Number(estimatedPrice)
  const terms = selection.expiry
  const fitted = useSmileSurface(selection.underlying, [terms.id], !surface && !closes)
  const distribution = useMemo(() => smileDistribution(terms, smile, surface ?? fitted, isNum(quote?.iv) && quote.iv > 0 ? quote.iv : terms.atm_iv), [terms, smile, surface, fitted, quote?.iv])
  const odds = !closes && Number.isFinite(premium) && premium > 0 && distribution ? (() => {
    const { breakeven, value } = singleLeg(selection.optionType, side, selection.strike, premium)
    return { breakeven, pop: probabilityOfProfit(value, [breakeven], distribution) }
  })() : null
  const watchedSymbol = watchSymbol.trim().toUpperCase()
  const trigger: TriggerRequest | undefined = condition === "cross" ? (validMoney(crossLevel) && Number(crossLevel) > 0
      ? { source: "underlying", direction: crossDirection(Number(crossLevel), spot), level: crossLevel } : undefined)
    : condition === "watch" ? (validMoney(watchLevel) && Number(watchLevel) > 0 && /^[A-Z0-9._]{0,12}$/.test(watchedSymbol) && (watchKind !== "price" || watchedSymbol !== "")
      ? { source: watchKind === "price" ? "underlying" : "study", direction: watchDirection, level: watchLevel,
          ...(watchedSymbol && watchedSymbol !== selection.underlying ? { symbol: watchedSymbol } : {}), ...(watchKind !== "price" ? { study: watchKind } : {}) }
      : undefined)
    : condition === "time" ? (/^([01]\d|2[0-3]):[0-5]\d$/.test(timeAt) ? { source: "time", at: timeAt } : undefined)
    : undefined
  const exitLevel = (value: string) => validMoney(value) && Number(value) > 0
  const stopValid = exitLevel(stopLevel) && (!stopLimitOn || exitLevel(stopLimit)) && stopTrail.valid
  const bracket: Bracket | undefined = protect ? {
    ...(stopOn && stopValid ? { stop_loss: { trigger: { source: stopSource, direction: stopDirection(stopSource, side, selection.optionType), level: stopLevel, ...stopTrail.terms },
      ...(stopLimitOn ? { limit_price: stopLimit } : {}) } } : {}),
    ...(targetOn && exitLevel(targetLevel) ? { take_profit: targetSource === "option" ? { limit_price: targetLevel }
      : { trigger: { source: "underlying" as const, direction: opposite(stopDirection("underlying", side, selection.optionType)), level: targetLevel } } } : {}),
  } : undefined
  // A second order chained to this one: placed once it fills, or cancelling each other.
  const chain = useChainedOrder({ symbol: selection.symbol, side, quantity: Number.isSafeInteger(q) && q > 0 ? q : 1, underlying: selection.underlying, spot })
  const bracketValid = !protect || ((stopOn || targetOn) && (!stopOn || stopValid) && (!targetOn || exitLevel(targetLevel)))
  const walk = useWalk(type === "limit" && condition === "now" && ["day", "gtc"].includes(effectiveTif), limitPrice, side === "buy", "0.10", String(marketPrice ?? ""))
  const valid = walk.valid && /^\d+$/.test(quantity) && Number.isSafeInteger(q * 100) && q > 0 && (type === "market" || validMoney(limitPrice)) && (effectiveFee == null || validMoney(effectiveFee)) &&
    (condition === "now" || trigger != null) && bracketValid && (effectiveTif !== "gtd" || good_till != null) && chain.valid
  const untradable = quote?.tradable !== true || quote.symbol !== selection.symbol
  const notice = paperNotice(selection.underlying, underlying)
  const limitOnly = notice ? null : limitOnlyNotice(selection.underlying, underlying)
  const rules = account?.rules
  const closed = account?.evaluation.enabled && account.evaluation.status !== "active"
  const dayLocked = account?.evaluation.status === "active" && !!account.evaluation.day_lock
  const opening = split(side, Number.isSafeInteger(q) ? q : 0, held).opening
  const stopRequired = rules?.require_stop_loss && opening > 0
  const missingStop = stopRequired && !bracket?.stop_loss?.trigger
  const buyOnlyBlock = rules?.buy_only && side === "sell" && opening > 0
  const planNotice = planEntryNotice(account?.rules, selection.underlying, planMarketTime(account?.time, underlying?.as_of, source !== "live" ? replay?.time : null), opening === 0, account)
  const blocked = !!planNotice || writeBlocked(trading, token) || ((trading.kill_latched || dayLocked || closed) && opening > 0) || untradable || !!notice || !!buyOnlyBlock || !!missingStop
  const reason = quote?.untradable_reason ?? "Contract unavailable for paper trading"
  const root = selection.symbol.slice(0, 6).trim()
  const name = strategyName(side, selection.optionType, held, Number.isSafeInteger(q) && q > 0 ? q : 1)
  const fill = type === "limit" && tif === "gtc" && extended
    ? { marketable: false, message: "GTC waits for the regular session, even if the current quote crosses its limit." }
    : marketability(side, type, limitPrice, quote)
  const draft: NewOrder | null = valid ? {
    client_order_id: "preview:single", symbol: selection.symbol, side, quantity: q,
    ...(join.group ? { group: join.group } : {}),
    ...(type === "market" ? { type, time_in_force: effectiveTif as "ioc" | "exto" | "gtc_exto" | "gtd" } : { type, time_in_force: effectiveTif, limit_price: limitPrice }),
    ...(effectiveTif === "gtd" ? { good_till } : {}),
    ...(trigger ? { trigger } : {}), ...(bracket ? { bracket } : {}),
    ...(walk.walk ? { walk: walk.walk } : {}),
  } : null
  const preview = useOrderPreview(draft, trading)
  const whatIfScope = useWhatIfScope()
  const expiryLabel = `${selection.expiry.expiry} ${selection.expiry.settlement}`

  function newOrder() {
    request.current = null
    setOrder(undefined)
    setError(undefined)
    setSubmitted(false)
  }
  async function submit() {
    if (!valid || blocked || busy.current || order || (submitted && !retryable(error))) return
    busy.current = true
    setPending(true)
    setError(undefined)
    try {
      request.current ??= {
        client_order_id: crypto.randomUUID(), symbol: selection.symbol, side, quantity: q,
        ...(join.group ? { group: join.group } : {}),
        ...(type === "market" ? { type, time_in_force: effectiveTif as "ioc" | "exto" | "gtc_exto" | "gtd" } : { type, time_in_force: effectiveTif, limit_price: limitPrice }),
        ...(effectiveTif === "gtd" ? { good_till } : {}),
        ...(trigger ? { trigger } : {}), ...(bracket ? { bracket } : {}),
        ...(walk.walk ? { walk: walk.walk } : {}), ...chain.terms,
        ...(tags ? { tags } : {}), ...(note ? { note } : {}),
      }
      setSubmitted(true)
      const response = await api.submitOrder(request.current, trading.write)
      if (sameSession()) setOrder(response.order)
    } catch (failure) {
      if (sameSession()) setError(failure)
    } finally {
      busy.current = false
      if (sameSession()) setPending(false)
      void refresh()
    }
  }
  const setPrice = (value: number | null | undefined) => { if (value != null && Number.isFinite(value)) setLimitPrice(limitPriceText(value.toFixed(2))) }
  const entryPrice = Number(estimatedPrice)
  // Suggested exits: 25% of premium at risk, 50% target, or 0.5% of spot.
  const suggest = (kind: "stop" | "target", source: "option" | "underlying") => {
    const worse = kind === "stop"
    if (source === "option") {
      if (!Number.isFinite(entryPrice) || entryPrice <= 0) return ""
      const long = side === "buy"
      const factor = worse ? (long ? 0.75 : 1.25) : (long ? 1.5 : 0.5)
      return roundToTick(root, entryPrice * factor) ?? ""
    }
    if (spot == null) return ""
    const down = stopDirection("underlying", side, selection.optionType) === "at_or_below"
    return (spot * ((down === worse) ? 0.995 : 1.005)).toFixed(2)
  }
  const enableProtection = (on: boolean) => {
    setProtect(on)
    if (on && !stopLevel) setStopLevel(suggest("stop", stopSource))
    if (on && !targetLevel) setTargetLevel(suggest("target", targetSource))
  }
  // A stop-limit starts at its option stop's own price, or at the suggested stop price.
  const enableStopLimit = (on: boolean) => {
    setStopLimitOn(on)
    if (on && !stopLimit) setStopLimit(stopSource === "option" && exitLevel(stopLevel) ? stopLevel : suggest("stop", "option"))
  }
  const exitSide = side === "buy" ? "sell" : "buy"
  const stopText = describeTrigger({ source: stopSource, direction: stopDirection(stopSource, side, selection.optionType), level: stopLevel, ...stopTrail.terms }, exitSide, selection.underlying)
  const stopHint = !exitLevel(stopLevel) ? "Enter a stop level."
    : !stopLimitOn ? `${side === "buy" ? "Sells" : "Buys"} at market when ${stopText}.`
    : !exitLevel(stopLimit) ? "Enter the stop-limit's price."
    : `When ${stopText}, rests as a ${formatMoney(stopLimit)} limit to ${exitSide}: it fills at that price or better, and keeps waiting if the market gaps through it.`
  const stepQuantity = (delta: number) => setQuantity(String(Math.max(1, (Number.isSafeInteger(q) ? q : 1) + delta)))

  return <>
    {(result != null || error != null) && <OrderResult order={result} error={error}>
      {(rejected(error) || result?.status === "rejected") && <button type="button" className="trade-button" onClick={newOrder}>New order</button>}
      {!order && retryable(error) && <>
        <button type="button" className="trade-button" disabled={!valid || blocked || pending} onClick={() => void submit()}>Retry same order</button>
        <p className="text-xs text-muted">Retries keep the same client order ID. Check Positions before starting another ticket if the response was interrupted.</p>
      </>}
      {!order && error != null && !rejected(error) && !retryable(error) && <p className="text-xs text-muted">Check Positions and Orders to confirm the order’s state before starting another ticket.</p>}
      {result && result.status !== "rejected" && variant === "panel" && <button type="button" className="trade-button" onClick={newOrder}>Another order</button>}
    </OrderResult>}
    <div>
      <div className="flex items-baseline justify-between gap-2">
        <div className="text-base font-semibold">{selection.underlying} <span className="font-normal text-muted">{name}</span></div>
        {variant === "dialog" && <button type="button" className="text-xs text-muted hover:text-foreground" onClick={onClose}>Cancel</button>}
      </div>
      <div className="mt-1 text-sm">{selection.strike} {selection.optionType} · {expiryLabel} <span className="text-xs text-muted">({days(selection.expiry.days)})</span></div>
      <div className="mt-0.5 break-all text-[11px] tabular text-faint">{selection.symbol}</div>
      <p className="mt-1 text-[11px] text-muted">{selection.expiry.style !== "american" ? "European · cash settled"
        : deliversShares(selection.underlying) ? "American · exercise and assignment deliver 100 shares a contract"
        : "American · cash settled; early exercise is not simulated"} · 100 multiplier</p>
      {held !== 0 && <p className="mt-1 text-xs"><span className="text-muted">You hold</span> <span className="tabular">{held > 0 ? `${held} long` : `${-held} short`}</span></p>}
    </div>
    <div className="grid grid-cols-3 gap-2 rounded-md border border-border bg-background/40 p-3 text-xs tabular">
      <button type="button" className="text-left hover:text-accent" disabled={type === "market"} onClick={() => setPrice(quote?.bid)} title="Use the bid as the limit">
        <span className="text-muted">Bid</span><div className="text-bearish">{price(quote?.bid)} × {count(quote?.bid_size)}{leftNote("bid")}</div></button>
      <button type="button" className="text-left hover:text-accent" disabled={type === "market"} onClick={() => setPrice(quote?.mid)} title="Use the mid as the limit">
        <span className="text-muted">Mid</span><div>{price(quote?.mid)}</div></button>
      <button type="button" className="text-left hover:text-accent" disabled={type === "market"} onClick={() => setPrice(quote?.ask)} title="Use the ask as the limit">
        <span className="text-muted">Ask</span><div className="text-bullish">{price(quote?.ask)} × {count(quote?.ask_size)}{leftNote("ask")}</div></button>
    </div>
    <WriteAccess trading={trading} />
    {notice && <p role="status" className="text-sm text-warn">{notice}</p>}
    {limitOnly && <p role="status" className="text-xs text-muted">{limitOnly}</p>}
    {(!trading.enabled || trading.kill_latched || untradable) && <p role="status" className="text-sm text-warn">{!trading.enabled ? trading.reason ?? "Trading unavailable" : trading.kill_latched ? "Kill switch latched · reduce-only: closing orders and exits still work." : reason}</p>}
    {closed && <p role="status" className="text-sm text-warn">The evaluation has {account?.evaluation.status}. Closing orders are allowed; start a new attempt from the Dashboard to open positions.</p>}
    {!planNotice && flatRuleNotice(rules) && <p className="text-xs text-muted">{flatRuleNotice(rules)}</p>}
      {planNotice && <p role="status" className="text-sm text-warn">{planNotice}</p>}
    {dayLocked && <p role="status" className="text-sm text-warn">{dayLockNotice}</p>}
    {buyOnlyBlock && <p role="status" className="text-sm text-warn">{rules?.plan ?? "This plan"} is buy-only: sells may only close contracts you hold{held > 0 ? ` (${held} long)` : ""}.</p>}
    <form className="space-y-4" onSubmit={(event) => { event.preventDefault(); void submit() }}>
      <fieldset disabled={submitted || pending} className="grid min-w-0 grid-cols-2 gap-3 disabled:opacity-70">
        <legend className="sr-only">Order details</legend>
        <div ref={first} className="trade-label">Side
          <Segmented label="Side" value={side} onChange={selection.closing ? () => {} : setSide} options={selection.closing ? [{ value: side, label: side === "buy" ? "Buy" : "Sell" }] : [{ value: "buy", label: "Buy" }, { value: "sell", label: "Sell" }]} />
        </div>
        <div className="trade-label">Order type
          <Segmented label="Order type" value={type} onChange={(next) => { setType(next); if (next === "market" && !allSessions && tif !== "gtd") setTif("ioc") }}
            options={!marketAllowed ? [{ value: "limit", label: "Limit" }] : [{ value: "limit", label: "Limit" }, { value: "market", label: "Market" }]} />
        </div>
        <div className="trade-label col-span-2">
          <label className="trade-label">Quantity<input className="trade-input" inputMode="numeric" type="number" min="1" max={maxQuantity} step="1" value={quantity} onChange={(e) => setQuantity(e.target.value)} required /></label>
          <div className="flex flex-wrap items-center gap-1.5">
            <button type="button" className="trade-button" aria-label="Decrease quantity" onClick={() => stepQuantity(-1)}>−</button>
            <button type="button" className="trade-button" aria-label="Increase quantity" disabled={maxQuantity != null && q >= maxQuantity} onClick={() => stepQuantity(1)}>+</button>
            {quickSizes.filter((size) => maxQuantity == null || size <= maxQuantity).map((size) => <button key={size} type="button" className={`trade-button ${q === size ? "border-accent" : ""}`} aria-label={`Quantity ${size}`} onClick={() => setQuantity(String(size))}>{size}</button>)}
          </div>
        </div>
        <div className="col-span-2"><TimeInForceField value={effectiveTif} onChange={(next) => { setTif(next); if (next === "exto" || next === "gtc_exto") setStopLimitOn(true) }} market={type === "market"}
          conditional={condition !== "now"} goodTill={goodTill} setGoodTill={setGoodTill} /></div>
        {type === "limit" && <div className="trade-label col-span-2">
          <label className="trade-label">Limit price ($)<input className="trade-input" inputMode="decimal" value={limitPrice} onChange={(e) => setLimitPrice(e.target.value)} onBlur={() => setLimitPrice(limitPriceText(limitPrice))} onKeyDown={(e) => { if (e.key === "ArrowUp" || e.key === "ArrowDown") { e.preventDefault(); setLimitPrice(stepLimitPrice(root, limitPrice, e.key === "ArrowUp" ? 1 : -1)) } }} pattern="[0-9]+([.][0-9]+)?" required /></label>
          <div className="flex flex-wrap items-center gap-2">
            <button type="button" className="trade-button" aria-label="Decrease limit price one tick" onClick={() => setLimitPrice(stepLimitPrice(root, limitPrice, -1))}>−</button>
            <button type="button" className="trade-button" aria-label="Increase limit price one tick" onClick={() => setLimitPrice(stepLimitPrice(root, limitPrice, 1))}>+</button>
            <span className="text-xs text-muted">{formatMoney(limitPriceTick(root, limitPrice))} tick · {root}</span>
          </div>
        </div>}
        {walk.fields}
        {join.fields}
        {rules?.fees ? <div className="trade-label col-span-2">Fees<div className="text-foreground">Itemized · see preview below</div></div>
          : serverFee == null ? <label className="trade-label col-span-2">Fee / contract ($, estimate)<input className="trade-input" inputMode="decimal" value={fee} placeholder="Not provided by server" onChange={(e) => setFee(e.target.value)} pattern="[0-9]+([.][0-9]+)?" /></label>
          : <div className="trade-label col-span-2">Fee / contract<div className="tabular text-foreground">{formatMoney(serverFee)}</div></div>}
        {conditional && <div className="trade-label col-span-2">Condition
          <Segmented label="Condition" value={condition} onChange={setCondition}
            options={[{ value: "now", label: "Now" }, { value: "cross", label: `When ${selection.underlying} crosses` },
              { value: "watch", label: "When a price or study" }, { value: "time", label: "At a time" }]} />
          {condition === "cross" && <>
            <label className="trade-label">{selection.underlying} level
              <input className="trade-input" inputMode="decimal" value={crossLevel} placeholder={spot != null ? spot.toFixed(2) : undefined}
                onChange={(e) => setCrossLevel(e.target.value)} pattern="[0-9]+([.][0-9]+)?" required /></label>
            <span className="text-[11px] text-muted">{trigger
              ? `Arms now and activates when ${describeTrigger(trigger, side, selection.underlying)}${spot != null ? ` (now ${spot.toFixed(2)})` : ""}; ${effectiveTif === "gtd" ? "good until the chosen timestamp" : effectiveTif === "exto" ? "good through this trading date" : "good until expiry"}.`
              : `Enter the level that activates the order${spot != null ? `; ${selection.underlying} is at ${spot.toFixed(2)}` : ""}.`}</span>
          </>}
          {condition === "watch" && <>
            <div className="grid grid-cols-2 gap-2">
              <label className="trade-label">Watch
                <select className="trade-input" aria-label="Watch" value={watchKind} onChange={(e) => setWatchKind(e.target.value as "price" | TriggerStudy)}>
                  <option value="price">Price</option>
                  <option value="iv30">30-day IV (vol points)</option>
                  <option value="iv7">7-day IV (vol points)</option>
                  <option value="term_ratio">9d/30d IV ratio</option>
                </select></label>
              <label className="trade-label">Underlying
                <input className="trade-input" aria-label="Watched underlying" value={watchSymbol} placeholder={selection.underlying}
                  onChange={(e) => setWatchSymbol(e.target.value)} /></label>
            </div>
            <Segmented label="Watch direction" value={watchDirection} onChange={setWatchDirection}
              options={[{ value: "at_or_above", label: "At or above" }, { value: "at_or_below", label: "At or below" }]} />
            <label className="trade-label">Level
              <input className="trade-input" inputMode="decimal" aria-label="Watch level" value={watchLevel}
                onChange={(e) => setWatchLevel(e.target.value)} pattern="[0-9]+([.][0-9]+)?" required /></label>
            <span className="text-[11px] text-muted">{trigger
              ? `Arms now and activates when ${describeTrigger(trigger, side, selection.underlying)}; good until expiry. Needs that underlying on the feed.`
              : "Enter an underlying (empty for a study of this one) and a positive level."}</span>
          </>}
          {condition === "time" && <>
            <label className="trade-label">New York time
              <input className="trade-input" type="time" aria-label="Activation time" value={timeAt} onChange={(e) => setTimeAt(e.target.value)} required /></label>
            <span className="text-[11px] text-muted">{trigger
              ? `Arms now and activates at the first update at or after ${timeAt} in the regular session${closes ? "; it closes this quantity, so cancel it if you close the position first" : ""}.`
              : "Enter a time as HH:MM."}</span>
          </>}
        </div>}
        {conditional && <div className="col-span-2 space-y-2 rounded-md border border-border p-3">
          <label className="flex items-center justify-between gap-2 text-xs text-muted">
            <span>Protect with a stop-loss and take-profit</span>
            <input type="checkbox" role="switch" aria-label="Bracket" checked={protect} onChange={(e) => enableProtection(e.target.checked)} className="h-4 w-4 accent-[var(--accent)]" />
          </label>
          {protect && <>
            <ExitRow label="Stop loss" on={stopOn} setOn={setStopOn} source={stopSource} setSource={(next) => { setStopSource(next); setStopLevel(suggest("stop", next)) }}
              level={stopLevel} setLevel={setStopLevel} underlying={selection.underlying} hint={stopHint}
              limit={{ on: stopLimitOn, setOn: enableStopLimit, price: stopLimit, setPrice: setStopLimit }} extra={stopTrail.fields} />
            <ExitRow label="Take profit" on={targetOn} setOn={setTargetOn} source={targetSource} setSource={(next) => { setTargetSource(next); setTargetLevel(suggest("target", next)) }}
              level={targetLevel} setLevel={setTargetLevel} underlying={selection.underlying}
              hint={!exitLevel(targetLevel) ? "Enter a target." : targetSource === "option" ? `Rests as a ${formatMoney(targetLevel)} limit to ${side === "buy" ? "sell" : "buy"}.`
                : `${side === "buy" ? "Sells" : "Buys"} at market when ${describeTrigger({ source: "underlying", direction: opposite(stopDirection("underlying", side, selection.optionType)), level: targetLevel }, side, selection.underlying)}.`} />
            <p className="text-[11px] text-muted">Exits are placed as the entry fills, sized to the fill. One filling completely cancels the other; a stop that fills only in part re-arms for the rest. Both are good until expiry.</p>
          </>}
        </div>}
        <div className="col-span-2 rounded-md border border-border p-3">{chain.fields}</div>
      </fieldset>
      <LiquidityWarning modeled={!!(rules?.fill_latency_ms || rules?.impact_ticks)} impact={!!rules?.impact_ticks} market={type === "market"}
        preview={preview.data?.liquidity} ioc={type === "market" || tif === "ioc"} legs={[{ label: `${selection.strike} ${selection.optionType}`, quote, side, quantity: q }]} />
      <p role="status" className={`rounded-md border px-3 py-2 text-xs ${fill.marketable ? "border-accent/40 text-foreground" : "border-border text-muted"}`}>{rules?.inside_fill_percent && type === "limit" && condition === "now"
        ? `Inside fills enabled at ${rules.inside_fill_percent}% across the spread, bounded by displayed size. See preview for execution now.`
        : rules?.fill_latency_ms || rules?.impact_ticks
        ? "Simulated fills use the account’s latency and size impact. Displayed prices and quantities are estimates; the order may wait or fill at worse prices within its limit."
        : fill.message}</p>
      <dl className="grid grid-cols-2 gap-x-3 gap-y-2 rounded-md border border-border p-3 text-xs">
        <dt className="text-muted">Estimated premium · {side === "buy" ? "debit" : "credit"}</dt><dd className="text-right tabular">{formatMoney(estimate.premium)}</dd>
        <dt className="text-muted">Estimated fees</dt><dd className="text-right tabular"><FeeAmount fee={preview.data?.fee ?? (rules?.fees ? null : estimate.fees)} fees={preview.data?.fees} /></dd>
        {odds && <>
          <dt className="text-muted">Breakeven at expiry</dt><dd className="text-right tabular">{odds.breakeven.toFixed(2)}</dd>
          <dt className="text-muted" title="Risk-neutral mass beyond the breakeven, including the smile’s skew">Probability of profit · risk-neutral</dt>
          <dd className="text-right tabular">{odds.pop == null ? "—" : `≈ ${(odds.pop * 100).toFixed(0)}%`}{distribution && <span className="block text-[11px] text-muted">{probabilitySource(distribution, [odds.breakeven])}</span>}</dd>
        </>}
      </dl>
      {stopRequired && <p role="status" className={missingStop ? "text-sm text-warn" : "text-xs text-muted"}>Stop-loss required by this plan.{missingStop ? " Add a protective stop before submitting." : ""}</p>}
      <OrderPreviewPanel sizing={!selection.closing && !(closes && q <= Math.abs(held))} preview={preview} onSize={(size) => setQuantity(String(size))} disabled={submitted || pending}
        onWhatIf={() => draft != null && addWhatIfOrder(whatIfScope, draft, whatIfOrderText(draft))} />
      {!!(rules?.fill_latency_ms || rules?.impact_ticks) && <p className="text-xs text-muted">The preview uses current quotes. It cannot predict the later quote or the full cost of sweeping additional size blocks.</p>}
      {!!rules?.slippage_ticks && <p className="text-xs text-muted">Quoted price estimates exclude slippage; the server preview includes it.</p>}
      <details className="text-xs">
        <summary className="cursor-pointer text-muted">This order’s Greeks impact</summary>
        <p className="mb-2 mt-2 text-muted">Quantity × 100 × per-unit Greek, signed by side</p>
        <dl className="grid grid-cols-2 gap-2 tabular sm:grid-cols-4">{(["delta", "gamma", "vega", "theta"] as const).map((key) => <div key={key}><dt className="capitalize text-muted">{key}</dt><dd>{fixed(estimate[key], key === "gamma" ? 4 : 2)}</dd></div>)}</dl>
      </details>
      {(feedStatus?.provider.simulated || (source === "replay" && replay?.demo)) && <p className="text-xs text-warn">Demo market · simulated prices and volume</p>}
      <p className="text-[11px] text-muted">Estimates use the {type === "limit" ? "limit price" : "current executable quote"}. Fills and fees are determined by the server.{type === "market" ? " Market orders execute once when eligible." : ""}</p>
      {(!submitted || pending) && <button className={`w-full rounded-md px-3 py-2.5 text-sm font-medium text-background disabled:cursor-not-allowed disabled:opacity-50 ${side === "buy" ? "bg-bullish" : "bg-bearish"}`}
        type="submit" aria-label="Submit order" disabled={!valid || blocked || pending}>
        {pending ? "Submitting…" : `${trigger ? "Arm · " : ""}${side === "buy" ? "Buy" : "Sell"} ${valid ? q : ""} ${name}${type === "limit" && validMoney(limitPrice) ? ` @ ${formatMoney(limitPrice)}` : type === "market" ? " at market" : ""}${bracket?.stop_loss || bracket?.take_profit ? " · bracket" : ""}`}
      </button>}
    </form>
  </>
}

function ExitRow({ label, on, setOn, source, setSource, level, setLevel, underlying, hint, limit, extra }: {
  label: string; on: boolean; setOn: (on: boolean) => void; source: "option" | "underlying"; setSource: (source: "option" | "underlying") => void
  level: string; setLevel: (level: string) => void; underlying: string; hint: string
  /** A stop's optional limit price, which makes it a stop-limit. */
  limit?: { on: boolean; setOn: (on: boolean) => void; price: string; setPrice: (price: string) => void }
  /** More of the exit's terms, such as a stop's reference and trail. */
  extra?: ReactNode
}) {
  return <div className="space-y-1.5">
    <div className="flex flex-wrap items-center justify-between gap-2">
      <label className="flex items-center gap-2 text-xs"><input type="checkbox" checked={on} onChange={(e) => setOn(e.target.checked)} className="accent-[var(--accent)]" />{label}</label>
      {on && <Segmented label={`${label} source`} value={source} onChange={setSource} options={[{ value: "option", label: "Option" }, { value: "underlying", label: underlying }]} />}
    </div>
    {on && <>
      <label className="trade-label">{label} {source === "option" ? "price ($)" : `${underlying} level`}
        <input className="trade-input" inputMode="decimal" value={level} onChange={(e) => setLevel(e.target.value)} pattern="[0-9]+([.][0-9]+)?" /></label>
      {limit && <>
        <label className="flex items-center gap-2 text-xs"><input type="checkbox" checked={limit.on} onChange={(e) => limit.setOn(e.target.checked)} className="accent-[var(--accent)]" />Stop-limit</label>
        {limit.on && <label className="trade-label">{label} limit price ($)
          <input className="trade-input" inputMode="decimal" value={limit.price} onChange={(e) => limit.setPrice(e.target.value)} pattern="[0-9]+([.][0-9]+)?" /></label>}
      </>}
      {extra}
      <span className="text-[11px] text-muted">{hint}</span>
    </>}
  </div>
}
