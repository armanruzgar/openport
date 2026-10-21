import { useRef, useState } from "react"
import { useActionApi } from "../api/action-client"
import { useRefreshTrading, useTradingSession } from "../api/trading"
import type { Account, TradingStatus } from "../api/trading-types"
import { formatMoney, signedMoney } from "../lib/trading"
import { timestampET } from "../lib/freshness"
import { useWriteToken } from "../lib/write-token"
import { TradingError, writeBlocked } from "./TradingControls"
import { Panel } from "./ui"

export function CashFlows({ account, trading }: { account: Account; trading: TradingStatus }) {
  const api = useActionApi()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const token = useWriteToken()
  const [amount, setAmount] = useState("")
  const [direction, setDirection] = useState("deposit")
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<unknown>()
  const retry = useRef<{ amount: string; id: string } | undefined>(undefined)
  const busy = useRef(false)
  const valid = /^(?:0|[1-9][0-9]*)(?:\.[0-9]{1,2})?$/.test(amount) && Number(amount) > 0
  async function submit() {
    if (!valid || busy.current || writeBlocked(trading, token)) return
    const signed = direction === "withdrawal" ? `-${amount}` : amount
    if (retry.current?.amount !== signed) retry.current = { amount: signed, id: crypto.randomUUID() }
    busy.current = true; setPending(true); setError(undefined)
    try {
      await api.transferCash(retry.current.id, signed, trading.write)
      if (sameSession()) { setAmount(""); retry.current = undefined }
    } catch (failure) { if (sameSession()) setError(failure) }
    finally { busy.current = false; if (sameSession()) setPending(false); void refresh() }
  }
  const flows = (account.cash_flows ?? []).filter(flow => flow.attempt === account.evaluation.attempt).slice(-10).reverse()
  return <Panel title="Cash flows">
    {account.cash_transfers_allowed ? <form className="flex flex-wrap items-end gap-2" onSubmit={event => { event.preventDefault(); void submit() }}>
      <label className="text-sm">Transfer<select className="trade-input block" value={direction} disabled={pending}
        onChange={event => setDirection(event.target.value)}><option value="deposit">Deposit</option><option value="withdrawal">Withdraw</option></select></label>
      <label className="text-sm">Amount ($)<input className="trade-input block" inputMode="decimal" value={amount} disabled={pending}
        onChange={event => setAmount(event.target.value)} /></label>
      <button className="trade-button" type="submit" disabled={!valid || pending || writeBlocked(trading, token)}>{pending ? "Posting…" : "Post transfer"}</button>
      <p className="w-full text-xs text-muted">Practice paper cash only. Transfers do not count as profit or loss. Withdrawals must leave positive equity and fit unreserved buying power.</p>
    </form> : <p className="text-xs text-muted">Deposits and withdrawals are available on practice accounts. Evaluation capital follows the plan; funded accounts use payouts.</p>}
    <TradingError error={error} />
    <p className="mt-2 text-xs text-muted">Annual rates: cash credit {(account.rules.cash_credit_rate_bps ?? 0) / 100}%, margin debit {(account.rules.margin_debit_rate_bps ?? 0) / 100}%, stock borrow {(account.rules.stock_borrow_rate_bps ?? 0) / 100}%. Financing posts at rollover as other P&amp;L, counts in equity and daily loss, and leaves the daily-loss base unchanged.</p>
    {flows.length > 0 && <ul className="mt-2 space-y-1 text-sm">{flows.map(flow => <li key={flow.id}>
      {timestampET(flow.time)} · {flow.kind.replaceAll("_", " ")}{flow.symbol ? ` · ${flow.symbol}` : ""} · {signedMoney(flow.amount)}
      {flow.days > 0 ? ` (${flow.days} days on ${formatMoney(flow.basis)} at ${flow.rate_bps / 100}%)` : ""}
    </li>)}</ul>}
  </Panel>
}
