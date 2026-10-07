import { useActionApi } from "../api/action-client"
import { useEffect, useRef, useState } from "react"
import { useQueryClient } from "@tanstack/react-query"

import { useLive } from "../api/live"
import { usePortfolio, useServerAlerts } from "../api/trading"
import type { Alert, AlertRequest, AlertScope, OrderLeg } from "../api/trading-types"
import { alertMetrics, describeServerAlert, firedAlerts, heldLegs, serverAlertMessage } from "../lib/alerts"
import { notify } from "../lib/notify"

/** Announces each firing of the account's server alerts once, after the first list it sees. */
export function ServerAlertWatcher({ sound }: { sound: boolean }) {
  const { accountScope } = useLive()
  const alerts = useServerAlerts().data?.alerts
  const seen = useRef<{ scope: number; fired: Map<string, number> } | null>(null)
  useEffect(() => {
    if (!alerts) return
    const last = seen.current
    seen.current = { scope: accountScope, fired: new Map(alerts.map((a) => [a.id, a.fired])) }
    // The first list for an account is history.
    if (last?.scope !== accountScope) return
    for (const alert of firedAlerts(last.fired, alerts)) {
      const { title, body } = serverAlertMessage(alert)
      notify(title, body, sound)
    }
  }, [alerts, accountScope, sound])
  return null
}

const scopes: { value: AlertScope; label: string }[] = [
  { value: "contract", label: "Contract" }, { value: "spread", label: "Spread" },
  { value: "underlying", label: "Underlying" }, { value: "account", label: "Account" },
]

