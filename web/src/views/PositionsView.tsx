import { useQuery } from "@tanstack/react-query"
import { useMemo, useState, type ReactNode } from "react"
import { api } from "../api/client"
import { marketNow, useLive } from "../api/live"
import { useAccount, useAllOrders, useRefreshTrading, useTradingQueries } from "../api/trading"
import type { ClosedAttribution, Lifetime, Order, Position, Risk, StockHolding, TradingStatus } from "../api/trading-types"
import { DecisionPositionsNotice } from "../components/DecisionPositionsNotice"
import { Dialog } from "../components/Dialog"
import { KillSwitch } from "../components/KillSwitch"
import { LimitsEditor } from "../components/LimitsEditor"
import { MarginBreakdown } from "../components/MarginBreakdown"
import { FlattenDialog } from "../components/OrderActions"
import { OrderTicket } from "../components/OrderTicket"
import { PositionExitsDialog } from "../components/PositionExits"
import { AbandonDialog, CloseSharesDialog, ExerciseDialog, ExerciseInstructionDialog, SettleDialog, SharesTable, TradeSharesDialog } from "../components/StockActions"
import { CloseStrategyDialog, Strategies } from "../components/StrategyActions"
import { RiskPanel } from "../components/RiskPanel"
import { RiskProfilePanel } from "../components/RiskProfilePanel"
import { WhatIfPanel } from "../components/WhatIfPanel"
import { ScenarioGrid } from "../components/ScenarioGrid"
import { TradingError, WriteAccess, writeBlocked } from "../components/TradingControls"
import { Badge, Empty, PageHeader, Panel, Tile, toneOf, toneText } from "../components/ui"
import { fixed, signedPercent } from "../lib/format"
import { timestampET } from "../lib/freshness"
import { attributionParts, describeAttribution } from "../lib/attribution"
import { matchingPayload } from "../lib/payload"
import { contractLabel, osiLabel } from "../lib/journal"
import { strategyGroups, type StrategyGroup } from "../lib/positions"
import { closingLegs } from "../lib/strategy"
import { describeTrigger } from "../lib/ticket"
import { deliversShares, formatMoney, paperNotice, ratio, signedMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"

/** Right-aligned numeric table; the first `left` columns are labels, aligned left. */
export function Table({ label, headers, children, left = 1 }: { label: string; headers: string[]; children: ReactNode; left?: number }) {
  return <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label={label}>
    <table className="w-full text-right text-xs tabular whitespace-nowrap">
      <thead className="text-muted"><tr>{headers.map((header, i) => <th key={header} scope="col" className={`px-2 py-2 text-[11px] font-normal uppercase tracking-wide ${i < left ? "text-left" : ""}`}>{header}</th>)}</tr></thead>
      <tbody className="[&_td]:px-2 [&_td]:py-2 [&_tr]:border-t [&_tr]:border-border/40 [&_td:first-child]:text-left">{children}</tbody>
    </table>
  </div>
}

/** Fetches the position's chain so the closing ticket has a live quote. */
export function CloseTicket({ position, trading, onClose }: { position: Position; trading: TradingStatus; onClose: () => void }) {
  const { version } = useLive()
  const expiry = `${position.expiry}${position.settlement}`
  const chain = useQuery({
    queryKey: ["chain", position.underlying, expiry, 0, version(position.underlying)],
    queryFn: ({ signal }) => api.chain(position.underlying, expiry, 0, signal),
    placeholderData: (previous) => matchingPayload(previous, position.underlying, expiry),
  })
  const row = chain.data?.strikes.find((r) => r.strike === position.strike)
  const quote = row?.[position.type] ?? null
  if (!chain.data) return chain.isError ? <TradingError error={chain.error} /> : null
  const long = position.quantity > 0
  return <OrderTicket trading={trading} quote={quote} onClose={onClose} smile={chain.data.strikes} selection={{
    symbol: position.symbol, underlying: position.underlying, expiry: chain.data.expiry, strike: position.strike,
    optionType: position.type, cell: long ? "bid" : "ask", price: String((long ? quote?.bid : quote?.ask) ?? position.mark ?? ""),
    spot: chain.data.spot, closing: true, quantity: Math.abs(position.quantity),
  }} />
}

/**
 * Closes several positions as one multi-leg order, each leg reversed: the way
 * to take off a spread without uncovering a short on the way.
 */
function CloseTogether({ positions, trading, onClose }: { positions: Position[]; trading: TradingStatus; onClose: () => void }) {
  const plan = closingLegs(positions)
  if (!("legs" in plan)) return <Dialog title="Close together" onClose={onClose}><p className="text-sm text-warn">{plan.reason}</p></Dialog>
  return <CloseStrategyDialog plan={plan} underlying={positions[0]?.underlying ?? ""} trading={trading} onClose={onClose} />
}

/** Open bracket exits protecting a position, e.g. "Stop bid ≤ $3.50 · Target $5.00". */
function protection(position: Position, orders: Order[]): string | null {
  const exits = orders.filter((o) => o.symbol === position.symbol && o.role &&
    (o.status === "working" || o.status === "partially_filled" || o.status === "armed"))
  if (!exits.length) return null
  return exits.map((o) => `${o.role === "stop_loss" ? "Stop" : "Target"} ${o.trigger ? describeTrigger(o.trigger, o.side ?? "sell", o.underlying)
    + (o.limit_price != null ? `, limit ${formatMoney(o.limit_price)}` : "") : formatMoney(o.limit_price)}`).join(" · ")
}
/** The round trip's realised P&L after fees, and the contract's lifetime this attempt once it has had more than one. */
export function RealizedCell({ realised, fees, lifetime }: { realised: string; fees: string; lifetime?: Lifetime | null }) {
  const net = (Number(realised) - Number(fees)).toFixed(2)
  return <td className={toneText[toneOf(net)]} title={`This round trip: realized ${signedMoney(realised)}, fees ${formatMoney(fees)}${lifetime
    ? ` · All ${lifetime.round_trips} round trip${lifetime.round_trips === 1 ? "" : "s"} this attempt: net ${signedMoney(lifetime.net)}` : ""}`}>
    <div>{signedMoney(net)}</div>
    {lifetime && lifetime.round_trips > 1 && <div className="mt-1 text-[11px] text-muted">lifetime {signedMoney(lifetime.net)}</div>}
  </td>
}
function Positions({ positions, onClose, onExits, onExercise, onSettle, onAbandon, onInstruct, orders = [], selected, onSelect, groups = [] }: {
  positions: Position[]; onClose?: (position: Position) => void; onExercise?: (position: Position) => void
  /** Set, change or cancel a held contract's stop-loss and take-profit. */
  onExits?: (position: Position) => void
  onSettle?: (position: Position) => void; orders?: Order[]
  /** Give up a long nobody bids for; instruct or withdraw do-not-exercise on a long option. */
  onAbandon?: (position: Position) => void; onInstruct?: (position: Position) => void
  /** Held strategies, to note which positions belong to one. */
  groups?: readonly StrategyGroup[]
  /** Positions picked to close together, by symbol. */
  selected?: Set<string>; onSelect?: (symbol: string, on: boolean) => void
}) {
  if (!positions.length) return <p className="text-sm text-muted">No open positions. Click a bid or ask on the Trade page to build a ticket.</p>
  const picking = onSelect != null && selected != null
  const underlying = picking ? positions.find((p) => selected.has(p.symbol))?.underlying : undefined
  return <Table label="Positions" left={picking ? 2 : 1} headers={[...(picking ? ["Pick"] : []), "Contract", "Qty", "Avg price", "Mark / age", "Market value", "Unrealized", "P&L %", "Realized", "Today", "Dollar delta", "Vega", "Theta", ""]}>
    {positions.map((position) => {
      const change = position.unrealised != null ? ratio(position.unrealised, position.basis.replace("-", "")) : null
      return <tr key={position.symbol}>
        {picking && <td><input type="checkbox" className="accent-[var(--accent)]" aria-label={`Pick ${contractLabel(position)} to close together`}
          checked={selected.has(position.symbol)} disabled={position.awaiting_settlement || (underlying != null && position.underlying !== underlying)}
          onChange={(e) => onSelect(position.symbol, e.target.checked)} /></td>}
        <td>
          <div className="font-medium">{contractLabel(position)} <span className="text-muted">{position.settlement}</span></div>
          <div className="mt-1 text-[11px] text-faint">{position.symbol}</div>
          {position.awaiting_settlement && <span className="mt-1 inline-block rounded-full border border-warn px-2 py-0.5 text-[10px] text-warn">Awaiting settlement</span>}
          {position.awaiting_settlement && position.settle_by && <div className="mt-1 text-[11px] text-muted">
            {position.settle_by === "opening_print" ? "Waiting for opening print · AM approximation"
              : position.settle_by === "closing_print" ? "Waiting for closing print" : "Manual settlement value required"}
          </div>}
          {position.do_not_exercise && <span className="mt-1 ml-1 inline-block rounded-full border border-border px-2 py-0.5 text-[10px] text-muted">Not to be exercised</span>}
          {position.no_bid && !position.awaiting_settlement && <div className="mt-1 text-[10px] text-warn">Nobody bids for it</div>}
          {protection(position, orders) && <div className="mt-1 text-[10px] text-accent">{protection(position, orders)}</div>}
          {groups.filter((g) => g.legs.some((l) => l.position.symbol === position.symbol)).map((g) =>
            <div key={g.order.id} className="mt-1 text-[10px] text-muted">In {g.label.toLowerCase()} #{g.order.id}</div>)}
        </td>
        <td><Badge tone={position.quantity > 0 ? "positive" : "negative"}>{position.quantity > 0 ? `+${position.quantity} long` : `${position.quantity} short`}</Badge></td>
        <td>{formatMoney(position.average_price)}</td>
        <td><div>{formatMoney(position.mark)}</div><div className={`mt-1 text-[11px] ${position.fresh ? "text-muted" : "text-warn"}`}>{position.mark_age_seconds == null ? "Age unavailable" : `${fixed(position.mark_age_seconds, 0)} s old`}{!position.fresh ? " · stale" : ""}</div></td>
        <td>{formatMoney(position.market_value)}</td>
        <td className={toneText[toneOf(position.unrealised)]}>{signedMoney(position.unrealised)}</td>
        <td className={toneText[toneOf(change)]}>{signedPercent(change)}</td>
        <RealizedCell realised={position.realised} fees={position.fees} lifetime={position.lifetime} />
        <td className={toneText[toneOf(position.attribution?.total)]} title={position.attribution ? describeAttribution(position.attribution) : "Explained from this contract's next fill or rollover"}>
          {position.attribution ? signedMoney(position.attribution.total.toFixed(2)) : "—"}</td>
        <td>{fixed(position.greeks.dollar_delta, 2)}</td><td>{fixed(position.greeks.vega_dollars, 2)}</td><td>{fixed(position.greeks.theta_dollars, 2)}</td>
        <td><div className="flex justify-end gap-1.5">
          {onExercise && position.quantity > 0 && !position.awaiting_settlement && deliversShares(position.underlying) &&
            <button type="button" className="trade-button" aria-label={`Exercise ${position.symbol}`} onClick={() => onExercise(position)}>Exercise</button>}
          {onInstruct && position.quantity > 0 &&
            <button type="button" className="trade-button" aria-label={`Exercise instruction for ${position.symbol}`} onClick={() => onInstruct(position)}>
              {position.do_not_exercise ? "Exercise at expiry" : "Don't exercise"}</button>}
          {onAbandon && position.quantity > 0 && position.no_bid &&
            <button type="button" className="trade-button" aria-label={`Abandon ${position.symbol}`} onClick={() => onAbandon(position)}>Abandon</button>}
          {onClose && !position.awaiting_settlement && <button type="button" className="trade-button" aria-label={`Close ${position.symbol}`} onClick={() => onClose(position)}>Close</button>}
          {onExits && !position.awaiting_settlement && <button type="button" className="trade-button" aria-label={`Exits for ${position.symbol}`} onClick={() => onExits(position)}>Exits…</button>}
          {onSettle && position.awaiting_settlement && (position.settle_by === "manual" || position.settle_by === "opening_print") &&
            <button type="button" className="trade-button" aria-label={`Settle ${position.symbol}`} onClick={() => onSettle(position)}>Settle</button>}
        </div></td>
      </tr>
    })}
  </Table>
}

/** Joins the picked positions' round trips into one whole trade, so legs entered one by one review as one. */
export function GroupButton({ positions, trading, onDone }: { positions: Position[]; trading: TradingStatus; onDone: () => void }) {
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const [error, setError] = useState<unknown>()
  const [pending, setPending] = useState(false)
  const trades = positions.flatMap((p) => p.trade ? [p.trade] : [])
  async function group() {
    setPending(true)
    setError(undefined)
    try {
      await api.groupTrades(trades, true, trading.write)
      onDone()
    } catch (failure) {
      setError(failure)
    } finally {
      setPending(false)
      void refresh()
    }
  }
  return <>
    <button type="button" className="trade-button" title="Review the picked legs as one whole trade in the Journal"
      disabled={trades.length < 2 || trades.length !== positions.length || pending || writeBlocked(trading, token)} onClick={() => void group()}>
      {pending ? "Grouping…" : "Group as one trade"}</button>
    <TradingError error={error} />
  </>
}

/** Contracts and shares traded today and no longer held, with the day's P&L by Greek, so the rows still add up to the day. */
export function ClosedToday({ closed }: { closed: ClosedAttribution[] }) {
  return <Table label="Closed today" headers={["Contract", "Today", ...attributionParts.map((part) => part.label)]}>
    {closed.map((row) => <tr key={row.symbol}>
      <td className="font-medium">{row.kind === "shares" ? `${row.symbol} shares` : osiLabel(row.symbol, row.underlying)}</td>
      <td className={toneText[toneOf(row.attribution.total)]} title={describeAttribution(row.attribution)}>{signedMoney(row.attribution.total.toFixed(2))}</td>
      {attributionParts.map((part) => <td key={part.key}>{signedMoney(row.attribution[part.key].toFixed(2))}</td>)}
    </tr>)}
  </Table>
}

export function PositionsView() {
  const { trading, accountScope } = useLive()
  if (!trading) return null
  return <PositionsAccount key={accountScope} trading={trading} />
}

function PositionsAccount({ trading }: { trading: TradingStatus }) {
  const live = useLive()
  const { underlyings } = live
  const { portfolio, orders, risk } = useTradingQueries()
  const account = useAccount().data
  const allOrders = useAllOrders().data?.orders
  const [editing, setEditing] = useState<Risk | null>(null)
  const [closing, setClosing] = useState<Position | null>(null)
  const [exiting, setExiting] = useState<Position | null>(null)
  const [exercising, setExercising] = useState<Position | null>(null)
  const [settling, setSettling] = useState<Position | null>(null)
  const [abandoning, setAbandoning] = useState<Position | null>(null)
  const [instructing, setInstructing] = useState<Position | null>(null)
  const [closingShares, setClosingShares] = useState<StockHolding | null>(null)
  const [tradingShares, setTradingShares] = useState<{ symbol?: string } | null>(null)
  const [picked, setPicked] = useState<Set<string>>(new Set())
  const [together, setTogether] = useState(false)
  const [flatten, setFlatten] = useState<{ underlying: string | null } | null>(null)
  const refresh = useRefreshTrading()
  const data = portfolio.data
  const groups = useMemo(() => strategyGroups(data?.positions ?? [], allOrders ?? [], account?.evaluation.started ?? null),
    [data?.positions, allOrders, account?.evaluation.started])
  return <div className="min-w-0 space-y-4">
    <PageHeader title="Positions" subtitle={<>Open positions, risk and scenarios · account version {data?.account_version ?? trading.account_version}</>}>
      <WriteAccess trading={trading} />
      <button type="button" className="trade-button" onClick={() => void refresh()}>Refresh</button>
    </PageHeader>
    {underlyings.filter((u) => u.has_tradable_contracts ||
      data?.positions.some((p) => p.underlying === u.symbol && p.quantity !== 0) ||
      orders.data?.orders.some((o) => o.underlying === u.symbol && o.remaining_quantity > 0 &&
        (o.status === "working" || o.status === "partially_filled"))).map((u) => {
      const notice = paperNotice(u.symbol, u)
      return notice && <p key={u.symbol} role="status" className="text-sm text-warn">{notice}</p>
    })}
    {!trading.enabled && <p className="rounded-md border border-warn p-3 text-sm text-warn">{trading.reason ?? "Paper trading is unavailable."}</p>}
    <DecisionPositionsNotice account={account} portfolio={data} />
    <TradingError error={portfolio.error} />
    {data ? <>
      {!data.valuation_complete && <div role="status" className="rounded-md border border-warn p-3 text-sm text-warn">Valuation incomplete · equity and P&amp;L may omit unpriced positions.{data.quality_flags.length > 0 && <div className="mt-1 break-words text-xs">{data.quality_flags.join(" · ")}</div>}</div>}
      {data.valuation_complete && data.quality_flags.length > 0 && <p className="text-xs text-warn">{data.quality_flags.join(" · ")}</p>}
      <div className="grid min-w-0 grid-cols-2 gap-3 sm:grid-cols-3 xl:grid-cols-6">
        <Tile label="Open P&L" value={signedMoney(data.unrealised)} tone={toneOf(data.unrealised)} />
        <Tile label="Day P&L" value={signedMoney(data.day_pnl)} tone={toneOf(data.day_pnl)} detail={`from ${formatMoney(data.start_of_day_equity)}`} />
        <Tile label="Equity" value={formatMoney(data.equity)} />
        <Tile label="Buying power" value={formatMoney(data.buying_power?.available ?? account?.buying_power.available)}
          detail={data.buying_power && Number(data.buying_power.reserved) ? `${formatMoney(data.buying_power.reserved)} reserved` : undefined} />
        <Tile label="Cash" value={formatMoney(data.cash)} />
        <Tile label="Realized" value={signedMoney(data.realised)} tone={toneOf(data.realised)} detail={`fees ${formatMoney(data.fees)}`} />
      </div>
      <p className="text-[11px] text-muted">Valued {timestampET(data.time)}</p>
      {groups.length > 0 && <Panel title={`Strategies · ${groups.length}`}><Strategies groups={groups} trading={trading} now={marketNow(live)} /></Panel>}
      <Panel title={`Open positions · ${data.positions.length}`} actions={trading.enabled && (data.positions.length > 0 || (data.stocks?.length ?? 0) > 0) ? <>
        {data.positions.length > 1 && <>
          <span className="text-[11px] text-muted">{picked.size ? `${picked.size} picked` : "Pick positions to close them in one order"}</span>
          {picked.size > 0 && <button type="button" className="trade-button" onClick={() => setPicked(new Set())}>Clear</button>}
          <button type="button" className="trade-button" disabled={picked.size < 2} onClick={() => setTogether(true)}>Close together</button>
          <GroupButton positions={data.positions.filter((p) => picked.has(p.symbol))} trading={trading} onDone={() => setPicked(new Set())} />
        </>}
        <button type="button" className="trade-button" onClick={() => setFlatten({ underlying: null })}>Close all</button>
      </> : undefined}>
        <Positions positions={data.positions} orders={allOrders} groups={groups} onClose={trading.enabled ? setClosing : undefined}
          onExits={trading.enabled ? setExiting : undefined}
          onExercise={trading.enabled ? setExercising : undefined}
          onSettle={trading.enabled ? setSettling : undefined}
          onAbandon={trading.enabled ? setAbandoning : undefined}
          onInstruct={trading.enabled ? setInstructing : undefined}
          selected={trading.enabled && data.positions.length > 1 ? picked : undefined}
          onSelect={(symbol, on) => setPicked((current) => { const next = new Set(current); if (on) next.add(symbol); else next.delete(symbol); return next })} />
      </Panel>
      <Panel title={`Shares · ${data.stocks?.length ?? 0}`} actions={trading.enabled && underlyings.some((u) => deliversShares(u.symbol))
        ? <button type="button" className="trade-button" onClick={() => setTradingShares({})}>Trade shares</button> : undefined}>
        {(data.stocks?.length ?? 0) > 0
          ? <SharesTable stocks={data.stocks!} onClose={trading.enabled ? setClosingShares : undefined}
            onTrade={trading.enabled ? (stock) => setTradingShares({ symbol: stock.symbol }) : undefined} />
          : <p className="text-sm text-muted">No shares held. Buy or sell a stock or ETF underlying's shares to hedge delta or write covered calls.</p>}
        <p className="mt-2 text-[11px] text-muted">Bought or sold here, or delivered by exercise and assignment of equity and ETF options; marked and traded at the underlying's price.</p>
      </Panel>
      {data.margin && (data.positions.length > 0 || (data.stocks?.length ?? 0) > 0) && <Panel title={`Margin requirement · ${formatMoney(data.buying_power?.requirement ?? data.buying_power?.short_requirement)}`}>
        <MarginBreakdown margin={data.margin} />
        <p className="mt-2 text-[11px] text-muted">{account?.rules.margin === "portfolio"
          ? "Portfolio margin: each underlying holds its largest scanned loss, taken from equity."
          : "Strategy margin: each short pairs with what covers it to hold the least in total; hover a row for its rule."}</p>
      </Panel>}
      {(data.closed?.length ?? 0) > 0 && <Panel title={`Closed today · ${data.closed!.length}`}>
        <ClosedToday closed={data.closed!} />
      </Panel>}
    </> : trading.enabled && !portfolio.error ? <Empty>Loading positions…</Empty> : null}
    <Panel title="Portfolio risk" actions={<button type="button" className="trade-button" disabled={!risk.data || !trading.enabled} onClick={() => { if (risk.data) setEditing(risk.data) }}>Edit limits</button>}>
      <TradingError error={risk.error} />{risk.data ? <RiskPanel risk={risk.data} /> : <p className="text-sm text-muted">{trading.enabled ? "Loading risk…" : "Risk unavailable"}</p>}
    </Panel>
    {data && trading.enabled && <Panel title="Risk profile"><RiskProfilePanel underlyings={[...new Set([
      ...data.positions.filter((p) => !p.awaiting_settlement).map((p) => p.underlying), ...(data.stocks ?? []).map((s) => s.symbol)])].sort()} /></Panel>}
    {risk.data && <Panel title="Spot × volatility scenarios"><ScenarioGrid scenarios={risk.data.scenarios} /></Panel>}
    {trading.enabled && <Panel title="What-if"><WhatIfPanel positions={data?.positions ?? []}
      picked={data?.positions.filter((p) => picked.has(p.symbol)) ?? []} trading={trading} /></Panel>}
    <Panel title="Kill switch"><KillSwitch kill={risk.data?.kill ?? { latched: trading.kill_latched, reason: null }} trading={trading} /></Panel>
    {editing && <LimitsEditor initial={editing} trading={trading} onClose={() => setEditing(null)} />}
    {closing && <CloseTicket position={closing} trading={trading} onClose={() => setClosing(null)} />}
    {exiting && <PositionExitsDialog position={exiting} trading={trading} onClose={() => setExiting(null)} />}
    {exercising && <ExerciseDialog position={exercising} trading={trading} onClose={() => setExercising(null)} />}
    {settling && <SettleDialog position={settling} trading={trading} onClose={() => setSettling(null)} />}
    {abandoning && <AbandonDialog position={abandoning} trading={trading} onClose={() => setAbandoning(null)} />}
    {instructing && <ExerciseInstructionDialog position={instructing} trading={trading} onClose={() => setInstructing(null)} />}
    {closingShares && <CloseSharesDialog stock={closingShares} trading={trading} onClose={() => setClosingShares(null)} />}
    {tradingShares && <TradeSharesDialog initial={tradingShares.symbol} trading={trading} onClose={() => setTradingShares(null)} />}
    {flatten && data && <FlattenDialog positions={data.positions} stocks={data.stocks ?? []} orders={allOrders ?? []} trading={trading}
      initial={flatten.underlying} onClose={() => setFlatten(null)} />}
    {together && data && <CloseTogether positions={data.positions.filter((p) => picked.has(p.symbol))} trading={trading}
      onClose={() => { setTogether(false); setPicked(new Set()) }} />}
  </div>
}
