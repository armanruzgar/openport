import { useActionApi } from "../api/action-client"
import { useState } from "react"
import { useQuery } from "@tanstack/react-query"
import { useWriteToken } from "../lib/write-token"
import { flatRuleNotice, planEntryNotice, planMarketTime } from "../lib/plan-rules"
import { useAccount, usePortfolio } from "../api/trading"

import { useLive } from "../api/live"
import type { Position, Side, StockHolding, TradingStatus } from "../api/trading-types"
import { describeAttribution } from "../lib/attribution"
import { fixed, isNum } from "../lib/format"
import { contractLabel } from "../lib/journal"
import { deliversShares, formatMoney, signedMoney } from "../lib/trading"
import { Dialog } from "./Dialog"
import { useWrite } from "./OrderActions"
import { TradingError, WriteAccess } from "./TradingControls"
import { Badge, Segmented, toneOf, toneText } from "./ui"

const dollars = (value: number) => formatMoney(value.toFixed(2))

/** An expired position's settlement value entered by hand: an AM series' opening
 * quotation, or a PM series' official close when no closing print arrived. */
export function SettleDialog({ position, trading, onClose }: { position: Position; trading: TradingStatus; onClose: () => void }) {
  const api = useActionApi()
  const write = useWrite(trading)
  const [value, setValue] = useState("")
  const reference = Number(value)
  const valid = /^\d+(\.\d{1,6})?$/.test(value.trim()) && reference > 0
  const call = position.type === "call"
  const intrinsic = valid ? position.do_not_exercise ? 0 : Math.max(0, call ? reference - position.strike : position.strike - reference) : null
  const am = position.settlement === "AM"
  return <Dialog title={`Settle ${contractLabel(position)}`} onClose={onClose}>
    <p className="text-sm">{am
      ? `AM-settled series settle on ${position.underlying}'s special opening quotation on the expiry date, which no feed here provides.`
      : `No closing print for ${position.underlying} arrived after this contract expired. Half an hour after the close it settles on the last print before it, if that came in the close's last five minutes.`} Enter the official
      {am ? " settlement value" : " closing value"} from the exchange; {position.do_not_exercise ? "the do-not-exercise instruction closes this long at zero, without shares or cash proceeds" : "the position closes at intrinsic value"}.</p>
    {position.settle_by === "opening_print" && <p className="text-sm text-muted">The simulator is waiting for an opening-print approximation. Importing a value now settles before that print arrives.</p>}
    <label className="trade-label">{position.underlying} settlement value
      <input className="trade-input" inputMode="decimal" value={value} onChange={(e) => setValue(e.target.value)} placeholder="Official value" /></label>
    {intrinsic != null && <p className="text-sm text-muted">{position.do_not_exercise ? "Settlement" : "Intrinsic"} value {dollars(intrinsic)} a share, {signedMoney((intrinsic * 100 * position.quantity).toFixed(2))} for
      {` ${Math.abs(position.quantity)} ${position.quantity < 0 ? "short " : ""}`}contract{Math.abs(position.quantity) === 1 ? "" : "s"}{deliversShares(position.underlying) && intrinsic >= 0.01 ? `, delivering ${Math.abs(position.quantity) * 100} ${position.underlying} shares` : ""}.</p>}
    <WriteAccess trading={trading} />
    <TradingError error={write.error} />
    <button type="button" className="trade-button" disabled={!valid || write.pending || write.blocked}
      onClick={() => void write.run(() => api.settle(position.symbol, value.trim(), trading.write), onClose)}>
      {write.pending ? "Settling…" : "Settle"}</button>
  </Dialog>
}

