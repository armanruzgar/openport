import { useActionApi } from "../api/action-client"
import { useRef, useState } from "react"

import { useRefreshTrading, useTradingSession } from "../api/trading"
import type { KillChange, KillState, TradingStatus } from "../api/trading-types"
import { useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { TradingError, writeBlocked } from "./TradingControls"

const timeFormat = new Intl.DateTimeFormat("en-US", { timeZone: "America/New_York", month: "short", day: "numeric", hour: "numeric", minute: "2-digit" })
/** One change of the latch in words: "Tripped: DAILY_LOSS (replaced a manual stop)". */
export function killChangeText(change: KillChange): string {
  if (change.action === "release") return `Released: ${change.previous ?? "a personal guardrail"} expired`
  if (change.action === "reset") return `Reset${change.reason ? `: ${change.reason}` : ""}${change.previous ? ` (cleared ${change.previous})` : ""}`
  return `Tripped: ${change.reason ?? "no reason"}${change.previous ? ` (replaced ${change.previous})` : ""}`
}

export function KillSwitch({ kill, trading }: { kill: KillState; trading: TradingStatus }) {
  const api = useActionApi()
  const [reason, setReason] = useState("")
  const [action, setAction] = useState<"trip" | "reset" | null>(null)
  const [error, setError] = useState<unknown>()
  const [pending, setPending] = useState(false)
  const [result, setResult] = useState<string | null>(null)
  const busy = useRef(false)
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const blocked = writeBlocked(trading, token)
  async function confirm() {
    if (!action || !reason.trim() || busy.current || blocked) return
    busy.current = true; setPending(true); setError(undefined)
    try {
      const response = await api.setKill(action, reason.trim(), trading.write)
      if (sameSession()) {
        setResult(`${response.kill.latched ? "Kill switch latched · reduce-only" : "Kill switch reset"}. ${response.cancelled_orders.length} orders cancelled.`)
        setAction(null); setReason("")
      }
    } catch (failure) { if (sameSession()) setError(failure) }
    finally { busy.current = false; if (sameSession()) setPending(false); void refresh() }
  }
  return <div className="space-y-3">
    <div className={`rounded-md border p-3 text-sm ${kill.latched ? "border-danger text-danger" : "border-border text-muted"}`}>
      <strong>{kill.latched ? "LATCHED · reduce-only: closing orders and exits still work" : "Armed · trading permitted"}</strong>
      <p className="mt-1 break-words">{kill.reason ?? (kill.latched ? "No reason provided" : "Trip to cancel opening orders and allow only closing orders and exits.")}</p>
      {kill.latched && kill.reset_blocked && <p className="mt-1 break-words text-xs">Kill-switch reset now would not clear it: {kill.reset_blocked.message}</p>}
      {kill.latched && kill.reason === "TRADE_LIMIT" && <p className="mt-1 text-xs">The opening order that reached the limit may finish its partial fills. Other risk and plan checks still apply.</p>}
    </div>
    {kill.history && kill.history.length > 0 && <details className="text-xs text-muted">
      <summary className="cursor-pointer">History · {kill.history.length}</summary>
      <ol className="mt-1 space-y-0.5">
        {[...kill.history].reverse().slice(0, 10).map((change, index) => <li key={index} className="break-words">
          <span className="text-faint">{Number.isFinite(Date.parse(change.time)) ? timeFormat.format(Date.parse(change.time)) : change.time} ET</span>{" "}
          {killChangeText(change)} <span className="text-faint">· {change.actor}</span>
        </li>)}
      </ol>
    </details>}
    <label className="trade-label">Reason<input className="trade-input" value={reason} onChange={(e) => setReason(e.target.value)} placeholder={kill.latched ? "Why is it safe to resume?" : "Why are you stopping trading?"} /></label>
    <button className="trade-button" type="button" disabled={blocked || !reason.trim()} onClick={() => { setError(undefined); setAction(kill.latched ? "reset" : "trip") }}>{kill.latched ? "Reset kill switch…" : "Trip kill switch…"}</button>
    {result && <p role="status" className="text-xs text-muted">{result}</p>}
    {action && <Dialog title={action === "trip" ? "Confirm kill switch" : "Confirm kill-switch reset"} onClose={() => { if (!pending) setAction(null) }}>
      <p className="text-sm">{action === "trip" ? "Latch the kill switch in reduce-only mode? Orders that open or increase positions are cancelled. Closing orders, Flatten and bracket exits still work."
        : kill.reset_blocked ? `This kill-switch reset cannot clear the latch: ${kill.reset_blocked.message}`
        : "Clear the latched kill switch and allow new orders? Risk limits will still apply."}</p>
      <p className="break-words text-sm text-muted">Reason: {reason}</p>
      <TradingError error={error} />
      <div className="flex gap-2"><button className="trade-button" disabled={pending || blocked} onClick={() => void confirm()}>{pending ? "Applying…" : action === "trip" ? "Confirm trip" : "Confirm reset"}</button><button className="trade-button" disabled={pending} onClick={() => setAction(null)}>Back</button></div>
    </Dialog>}
  </div>
}