/** The account's alerts, kept and checked by the server, and a form to add one. */
export function ServerAlerts() {
  const { trading, underlyings, source, accountScope, status } = useLive()
  const api = useActionApi(accountScope)
  const client = useQueryClient()
  const publication = useServerAlerts()
  const alerts = publication.data?.alerts ?? []
  const positions = usePortfolio().data?.positions ?? []
  const mode = trading?.write ?? "disabled"
  const notifications = status?.notifications
  const forwards = notifications?.enabled && (source === "live" || source === "replay") &&
    (source === "live" && !status?.provider.simulated || notifications.include_simulated === true) &&
    notifications.channels.some((channel) => channel.enabled && channel.events.includes("alert"))
  const held = [...new Set(positions.filter((p) => p.quantity !== 0).map((p) => p.symbol))]
  const heldUnderlyings = [...new Set(positions.filter((p) => p.quantity !== 0).map((p) => p.underlying))]
  const [scope, setScope] = useState<AlertScope>("underlying")
  const [metric, setMetric] = useState("price")
  const [symbol, setSymbol] = useState(underlyings[0]?.symbol ?? "SPX")
  const [direction, setDirection] = useState<"at_or_above" | "at_or_below">("at_or_above")
  const [level, setLevel] = useState("")
  const [label, setLabel] = useState("")
  const [repeat, setRepeat] = useState(false)
  const [pending, setPending] = useState(false)
  const [message, setMessage] = useState("")
  const [spreadLegs, setSpreadLegs] = useState<OrderLeg[]>([
    { symbol: "", side: "buy", ratio: 1 }, { symbol: "", side: "sell", ratio: 1 },
  ])
  useEffect(() => { setMessage(""); setPending(false) }, [accountScope])
  const metrics = alertMetrics[scope]
  const legs = scope === "spread" ? spreadLegs : null
  const valid = /^-?\d+(\.\d{1,6})?$/.test(level.trim()) && metrics.some((m) => m.value === metric) &&
    (scope === "account" || scope === "spread" || symbol.trim() !== "") && (!legs || (legs.length >= 2 && legs.length <= 4 &&
      new Set(legs.map((leg) => leg.symbol)).size === legs.length &&
      legs.every((leg) => leg.symbol.trim() && Number.isInteger(leg.ratio) && leg.ratio >= 1 && leg.ratio <= 10)))
  function choose(next: AlertScope) {
    setScope(next)
    setMetric(alertMetrics[next][0]!.value)
    setSymbol(next === "contract" ? held[0] ?? "" : next === "spread" ? heldUnderlyings[0] ?? "" : next === "underlying" ? underlyings[0]?.symbol ?? "SPX" : "")
  }
  function changeLeg(index: number, change: Partial<OrderLeg>) {
    setSpreadLegs((current) => current.map((leg, i) => i === index ? { ...leg, ...change } : leg))
  }
  async function add() {
    if (!valid) return
    const request: AlertRequest = {
      scope, metric, direction, level: level.trim(), repeat, ...(label.trim() ? { label: label.trim() } : {}),
      ...(scope === "contract" || scope === "underlying" ? { symbol: symbol.trim() } : {}), ...(legs ? { legs } : {}),
    }
    setPending(true); setMessage("")
    try {
      await api.createAlert(request, mode)
      setLevel(""); setLabel("")
      setMessage(forwards ? "Alert set. Each firing goes to the configured notification channels."
        : "Alert set. Firings appear here; external forwarding is not enabled for these alerts.")
    } catch (error) {
      setMessage(error instanceof Error ? `Could not set the alert: ${error.message}` : "Could not set the alert.")
    } finally {
      setPending(false)
      void client.invalidateQueries({ queryKey: ["trading"] })
    }
  }
  async function remove(alert: Alert) {
    setPending(true)
    try { await api.deleteAlert(alert.id, mode) } catch { setMessage("Could not delete the alert.") } finally {
      setPending(false)
      void client.invalidateQueries({ queryKey: ["trading"] })
    }
  }
  return <section className="space-y-2">
    <h3 className="text-xs font-medium uppercase tracking-wide text-muted">Account alerts</h3>
    <p className="text-xs text-muted">Watch options, spreads, Greeks, implied volatility and account measures.
      {forwards ? " The server checks them with the browser closed and forwards each firing to the configured channels below."
        : " The server checks them with each market update. External forwarding requires an enabled alert channel; demo and replay also require include_simulated in the server notification config."}</p>
    <form className="grid grid-cols-2 gap-2" onSubmit={(event) => { event.preventDefault(); void add() }}>
      <label className="trade-label">Watch
        <select className="trade-input" aria-label="Alert scope" value={scope} onChange={(e) => choose(e.target.value as AlertScope)}>
          {scopes.map((s) => <option key={s.value} value={s.value}>{s.label}</option>)}
        </select></label>
      <label className="trade-label">Measure
        <select className="trade-input" aria-label="Alert measure" value={metric} onChange={(e) => setMetric(e.target.value)}>
          {metrics.map((m) => <option key={m.value} value={m.value}>{m.label}</option>)}
        </select></label>
      {scope !== "account" && scope !== "spread" && <label className="trade-label col-span-2">{scope === "contract" ? "Contract (OSI)" : "Underlying"}
        <input className="trade-input" aria-label="Alert symbol" value={symbol} list={scope === "contract" ? "alert-contracts" : undefined}
          onChange={(e) => setSymbol(scope === "contract" ? e.target.value : e.target.value.toUpperCase())} />
        {scope === "contract" && <datalist id="alert-contracts">{held.map((s) => <option key={s} value={s} />)}</datalist>}</label>}
      {scope === "spread" && <fieldset className="col-span-2 space-y-2">
        <legend className="trade-label">Spread legs · buys add, sells subtract</legend>
        {heldUnderlyings.length > 0 && <label className="trade-label">Start from held legs
          <select className="trade-input" aria-label="Use held legs" value="" onChange={(event) => {
            const held = heldLegs(positions, event.target.value)
            if (held.length >= 2 && held.length <= 4 && held.every((leg) => leg.ratio <= 10)) setSpreadLegs(held)
          }}><option value="">Choose an underlying</option>{heldUnderlyings.map((underlying) =>
              <option key={underlying} value={underlying}>{underlying}</option>)}</select></label>}
        {spreadLegs.map((leg, index) => <div key={index} className="grid grid-cols-[1fr_4rem_3rem_auto] gap-1">
          <input className="trade-input min-w-0" aria-label={`Leg ${index + 1} contract`} placeholder="Contract OSI" value={leg.symbol}
            onChange={(event) => changeLeg(index, { symbol: event.target.value.toUpperCase() })} />
          <select className="trade-input" aria-label={`Leg ${index + 1} side`} value={leg.side}
            onChange={(event) => changeLeg(index, { side: event.target.value as OrderLeg["side"] })}>
            <option value="buy">Buy</option><option value="sell">Sell</option></select>
          <input className="trade-input" aria-label={`Leg ${index + 1} ratio`} type="number" min={1} max={10} value={leg.ratio}
            onChange={(event) => changeLeg(index, { ratio: Number(event.target.value) })} />
          <button type="button" className="trade-button" aria-label={`Remove leg ${index + 1}`} disabled={spreadLegs.length <= 2}
            onClick={() => setSpreadLegs((current) => current.filter((_, i) => i !== index))}>×</button>
        </div>)}
        <button type="button" className="trade-button" disabled={spreadLegs.length >= 4}
          onClick={() => setSpreadLegs((current) => [...current, { symbol: "", side: "buy", ratio: 1 }])}>Add leg</button>
      </fieldset>}
      <label className="trade-label">When it is
        <select className="trade-input" aria-label="Alert direction" value={direction} onChange={(e) => setDirection(e.target.value as typeof direction)}>
          <option value="at_or_above">At or above</option><option value="at_or_below">At or below</option>
        </select></label>
      <label className="trade-label">Level
        <input className="trade-input" inputMode="decimal" aria-label="Alert level" value={level} onChange={(e) => setLevel(e.target.value)} /></label>
      <label className="trade-label">Label
        <input className="trade-input" aria-label="Alert label" maxLength={100} value={label} onChange={(e) => setLabel(e.target.value)} /></label>
      <label className="flex items-center gap-2 self-end text-xs">
        <input type="checkbox" aria-label="Repeat alert" className="accent-[var(--accent)]" checked={repeat} onChange={(e) => setRepeat(e.target.checked)} />
        Repeat each time it returns</label>
      <button type="submit" className="trade-button col-span-2" disabled={!valid || pending || mode === "disabled" || alerts.length >= 100}>Set account alert</button>
    </form>
    {message && <p role="status" className="text-xs text-muted">{message}</p>}
    {publication.isError && <p role="alert" className="text-xs text-muted">Could not load account alerts.</p>}
    {alerts.length >= 100 && <p className="text-xs text-muted">This account has 100 alerts. Delete one to add another.</p>}
    {alerts.length ? <ul className="divide-y divide-border/60 text-sm">
      {alerts.map((alert) => <li key={alert.id} className="flex items-center justify-between gap-2 py-1.5">
        <span>{alert.label ? `${alert.label}: ` : ""}{describeServerAlert(alert)}
          <span className="text-xs text-muted"> · {alert.fired ? `fired ${alert.fired}×${alert.value != null ? ` at ${alert.value}` : ""}` : "waiting"}
            {alert.fired_at && <> · <time dateTime={alert.fired_at}>{new Date(alert.fired_at).toLocaleString()}</time></>}
            {alert.repeat ? alert.armed ? alert.fired ? " · armed again" : " · repeats" : " · waiting to rearm" : alert.fired ? " · done" : ""}</span></span>
        <button type="button" className="trade-button" disabled={pending || mode === "disabled"} aria-label={`Delete alert: ${describeServerAlert(alert)}`}
          onClick={() => void remove(alert)}>Delete</button>
      </li>)}
    </ul> : <p className="text-sm text-muted">No account alerts.</p>}
  </section>
}