/** Shares held, bought here or delivered by exercise and assignment, each tradable at the underlying's price. */
export function SharesTable({ stocks, onClose, onTrade }: {
  stocks: StockHolding[]; onClose?: (stock: StockHolding) => void; onTrade?: (stock: StockHolding) => void
}) {
  return <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Shares">
    <table className="w-full text-right text-xs tabular whitespace-nowrap">
      <thead className="text-muted"><tr>{["Stock", "Shares", "Avg price", "Price / age", "Market value", "Unrealized", "Today", ""].map((h, i) =>
        <th key={h} scope="col" className={`px-2 py-2 text-[11px] font-normal uppercase tracking-wide ${i === 0 ? "text-left" : ""}`}>{h}</th>)}</tr></thead>
      <tbody className="[&_td]:px-2 [&_td]:py-2 [&_tr]:border-t [&_tr]:border-border/40 [&_td:first-child]:text-left">
        {stocks.map((stock) => <tr key={stock.symbol}>
          <td className="font-medium">{stock.symbol}</td>
          <td><Badge tone={stock.shares > 0 ? "positive" : "negative"}>{stock.shares > 0 ? `+${stock.shares} long` : `${stock.shares} short`}</Badge></td>
          <td>{formatMoney(stock.average_price)}</td>
          <td><div>{formatMoney(stock.mark)}</div>
            <div className={`mt-1 text-[11px] ${stock.fresh ? "text-muted" : "text-warn"}`}>{stock.mark_time ? stock.fresh ? "current" : "stale" : "no price yet"}</div></td>
          <td>{formatMoney(stock.market_value)}</td>
          <td className={toneText[toneOf(stock.unrealised)]}>{signedMoney(stock.unrealised)}</td>
          <td className={toneText[toneOf(stock.attribution?.total)]} title={stock.attribution ? describeAttribution(stock.attribution) : undefined}>
            {stock.attribution ? signedMoney(stock.attribution.total.toFixed(2)) : "—"}</td>
          <td><div className="flex justify-end gap-1">
            {onTrade && <button type="button" className="trade-button" aria-label={`Trade ${stock.symbol} shares`} onClick={() => onTrade(stock)}>Trade</button>}
            {onClose && <button type="button" className="trade-button" aria-label={`Close ${stock.symbol} shares`} onClick={() => onClose(stock)}>Close</button>}
          </div></td>
        </tr>)}
      </tbody>
    </table>
  </div>
}

/** What a share trade does to the shares held: "opens", "adds to", "reduces", "closes" or "reverses". */
export function shareEffect(held: number, side: Side, shares: number): "opens" | "adds to" | "reduces" | "closes" | "reverses" {
  const signed = side === "buy" ? shares : -shares
  if (held === 0) return "opens"
  if (Math.sign(held) === Math.sign(signed)) return "adds to"
  const after = held + signed
  return after === 0 ? "closes" : Math.sign(after) === Math.sign(held) ? "reduces" : "reverses"
}

/**
 * Buy or sell an equity or ETF underlying's shares at its price, without a fee: a delta
 * hedge, or the shares behind a covered call or a collar. Opening takes the account's
 * checks; reducing works under the kill switch.
 */
