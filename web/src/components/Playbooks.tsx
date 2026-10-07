import { useActionApi } from "../api/action-client"
import { useQuery, useQueryClient } from "@tanstack/react-query"
import { useState } from "react"

import { useLive } from "../api/live"
import { useRefreshTrading } from "../api/trading"
import { timestampET } from "../lib/freshness"
import { formatMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"
import { TradingError, writeBlocked } from "./TradingControls"
import { Panel } from "./ui"

export function usePlaybooks() {
  const api = useActionApi()
  const live = useLive()
  return useQuery({ queryKey: ["playbooks", live.accountScope, live.trading?.account_version],
    queryFn: ({ signal }) => api.playbooks(signal), enabled: live.trading?.enabled === true, refetchInterval: 3000, retry: false })
}
export function AutoPlaybookIndicator() {
  const live = useLive()
  const query = usePlaybooks()
  const active = Object.entries(query.data?.modes ?? {}).filter(([id, mode]) => mode === "auto" && !query.data?.definitions[id]?.deleted)
  if (!active.length) return null
  return <p role="status" className="text-xs text-warn">Auto paper trading · {active.length} playbook{active.length === 1 ? "" : "s"} on {live.account}</p>
}
export function StagedOrders() {
  const live = useLive()
  return <Stages key={live.accountScope} />
}
function Stages() {
  const api = useActionApi()
  const live = useLive()
  const query = usePlaybooks()
  const refresh = useRefreshTrading()
  const client = useQueryClient()
  const token = useWriteToken()
  const [busy, setBusy] = useState<string | null>(null)
  const [error, setError] = useState<unknown>(null)
  if (!live.trading || !query.data?.staged.length) return null
  const blocked = writeBlocked(live.trading, token)
  const action = async (id: string, kind: "send" | "dismiss") => {
    setBusy(id); setError(null)
    try {
      await api.stagedAction(id, kind, live.trading!.write)
      await client.invalidateQueries({ queryKey: ["playbooks"] })
      await refresh()
    } catch (failure) { setError(failure) } finally { setBusy(null) }
  }
  return <Panel title="Staged playbook orders">
    <p className="mb-3 text-xs text-muted">Simulated orders · checked again when sent. Stages expire when the window or conditions end.</p>
    {live.source === "replay" && live.replay?.demo && <p className="mb-2 text-xs text-warn">Demo market · simulated prices</p>}
    <div className="space-y-3">{query.data.staged.map((stage) => <article key={stage.id} className="rounded-md border border-border p-3 text-xs">
      <div className="font-medium">{stage.name} · v{stage.version} · {stage.underlying} · {stage.units} units</div>
      <div className="my-2 space-y-1 tabular">{stage.legs.map((leg) => <div key={leg.symbol}>{leg.side} {leg.ratio} × {leg.strike} {leg.type} · {leg.expiry}</div>)}</div>
      <p>Net limit {formatMoney(stage.net)} · Maximum loss {formatMoney(stage.max_loss)}{stage.max_loss_basis === "scenario_grid" ? " (scenario estimate)" : ""}</p>
      {stage.warnings?.map((warning) => <p key={warning.code} role="status" className="mt-1 text-warn">Check: {warning.message}</p>)}
      {stage.management && <p className="mt-1 text-muted">Take profit: {stage.management.take_profit_percent == null ? "none" : `${stage.management.take_profit_percent}% closing premium`} · Stop: {stage.management.stop_credit_multiple != null ? `${stage.management.stop_credit_multiple}× credit` : stage.management.stop_loss_percent != null ? `${stage.management.stop_loss_percent}% debit loss` : stage.management.stop_underlying ? `underlying ${stage.management.stop_underlying.direction} ${stage.management.stop_underlying.level}` : "none"}</p>}
      {stage.management?.trailing_stop && <p className="mt-1 text-muted">Trailing stop: {stage.management.trailing_stop.percent}% of peak gross profit given back (closing natural)</p>}
      {stage.management?.close_at_dte != null && <p className="mt-1 text-muted">Close at {stage.management.close_at_dte} calendar DTE</p>}
      {stage.management?.max_days_in_trade != null && <p className="mt-1 text-muted">Close after {stage.management.max_days_in_trade} trading days at close by</p>}
      <p className="mt-1 text-muted">Close by {timestampET(stage.close_by)} · Tag playbook:{stage.playbook}@v{stage.version}</p>
      <div className="mt-3 flex gap-2">
        <button className="trade-button" disabled={!!blocked || busy != null} onClick={() => void action(stage.id, "send")}>Send {stage.name}</button>
        <button className="trade-button" disabled={!!blocked || busy != null} onClick={() => void action(stage.id, "dismiss")}>Dismiss {stage.name}</button>
      </div>
    </article>)}</div>
    {error != null && <TradingError error={error} />}
  </Panel>
}
export function PassOddsCard({ playbook = "" }: { playbook?: string }) {
  const api = useActionApi()
  const live = useLive()
  const [days, setDays] = useState(20)
  const query = useQuery({ queryKey: ["pass-odds", live.accountScope, live.trading?.account_version, playbook, days],
    queryFn: ({ signal }) => api.passOdds(days, 1000, playbook, signal), enabled: live.trading?.enabled === true, retry: false })
  const result = query.data
  const percent = (value: number) => `${(value * 100).toFixed(1)}%`
  return <Panel title="Evaluation pass odds" actions={<label className="flex items-center gap-2 text-xs">Trading days
    <select aria-label="Pass odds horizon" className="trade-input !w-auto" value={days} onChange={(event) => setDays(Number(event.target.value))}>
      {[5, 10, 20, 60].map((day) => <option key={day} value={day}>{day}</option>)}
    </select>
  </label>}>
    <p className="mb-2 text-xs text-muted">Estimate from past results, not a prediction.</p>
    {query.error ? <p role="status" className="text-xs text-muted">{query.error.message}</p> : !result ? <p className="text-xs text-muted">Loading historical estimate…</p> : <>
      <dl className="grid grid-cols-3 gap-3 text-sm tabular">
        <div><dt className="text-xs text-muted">Pass</dt><dd>{percent(result.pass)}</dd></div>
        <div><dt className="text-xs text-muted">Fail</dt><dd>{percent(result.fail)}</dd></div>
        <div><dt className="text-xs text-muted">Neither</dt><dd>{percent(result.neither)}</dd></div>
      </dl>
      <p className="mt-3 text-xs text-muted">Median days to pass: {result.median_days_to_pass ?? "—"} · {result.historical_days} historical days · {result.samples} samples · seed {result.seed}</p>
      <p className="mt-1 text-[11px] text-faint">{result.history_basis}. {result.path_assumption}.</p>
    </>}
  </Panel>
}
