import { useReplayControls } from "./ReplayView"
import { scenarioMarkers } from "../lib/practice"
import { useState } from "react"
import { useTrades } from "../api/trading"
import { coachFlags, type JournalTrade } from "../lib/journal"
import { requestAiReview } from "../lib/ai-review"
import { TradingError } from "../components/TradingControls"
import { useQuery } from "@tanstack/react-query"
import { api } from "../api/client"
import { marketNow, useLive } from "../api/live"
import type { Attribution, FillContext, Trade, TradeReview } from "../api/trading-types"
import { LineChart, type Marker, type Series } from "../charts/LineChart"
import { attributionParts } from "../lib/attribution"
import { timestampET } from "../lib/freshness"
import { formatMoney, signedMoney } from "../lib/trading"

const chartTime = new Intl.DateTimeFormat("en-US", { month: "short", day: "numeric", hour: "numeric", minute: "2-digit", timeZone: "America/New_York" })
const number = (value: number | null | undefined, digits = 2) => value == null ? "—" : value.toFixed(digits)

export function ContextCard({ title, context }: { title: string; context?: FillContext | null }) {
  const c = context
  return <section className="rounded border border-border p-3">
    <h3 className="mb-2 text-xs font-medium">{title}</h3>
    <dl className="grid grid-cols-2 gap-x-4 gap-y-1 text-xs tabular">
      <dt className="text-muted">Underlying</dt><dd>{number(c?.spot)}{c?.spot_source ? ` · ${c.spot_source}` : ""}</dd>
      <dt className="text-muted">IV / delta</dt><dd>{c?.iv == null ? "—" : `${(c.iv * 100).toFixed(2)}%`} / {number(c?.delta, 3)}</dd>
      <dt className="text-muted">Time to expiry</dt><dd>{c?.years == null ? "—" : `${(c.years * 365).toFixed(2)} days`}</dd>
      <dt className="text-muted">Equity</dt><dd>{formatMoney(c?.equity)}</dd>
      <dt className="text-muted">Room above floor</dt><dd>{formatMoney(c?.floor_room)}</dd>
      <dt className="text-muted">Buying power</dt><dd>{formatMoney(c?.buying_power)}</dd>
    </dl>
  </section>
}

export function ReviewMetrics({ review, label = "Trade excursions" }: { review?: TradeReview | null; label?: string }) {
  const r = review
  return <section aria-label={label}>
    <h3 className="mb-2 text-xs font-medium">{label}</h3>
    <div className="grid gap-3 text-xs sm:grid-cols-2 lg:grid-cols-4">
      {([['MAE', r?.mae, r?.worst], ['MFE', r?.mfe, r?.best]] as const).map(([name, value, point]) =>
        <div key={name}><div className="text-muted">{name}</div><div className="tabular">{formatMoney(value)}</div>
          <div className="text-[11px] text-muted">{point ? `${timestampET(point.time)} · spot ${number(point.spot)}` : "Not recorded"}</div></div>)}
      <div><div className="text-muted">Give-back</div><div className="tabular">{formatMoney(r?.give_back)}</div></div>
      <div><div className="text-muted">Heat / R</div><div className="tabular">{r?.heat == null ? "—" : `${(r.heat * 100).toFixed(1)}%`} / {r?.r_multiple == null ? "—" : `${r.r_multiple.toFixed(2)}R`}</div></div>
    </div>
    <p className="mt-2 text-[11px] text-muted">{r?.planned_risk == null ? "Planned risk unavailable: no option-price stop or measurable defined-risk structure." : `Planned risk ${formatMoney(r.planned_risk)}.`} Excursions include fees and resolve to the marking cadence, about 15 s on Cboe delayed data.</p>
  </section>
}

/** A round trip's P&L by Greek over its life; nothing for one from before they were kept. */
export function TripAttribution({ attribution: a, open }: { attribution?: Attribution | null; open: boolean }) {
  if (!a) return null
  return <section aria-label="P&L by Greek">
    <h3 className="mb-2 text-xs font-medium">P&L by Greek{open ? " so far" : ""}</h3>
    <div className="grid gap-3 text-xs sm:grid-cols-4 lg:grid-cols-7">
      {attributionParts.map((part) => <div key={part.key} title={part.title}><div className="text-muted">{part.label}</div>
        <div className="tabular">{signedMoney(a[part.key].toFixed(2))}</div></div>)}
      <div><div className="text-muted">Total</div><div className="tabular">{signedMoney(a.total.toFixed(2))}</div></div>
    </div>
    {a.fallback && <p className="mt-2 text-[11px] text-warn">Part of it is other: Greeks were missing at one end of a stretch whose mark moved.</p>}
  </section>
}