export function TradeSharesDialog({ initial, trading, onClose }: { initial?: string; trading: TradingStatus; onClose: () => void }) {
  const api = useActionApi()
  const write = useWrite(trading)
  const { underlyings, accountScope, source, replay } = useLive()
  const token = useWriteToken()
  const stocks = usePortfolio().data?.stocks ?? []
  const account = useAccount().data
  const rules = account?.rules
  const symbols = [...new Set([...underlyings.map((u) => u.symbol), ...stocks.map((s) => s.symbol)])].filter(deliversShares).sort()
  const [symbol, setSymbol] = useState(initial ?? symbols[0] ?? "")
  const [side, setSide] = useState<Side>("buy")
  const [shares, setShares] = useState("100")
  const n = Number(shares)
  const valid = symbols.includes(symbol) && /^\d+$/.test(shares) && Number.isSafeInteger(n) && n > 0 && n <= 10_000_000
  const held = stocks.find((s) => s.symbol === symbol)?.shares ?? 0
  const spot = underlyings.find((u) => u.symbol === symbol)?.spot
  const effect = valid ? shareEffect(held, side, n) : null
  const reduces = effect === "reduces" || effect === "closes"
  const shortSale = side === "sell" && valid && held - n < 0
  const decided = account?.evaluation.enabled && account.evaluation.status !== "active"
  const planNotice = planEntryNotice(rules, symbol, planMarketTime(account?.time, underlyings.find((u) => u.symbol === symbol)?.as_of, source !== "live" ? replay?.time : null), reduces, account)
  const refused = planNotice ?? (decided && !reduces ? "The evaluation is decided. Only trades that reduce shares toward zero are allowed; opening requires a new attempt."
    : trading.kill_latched && !reduces ? "Kill switch latched · reduce-only: only trades that reduce shares toward zero are allowed."
    : rules?.require_stop_loss && !reduces ? "Stop-loss required by this plan. Share entries cannot attach a protective stop."
    : rules?.buy_only && shortSale ? `${rules.plan ?? "This plan"} is buy-only: share sales may only close shares you hold.`
    : rules?.defined_risk && shortSale ? "This plan allows defined risk only, and short shares can lose without limit."
    : null)
  const preview = useQuery({
    queryKey: ["trading", accountScope, "stock-preview", trading.account_version, symbol, side, n, token],
    queryFn: () => api.previewStock(symbol, side, n, trading.write),
    enabled: valid && trading.enabled && !write.blocked,
    retry: false,
  })
  return <Dialog title="Trade shares" onClose={onClose}>
    {symbols.length === 0 ? <p className="text-sm text-muted">No stock or ETF underlying is in the feed.</p> : <>
      <label className="trade-label">Stock or ETF
        <select className="trade-input" value={symbol} onChange={(e) => setSymbol(e.target.value)}>
          {symbols.map((s) => <option key={s} value={s}>{s}</option>)}</select></label>
      <div className="trade-label">Side
        <Segmented label="Side" value={side} onChange={setSide} options={[{ value: "buy", label: "Buy" }, { value: "sell", label: "Sell" }]} /></div>
      <label className="trade-label">Shares
        <input className="trade-input" type="number" min="1" step="1" value={shares} onChange={(e) => setShares(e.target.value)} /></label>
      <p className="text-sm">{held !== 0 ? `You hold ${held > 0 ? `${held} long` : `${-held} short`}. ` : ""}
        {effect ? `${side === "buy" ? "Buying" : "Selling"} ${n} ${effect} the position` : "Enter a whole number of shares"}
        {isNum(spot) && valid ? `, about ${dollars(spot * n)} at ${symbol}'s price of ${fixed(spot, 2)}` : ""}, without a fee.</p>
      <p className="text-xs text-muted">Shares trade at the underlying's price in the stock market's regular session. Long shares are paid for in
        full under strategy margin, where unprotected short shares hold 150% of their value. Portfolio margin scans the book. Opening takes the account's checks, including exposure limits.</p>
      <section aria-label="Share preview" className="rounded-md border border-border p-3 text-xs space-y-1">
        <p>Simulated share preview</p>
        {preview.isFetching ? <p>Checking shares…</p> : preview.data ? <>
          <p>Cash cost (negative receives): {formatMoney(preview.data.cost)}</p>
          <p>Buying power: {formatMoney(preview.data.current.buying_power)} → {formatMoney(preview.data.after.buying_power)}</p>
          <p>Dollar delta: {fixed(preview.data.current.exposure?.dollar_delta, 2)} → {fixed(preview.data.after.exposure?.dollar_delta, 2)}</p>
          <p>Share delta change: {side === "buy" ? "+" : "−"}{valid ? n : "…"}</p>
          {preview.data.reason && <p role="status" className="text-warn">{preview.data.reason.code}: {preview.data.reason.message}</p>}
        </> : <p>{preview.error ? "Preview unavailable; submitting still takes server checks." : "Enter shares to preview."}</p>}
      </section>
      {!planNotice && flatRuleNotice(rules) && <p className="text-xs text-muted">{flatRuleNotice(rules)}</p>}
      {refused && <p role="status" className="text-sm text-warn">{refused}</p>}
      <WriteAccess trading={trading} />
      <TradingError error={write.error} />
      <button type="button" className="trade-button" disabled={!valid || !!refused || write.pending || write.blocked}
        onClick={() => void write.run(() => api.tradeStock(symbol, side, n, trading.write), onClose)}>
        {write.pending ? "Trading…" : `${side === "buy" ? "Buy" : "Sell"} ${valid ? n : ""} ${symbol}`}</button>
    </>}
  </Dialog>
}

