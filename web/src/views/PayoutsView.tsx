import { useActionApi } from "../api/action-client"
import { useId, useRef, useState } from "react"

import { useLive } from "../api/live"
import { useAccount, usePlans, useRefreshTrading, useTradingSession } from "../api/trading"
import type { Account, SizeScalingStatus, Payout, PayoutStatus, Plan, TradingStatus } from "../api/trading-types"
import { ResetDialog } from "../components/ResetDialog"
import { evaluationBadge } from "../components/Sidebar"
import { TradingError, WriteAccess, writeBlocked } from "../components/TradingControls"
import { Badge, Check, Empty, PageHeader, Panel, Tile, toneOf, toneText } from "../components/ui"
import { timestampET } from "../lib/freshness"
import { cycleDays, lockReason, offeredPlans, payoutAmountError, payoutBestDayShare, payoutCap, payoutChecks, unlockedNextPlan } from "../lib/payouts"
import { useRoute } from "../lib/route"
import { formatMoney, percentOfMoney, signedMoney, sumMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"

const dayFormat = new Intl.DateTimeFormat("en-US", { weekday: "short", month: "short", day: "numeric", timeZone: "UTC" })

export function PayoutsView() {
  const { trading, accountScope } = useLive()
  if (!trading) return null
  return <Payouts key={accountScope} trading={trading} />
}

function Payouts({ trading }: { trading: TradingStatus }) {
  const account = useAccount()
  const plans = usePlans()
  const [start, setStart] = useState<string | null>(null)
  const data = account.data
  if (account.error) return <TradingError error={account.error} />
  if (!data) return <Empty>Loading payouts…</Empty>
  const list = offeredPlans(plans.data?.plans ?? [], data)
  return (
    <div className="min-w-0 space-y-4">
      <PageHeader title={<span className="flex flex-wrap items-center gap-2">Payouts {evaluationBadge(data, data.rules.plan)}</span>}
        subtitle={data.payout ? <>{data.rules.plan} · payout cycle since {timestampET(data.evaluation.cycle_started)}</>
          : "Withdraw profits from a funded account"} />
      {data.payout ? <Funded trading={trading} account={data} status={data.payout} />
        : <NotFunded trading={trading} account={data} plans={list} onStart={setStart} />}
      {data.evaluation.payouts.length > 0 && <History payouts={data.evaluation.payouts} split={data.rules.payouts?.split_percent} />}
      {start && <ResetDialog trading={trading} attempt={data.evaluation.attempt} initial={start} onClose={() => setStart(null)} />}
    </div>
  )
}

function Funded({ trading, account, status }: { trading: TradingStatus; account: Account; status: PayoutStatus }) {
  const e = account.evaluation
  const rules = account.rules.payouts
  const paid = sumMoney(e.payouts.map((p) => p.trader_share))
  const withdrawn = sumMoney(e.payouts.map((p) => p.amount))
  const days = cycleDays(account)
  return <>
    <div className="grid min-w-0 grid-cols-2 gap-3 lg:grid-cols-4">
      <Tile label="Available now" value={status.eligible ? formatMoney(status.maximum) : "—"} tone={status.eligible ? "positive" : "neutral"}
        detail={status.eligible ? `${formatMoney(status.trader_share)} to you (${status.split_percent}%)` : status.blocked?.message} />
      <Tile label="Profit" value={signedMoney(status.profit)} tone={toneOf(status.profit)}
        detail={`${status.withdrawal_percent}% withdrawable: ${formatMoney(status.withdrawable)}`} />
      <Tile label="Qualifying days" value={`${status.qualifying_days} / ${status.required_days}`}
        detail={`Days of ${formatMoney(status.qualifying_profit, 0)}+ net realised`}
        meter={{ value: status.qualifying_days / status.required_days, tone: "positive", label: "Qualifying days toward the next payout" }} />
      <Tile label="Paid to you" value={formatMoney(paid)}
        detail={e.payouts.length ? `${e.payouts.length} payout${e.payouts.length === 1 ? "" : "s"} · ${formatMoney(withdrawn)} withdrawn` : "No payouts yet"} />
    </div>
    {e.size_scaling && <SizeScalingProgress status={e.size_scaling} />}
    {(status.consistency_percent != null || status.buffer_balance != null) && <Panel title="Payout standing">
      <dl className="grid gap-3 text-sm sm:grid-cols-2">
        {status.consistency_percent != null && <>
          <div><dt className="text-muted">Cycle net realised profit</dt><dd>{signedMoney(status.cycle_profit)}</dd></div>
          <div><dt className="text-muted">Best day</dt><dd>{status.best_day ? `${formatMoney(status.best_day.profit)} on ${status.best_day.day}` : "No positive day yet"}</dd></div>
          <div><dt className="text-muted">Best day's share</dt><dd>{payoutBestDayShare(status)} · limit {status.consistency_percent}%</dd></div>
          <div><dt className="text-muted">Profit still needed</dt><dd>{formatMoney(status.consistency_needed)}</dd></div>
        </>}
        {status.buffer_balance != null && <div><dt className="text-muted">Payout buffer balance</dt><dd>{formatMoney(status.buffer_balance)} must remain after withdrawal</dd></div>}
      </dl>
      {status.consistency_percent != null && <p className="mt-3 text-xs text-muted">Includes the day in progress and losing days. Profit needed assumes the best day stays the same; cycle profit must be positive.</p>}
    </Panel>}
    <div className="grid min-w-0 gap-3 lg:grid-cols-[minmax(0,1fr)_minmax(0,1fr)]">
      <Panel title={`Request payout #${status.number}`}>
        <ul className="space-y-1.5 text-sm">
          {payoutChecks(status).map((check) => <Check key={check.label} ok={check.ok} label={check.label} value={check.value} />)}
        </ul>
        <RequestForm trading={trading} status={status} />
      </Panel>
      <Panel title="This cycle" actions={<span className="text-[11px] text-muted">days count at each close</span>}>
        {days.length ? (
          <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Days in this payout cycle">
            <table className="w-full text-left text-xs whitespace-nowrap">
              <thead className="text-[11px] uppercase tracking-wide text-muted"><tr>
                {["Day", "Net realised", "Close equity", ""].map((h) => <th key={h} className="px-2 py-1.5 font-normal">{h}</th>)}
              </tr></thead>
              <tbody>{[...days].reverse().map((d) => (
                <tr key={d.day} className="border-t border-border/40">
                  <td className="px-2 py-1.5">{dayFormat.format(Date.parse(`${d.day}T12:00:00Z`))}</td>
                  <td className={`px-2 py-1.5 tabular ${toneText[toneOf(d.realised)]}`}>{signedMoney(d.realised)}</td>
                  <td className="px-2 py-1.5 tabular">{formatMoney(d.close_equity)}</td>
                  <td className="px-2 py-1.5 text-right">{d.qualifying ? <Badge tone="positive">qualifying</Badge> : <span className="text-faint">—</span>}</td>
                </tr>
              ))}</tbody>
            </table>
          </div>
        ) : <p className="text-sm text-muted">No finished trading days in this cycle yet. A day qualifies when it ends with at least {formatMoney(status.qualifying_profit)} of net realised profit.</p>}
      </Panel>
    </div>
    {rules && <Panel title="Payout rules">
      <dl className="grid grid-cols-2 gap-x-4 gap-y-3 text-xs sm:grid-cols-4">
        {([
          ["Qualifying day", `${formatMoney(rules.qualifying_profit)}+ net realised`],
          ["Days per payout", String(rules.qualifying_days)],
          ["Per payout", `Up to ${rules.withdrawal_percent}% of profit`],
          ["Your share", `${rules.split_percent}%`],
          ["Minimum", formatMoney(rules.minimum)],
          ["Cap this payout", formatMoney(payoutCap(rules.caps, status.number)) === "—" ? "None" : formatMoney(payoutCap(rules.caps, status.number))],
          ["Caps by payout", rules.caps.length ? rules.caps.map((cap, i) => `#${i + 1}${i === rules.caps.length - 1 ? "+" : ""} ${formatMoney(cap, 0)}`).join(" · ") : "None"],
          ["Drawdown floor", e.floor_locked ? `Locked at ${formatMoney(e.floor)}` : account.rules.lock_balance ? `${formatMoney(e.floor)}, locks at ${formatMoney(account.rules.lock_balance, 0)}` : formatMoney(e.floor)],
        ] as const).map(([label, value]) => (
          <div key={label} className="min-w-0">
            <dt className="text-[10px] uppercase tracking-wide text-muted">{label}</dt>
            <dd className="mt-0.5 tabular">{value}</dd>
          </div>
        ))}
      </dl>
    </Panel>}
  </>
}

function RequestForm({ trading, status }: { trading: TradingStatus; status: PayoutStatus }) {
  const api = useActionApi()
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const [amount, setAmount] = useState(status.eligible ? status.maximum : "")
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<unknown>()
  const [done, setDone] = useState<Payout | null>(null)
  const busy = useRef(false)
  const id = useId()
  const invalid = amount.trim() ? payoutAmountError(amount, status) : null
  const blocked = writeBlocked(trading, token)
  async function submit() {
    if (!status.eligible || invalid || !amount.trim() || busy.current || blocked) return
    busy.current = true
    setPending(true)
    setError(undefined)
    try {
      const account = await api.requestPayout(amount.trim(), trading.write)
      if (sameSession()) {
        setDone(account.evaluation.payouts[account.evaluation.payouts.length - 1] ?? null)
        setAmount("")
      }
    } catch (failure) {
      if (sameSession()) setError(failure)
    } finally {
      busy.current = false
      if (sameSession()) setPending(false)
      void refresh()
    }
  }
  return (
    <form className="mt-4 space-y-3 border-t border-border/50 pt-3" onSubmit={(event) => { event.preventDefault(); void submit() }}>
      <WriteAccess trading={trading} />
      <fieldset disabled={pending || !status.eligible} className="space-y-2">
        <legend className="sr-only">Payout amount</legend>
        <div className="trade-label">
          <label htmlFor={id}>Amount</label>
          <span className="flex gap-2">
            <input id={id} className="trade-input min-w-0 flex-1" inputMode="decimal" autoComplete="off" value={amount} placeholder={status.eligible ? status.maximum : "Not eligible yet"}
              aria-invalid={invalid != null} aria-describedby={`${id}-hint`} onChange={(event) => { setAmount(event.target.value); setDone(null) }} />
            <button type="button" className="trade-button" aria-label="Use the maximum amount" onClick={() => { setAmount(status.maximum); setDone(null) }}>Max</button>
          </span>
        </div>
        <p id={`${id}-hint`} className={`text-xs ${invalid ? "text-warn" : "text-muted"}`}>
          {invalid ?? (status.eligible
            ? <>Between {formatMoney(status.minimum)} and {formatMoney(status.maximum)}. You receive {formatMoney(percentOfMoney(amount.trim() || status.maximum, status.split_percent))} ({status.split_percent}%).</>
            : status.blocked?.message)}
        </p>
      </fieldset>
      <TradingError error={error} />
      {done && <p role="status" className="text-sm text-bullish">Payout #{done.number} of {formatMoney(done.amount)} requested: {formatMoney(done.trader_share)} to you.</p>}
      <button type="submit" className="trade-button" disabled={!status.eligible || pending || invalid != null || !amount.trim() || blocked}>
        {pending ? "Requesting…" : "Request payout"}
      </button>
    </form>
  )
}

function NotFunded({ trading, account, plans, onStart }: { trading: TradingStatus; account: Account; plans: Plan[]; onStart: (id: string) => void }) {
  const [, navigate] = useRoute()
  const unlocked = unlockedNextPlan(plans, account)
  const funded = plans.filter((p) => p.rules.phase === "funded")
  const e = account.evaluation
  return <>
    {unlocked ? (
      <div role="status" className="rounded-lg border border-bullish/50 bg-bullish/5 p-4">
        <div className="font-medium text-bullish">{unlocked.rules.phase === "verification" ? "Verification unlocked" : "Funded account unlocked"}</div>
        <p className="mt-1 text-sm">You passed {account.rules.plan}. Start <strong>{unlocked.name}</strong> to continue your program. {unlocked.summary}</p>
        <button type="button" className="trade-button mt-3" disabled={!trading.enabled} onClick={() => onStart(unlocked.id)}>Start {unlocked.name}</button>
      </div>
    ) : (
      <div className="rounded-lg border border-border bg-panel p-4 text-sm">
        <div className="font-medium">Payouts are available on funded accounts</div>
        <p className="mt-1 text-muted">
          {e.status === "failed" ? `${account.rules.plan ?? "This attempt"} has ended. ` : account.rules.plan ? `You are on ${account.rules.plan}${e.enabled ? ` (${e.status})` : ""}. ` : ""}
          Pass each required evaluation and verification step to unlock its funded account.
        </p>
        <button type="button" className="trade-button mt-3" onClick={() => navigate({ view: "rules" })}>See plans and rules</button>
      </div>
    )}
    <Panel title="How payouts work">
      <ol className="grid gap-3 text-sm sm:grid-cols-2 lg:grid-cols-4">
        {[
          ["Pass the required steps", "Reach each target and its day objectives while respecting the loss rules."],
          ["Start the funded account", "Trade without a profit target, under the funded plan’s floor and payout rules."],
          ["Collect qualifying days", "Days that end with enough net realised profit, after fees, count toward the next payout."],
          ["Request a payout", "With no open positions or orders, withdraw part of the profit. You keep most of each payout."],
        ].map(([title, body], i) => (
          <li key={title} className="rounded-md border border-border/60 p-3">
            <div className="text-[11px] font-medium uppercase tracking-wide text-accent">Step {i + 1}</div>
            <div className="mt-1 font-medium">{title}</div>
            <p className="mt-1 text-xs text-muted">{body}</p>
          </li>
        ))}
      </ol>
    </Panel>
    {funded.length > 0 && <Panel title="Funded plans">
      <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Funded plans">
        <table className="w-full text-left text-xs whitespace-nowrap">
          <thead className="text-[11px] uppercase tracking-wide text-muted"><tr>
            {["Plan", "Balance", "Qualifying days", "Per payout", "Your share", "Minimum", "Caps", ""].map((h) => <th key={h} className="px-2 py-2 font-normal">{h}</th>)}
          </tr></thead>
          <tbody>{funded.map((p) => {
            const r = p.rules.payouts
            const locked = lockReason(p, plans, account)
            return <tr key={p.id} className="border-t border-border/40">
              <td className="px-2 py-2 font-medium">{p.name}</td>
              <td className="px-2 py-2 tabular">{formatMoney(p.initial_cash, 0)}</td>
              <td className="px-2 py-2 tabular">{r ? `${r.qualifying_days} × ${formatMoney(r.qualifying_profit, 0)}+` : "—"}</td>
              <td className="px-2 py-2">{r ? `Up to ${r.withdrawal_percent}% of profit` : "—"}</td>
              <td className="px-2 py-2 tabular">{r ? `${r.split_percent}%` : "—"}</td>
              <td className="px-2 py-2 tabular">{r ? formatMoney(r.minimum, 0) : "—"}</td>
              <td className="px-2 py-2 tabular">{r?.caps.length ? r.caps.map((c) => formatMoney(c, 0)).join(" / ") : "None"}</td>
              <td className="px-2 py-2 text-right">{locked
                ? <span className="text-faint" title={locked}>{locked}</span>
                : <button type="button" className="trade-button" disabled={!trading.enabled} onClick={() => onStart(p.id)}>Start</button>}</td>
            </tr>
          })}</tbody>
        </table>
      </div>
    </Panel>}
  </>
}

function History({ payouts, split }: { payouts: Payout[]; split?: number }) {
  return (
    <Panel title="Payout history">
      <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Payout history">
        <table className="w-full text-left text-xs whitespace-nowrap">
          <thead className="text-[11px] uppercase tracking-wide text-muted"><tr>
            {["#", "Requested", "Amount", split != null ? `Your share (${split}%)` : "Your share", "Balance at request"].map((h) => <th key={h} className="px-2 py-2 font-normal">{h}</th>)}
          </tr></thead>
          <tbody>{[...payouts].reverse().map((p) => (
            <tr key={p.number} className="border-t border-border/40">
              <td className="px-2 py-2 tabular">{p.number}</td>
              <td className="px-2 py-2">{timestampET(p.time)}</td>
              <td className="px-2 py-2 tabular">{formatMoney(p.amount)}</td>
              <td className="px-2 py-2 tabular text-bullish">{formatMoney(p.trader_share)}</td>
              <td className="px-2 py-2 tabular">{formatMoney(p.balance)}</td>
            </tr>
          ))}</tbody>
        </table>
      </div>
    </Panel>
  )
}

export function SizeScalingProgress({ status: s }: { status: SizeScalingStatus }) {
  return <Panel title="Account size scaling">
    <dl className="grid gap-3 text-sm sm:grid-cols-3">
      <div><dt className="text-muted">Current account size</dt><dd>{formatMoney(s.size)}</dd></div>
      <div><dt className="text-muted">Original account size</dt><dd>{formatMoney(s.original)}</dd></div>
      <div><dt className="text-muted">Next account size</dt><dd>{formatMoney(s.next_size)} · maximum {formatMoney(s.max_balance)}</dd></div>
      <div><dt className="text-muted">Finished review days</dt><dd>{s.period_days} / {s.days_required}</dd></div>
      <div><dt className="text-muted">Review net realised profit</dt><dd>{formatMoney(s.period_profit)} / {formatMoney(s.profit_required)}</dd></div>
      <div><dt className="text-muted">Review payouts</dt><dd>{s.period_payouts} / {s.payouts_required}</dd></div>
    </dl>
    <p className="mt-3 text-xs text-muted">Review started {s.period_started}. Withdrawals do not reduce review profit. Each review starts a new period; capital growth is not withdrawable profit.</p>
    {s.history.length > 0 && <ul className="mt-3 space-y-1 text-xs" aria-label="Account size history">
      {s.history.map((h) => <li key={h.day}>{h.day}: {formatMoney(h.old)} → {formatMoney(h.size)}</li>)}
    </ul>}
  </Panel>
}