/** Available candle closes only; exact stamped spots are separate points. */
export function TradeChart({ trade }: { trade: Trade }) {
  const live = useLive()
  const from = Date.parse(trade.opened) / 1000
  const to = trade.closed ? Date.parse(trade.closed) / 1000 : marketNow(live) / 1000
  const query = useQuery({ queryKey: ["trade-candles", live.accountScope, trade.underlying, live.trading?.account_version],
    queryFn: ({ signal }) => api.candles(trade.underlying, "1m", 5000, signal), staleTime: 15_000 })
  const bars = (query.data?.bars ?? []).filter((bar) => bar.t >= Math.floor(from / 60) * 60 && bar.t <= to)
  const marks = [
    { label: "Entry", time: trade.opened, spot: trade.entry_context?.spot, color: "var(--accent)" },
    { label: "Exit", time: trade.closed, spot: trade.exit_context?.spot, color: "var(--muted)" },
    { label: "MAE", time: trade.review?.worst?.time, spot: trade.review?.worst?.spot, color: "var(--bearish)" },
    { label: "MFE", time: trade.review?.best?.time, spot: trade.review?.best?.spot, color: "var(--bullish)" },
  ].filter((m) => m.time != null)
  const markers: Marker[] = [...marks.map((m) => ({ x: Date.parse(m.time!) / 1000, label: m.label, color: m.color })), ...scenarioMarkers(live.replay?.events ?? [], from * 1000, to * 1000, 1000, trade.underlying)]
  const series: Series[] = [{ id: "underlying", label: "1m candle close", color: "var(--muted)", points: bars.map((bar) => ({ x: bar.t, y: bar.c })) },
    ...marks.map((m) => ({ id: m.label, label: m.label, color: m.color, dots: true, points: [{ x: Date.parse(m.time!) / 1000, y: m.spot ?? null }] }))]
  return <section aria-label="Underlying across trade">
    <h3 className="text-xs font-medium">{trade.underlying} across the trade{live.status?.provider.simulated ? " · Simulated prices" : ""}</h3>
    {query.error && <p className="text-xs text-muted">Underlying candles unavailable.</p>}
    {bars.length || marks.some(m => m.spot != null) ? <LineChart series={series} markers={markers} height={190} formatX={(x) => chartTime.format(x * 1000)} xLabel="New York time" formatY={(y) => y.toFixed(2)} />
      : <p className="text-xs text-muted">{query.isPending ? "Loading underlying candles…" : "No stored prices for this trade."}</p>}
    <p className="text-[11px] text-muted">Candle closes and stamped spot markers. Missing history is not reconstructed.</p>
  </section>
}

export function TradeCoach({ trade }: { trade: JournalTrade }) {
  const live = useLive()
  const trades = useTrades("all")
  const controls = useReplayControls()
  const playbooks = useQuery({ queryKey: ["review-playbooks", live.accountScope, live.trading?.account_version], queryFn: ({ signal }) => api.playbooks(signal) })
  const brokenOrders = new Set(Object.values(playbooks.data?.reports ?? {}).flatMap(report => report.trades.filter(t => Object.values(t.rules).some(v => v === false)).map(t => t.order)))
  const flags = coachFlags(trade, [...(trades.data?.trades ?? []), ...(trades.data?.share_trades ?? [])], { brokenOrders, events: live.replay?.events })
  const [enabled, setEnabled] = useState(false)
  const [endpoint, setEndpoint] = useState("")
  const [key, setKey] = useState("")
  const [result, setResult] = useState("")
  const [error, setError] = useState<unknown>()
  const [pending, setPending] = useState(false)
  async function review() {
    setPending(true); setError(undefined)
    try { setResult(await requestAiReview({ enabled, endpoint, key }, trade, flags)) }
    catch (e) { setError(e) } finally { setPending(false) }
  }
  return <section className="space-y-2 text-xs" aria-label="Trade coach">
    {live.replay?.id && <><button className="trade-button" disabled={controls.blocked || controls.pending} onClick={() => void controls.fromTrade(live.replay!.id!, trade.id, () => live.switchSource("replay", "user"))}>Replay from here</button><p className="text-muted">New paused paper account, five minutes before entry (clamped to the session open), with the source seed, date and plan.</p><TradingError error={controls.error} /></>}
    <h3 className="font-medium">Coach flags</h3>
    {flags.length ? <ul>{flags.map(flag => <li key={flag}>{flag}</li>)}</ul> : <p className="text-muted">No flags from available data.</p>}
    <p className="text-muted">Size flag: over twice the median peak size of at least three closed trades in this underlying. Missing evidence is not treated as a passed rule.</p>
    <details><summary>Optional AI review · off by default</summary>
      <p className="my-2">Sends this trade’s review JSON, including notes and tags, to your endpoint only when you press Send. Your key stays in memory. The endpoint must allow this browser’s origin and return a JSON review string.</p>
      <label className="block"><input type="checkbox" checked={enabled} onChange={e => setEnabled(e.target.checked)} /> Enable for this review</label>
      <label className="block">Endpoint<input className="trade-input" value={endpoint} onChange={e => setEndpoint(e.target.value)} placeholder="https://your-host/review" /></label>
      <label className="block">Your key<input className="trade-input" type="password" autoComplete="off" value={key} onChange={e => setKey(e.target.value)} /></label>
      <button className="trade-button" disabled={!enabled || !endpoint || !key || pending} onClick={() => void review()}>{pending ? "Reviewing…" : "Send AI review"}</button>
      <TradingError error={error} />{result && <p className="whitespace-pre-wrap">{result}</p>}
    </details>
  </section>
}