/** Close some or all shares at the underlying's price. */
export function CloseSharesDialog({ stock, trading, onClose }: { stock: StockHolding; trading: TradingStatus; onClose: () => void }) {
  const api = useActionApi()
  const write = useWrite(trading)
  const held = Math.abs(stock.shares)
  const [shares, setShares] = useState(String(held))
  const n = Number(shares)
  const valid = Number.isSafeInteger(n) && n > 0 && n <= held
  const verb = stock.shares > 0 ? "Sell" : "Buy back"
  return <Dialog title={`Close ${stock.symbol} shares`} onClose={onClose}>
    <label className="trade-label">Shares
      <input className="trade-input" type="number" min="1" max={held} step="1" value={shares} onChange={(e) => setShares(e.target.value)} /></label>
    <p className="text-sm">{verb} {valid ? n : "…"} {stock.symbol} at the underlying's price{stock.mark ? `, now ${formatMoney(stock.mark)}` : ""}, without a fee.
      Shares trade in the regular session.</p>
    <WriteAccess trading={trading} />
    <TradingError error={write.error} />
    <button type="button" className="trade-button" disabled={!valid || write.pending || write.blocked}
      onClick={() => void write.run(() => api.closeStock(stock.symbol, n === held ? null : n, trading.write), onClose)}>
      {write.pending ? "Closing…" : `${verb} ${valid ? n : ""} shares`}</button>
  </Dialog>
}

/**
 * Give up a long nobody bids for (or one that expired and waits for its settlement):
 * it leaves the account at zero, without a fee.
 */
export function AbandonDialog({ position, trading, onClose }: { position: Position; trading: TradingStatus; onClose: () => void }) {
  const api = useActionApi()
  const write = useWrite(trading)
  const { underlyings } = useLive()
  const spot = underlyings.find((u) => u.symbol === position.underlying)?.spot
  const call = position.type === "call"
  const intrinsic = isNum(spot) ? Math.max(0, call ? spot - position.strike : position.strike - spot) : null
  return <Dialog title={`Abandon ${contractLabel(position)}`} onClose={onClose}>
    <p className="text-sm">{position.quantity} contract{position.quantity === 1 ? "" : "s"} leave the account at $0.00, without a fee,
      {position.awaiting_settlement ? " instead of waiting for the settlement." : " since nobody bids for them."} The loss of
      {` ${formatMoney(position.basis)}`} paid is realised now.</p>
    {position.awaiting_settlement && intrinsic != null && intrinsic >= 0.01 && <p role="status" className="text-sm text-warn">
      With {position.underlying} at {fixed(spot, 2)} it is {dollars(intrinsic)} in the money: the settlement may still pay
      {` ${dollars(intrinsic * 100 * position.quantity)}`}.</p>}
    <WriteAccess trading={trading} />
    <TradingError error={write.error} />
    <button type="button" className="trade-button" disabled={write.pending || write.blocked}
      onClick={() => void write.run(() => api.abandon(position.symbol, trading.write), onClose)}>
      {write.pending ? "Abandoning…" : `Abandon ${position.quantity}`}</button>
  </Dialog>
}

/**
 * Instruct that a long option held into expiry is not exercised, or
 * withdraw that instruction: it then expires worthless without shares or cash proceeds.
 */
