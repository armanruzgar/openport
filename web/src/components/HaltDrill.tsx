import { useRef, useState } from "react"
import { useActionApi } from "../api/action-client"
import { useLive } from "../api/live"
import { useRefreshTrading, useTradingSession } from "../api/trading"
import type { TradingStatus } from "../api/trading-types"
import { useWriteToken } from "../lib/write-token"
import { TradingError, writeBlocked } from "./TradingControls"
import { Panel } from "./ui"

export function HaltDrill({ trading }: { trading: TradingStatus }) {
  const { source, replay, underlyings } = useLive()
  const api = useActionApi()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const token = useWriteToken()
  const [symbol, setSymbol] = useState("")
  // The field's text: a clamp on each keystroke wrote 1 back into a cleared field.
  const [minutes, setMinutes] = useState("5")
  const count = /^\d+$/.test(minutes) ? Number(minutes) : NaN
  const minutesValid = count >= 1 && count <= 390
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<unknown>()
  const busy = useRef(false)
  if (source !== "replay") return null
  const blocked = pending || !!replay?.finished || !!replay?.stepping || !!replay?.fast_forwarding || writeBlocked(trading, token)
  async function submit() {
    if (blocked || busy.current || !symbol || !minutesValid) return
    busy.current = true; setPending(true); setError(undefined)
    try { await api.haltUnderlying(symbol, count, trading.write) }
    catch (failure) { if (sameSession()) setError(failure) }
    finally { busy.current = false; if (sameSession()) setPending(false); void refresh() }
  }
  return <Panel title="Single-name halt drill">
    <form className="flex flex-wrap items-end gap-2" onSubmit={event => { event.preventDefault(); void submit() }}>
      <label className="text-sm">Underlying<select className="trade-input block" value={symbol} disabled={blocked} onChange={event => setSymbol(event.target.value)}>
        <option value="">Choose a stock or ETF</option>{underlyings.map(item => <option key={item.symbol} value={item.symbol}>{item.symbol}</option>)}
      </select></label>
      <label className="text-sm">Minutes<input className="trade-input block" type="number" min={1} max={390} step={1} value={minutes} disabled={blocked}
        aria-invalid={!minutesValid} onChange={event => setMinutes(event.target.value)} /></label>
      <button className="trade-button" disabled={blocked || !symbol || !minutesValid}>Halt underlying</button>
    </form>
    {!minutesValid && <p className="mt-2 text-xs text-warn">Enter whole minutes from 1 to 390.</p>}
    <p className="mt-2 text-xs text-muted">Simulated stock/ETF pause at replay time, capped at the regular close. Orders on that underlying wait; other names are not halted. Index symbols are refused. Trading resumes when the pause ends and fresh quotes arrive.</p>
    <TradingError error={error} />
  </Panel>
}