export function ExerciseInstructionDialog({ position, trading, onClose }: { position: Position; trading: TradingStatus; onClose: () => void }) {
  const api = useActionApi()
  const write = useWrite(trading)
  const instructed = position.do_not_exercise === true
  const shares = position.quantity * 100
  const physical = deliversShares(position.underlying)
  return <Dialog title={`${instructed ? "Exercise" : "Do not exercise"} ${contractLabel(position)}`} onClose={onClose}>
    <p className="text-sm">{instructed
      ? !physical ? "Withdraw the instruction: held into expiry in the money, this long settles in cash at intrinsic value." : `Withdraw the instruction: held into expiry a cent or more in the money, the ${position.quantity} contract${position.quantity === 1 ? " is" : "s are"} exercised and ${position.type === "call" ? "buy" : "sell"} ${shares} ${position.underlying} shares.`
      : `Held into expiry, the ${position.quantity} contract${position.quantity === 1 ? "" : "s"} expire worthless however far in the money, and ${physical ? `no ${position.underlying} shares change hands` : "there are no cash proceeds"}. Selling them first keeps any value.`}</p>
    <p className="text-xs text-muted">The instruction ends with the position.</p>
    <WriteAccess trading={trading} />
    <TradingError error={write.error} />
    <button type="button" className="trade-button" disabled={write.pending || write.blocked}
      onClick={() => void write.run(() => api.exerciseInstruction(position.symbol, !instructed, trading.write), onClose)}>
      {write.pending ? "Saving…" : instructed ? "Exercise at expiry" : "Do not exercise"}</button>
  </Dialog>
}

/** Exercise long equity or ETF options into shares at the underlying's price. */
export function ExerciseDialog({ position, trading, onClose }: { position: Position; trading: TradingStatus; onClose: () => void }) {
  const api = useActionApi()
  const write = useWrite(trading)
  const { underlyings } = useLive()
  const spot = underlyings.find((u) => u.symbol === position.underlying)?.spot
  const [contracts, setContracts] = useState(String(position.quantity))
  const n = Number(contracts)
  const valid = Number.isSafeInteger(n) && n > 0 && n <= position.quantity
  const call = position.type === "call"
  const intrinsic = isNum(spot) ? Math.max(0, call ? spot - position.strike : position.strike - spot) : null
  const mark = position.mark != null ? Number(position.mark) : null
  const timeValue = intrinsic != null && mark != null ? Math.max(0, mark - intrinsic) : null
  return <Dialog title={`Exercise ${contractLabel(position)}`} onClose={onClose}>
    <label className="trade-label">Contracts
      <input className="trade-input" type="number" min="1" max={position.quantity} step="1" value={contracts} onChange={(e) => setContracts(e.target.value)} /></label>
    <p className="text-sm">{valid ? n : "…"} contract{n === 1 ? "" : "s"} close at intrinsic value{intrinsic != null ? `, ${dollars(intrinsic)} with ${position.underlying} at ${fixed(spot, 2)}` : ""},
      and {call ? "buy" : "sell"} {valid ? n * 100 : "…"} {position.underlying} shares at that price: together, the {formatMoney(String(position.strike))} strike.</p>
    {intrinsic === 0 && <p role="status" className="text-sm text-warn">The option is out of the money, so exercising it would lose value; the server refuses it.</p>}
    {timeValue != null && timeValue > 0 && <p className="text-xs text-muted">Exercising gives up about {dollars(timeValue * (valid ? n : 1) * 100)} of time value
      against the {formatMoney(position.mark)} mark; selling the option keeps it.</p>}
    <WriteAccess trading={trading} />
    <TradingError error={write.error} />
    <button type="button" className="trade-button" disabled={!valid || write.pending || write.blocked}
      onClick={() => void write.run(() => api.exercise(position.symbol, n, trading.write), onClose)}>
      {write.pending ? "Exercising…" : `Exercise ${valid ? n : ""}`}</button>
  </Dialog>
}
