import { useActionApi } from "../api/action-client"
import { PendingSettingsNotice } from "./PendingSettingsNotice"
import { useState } from "react"

import { useLive } from "../api/live"
import { useAccount, useRefreshTrading, useTradingSession } from "../api/trading"
import type { Guardrails, Risk } from "../api/trading-types"
import { formatMoney, validMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { LimitsEditor } from "./LimitsEditor"
import { TradingError, writeBlocked } from "./TradingControls"

const fields: { key: keyof Guardrails; label: string; money?: boolean }[] = [
  { key: "soft_floor", label: "Soft floor equity ($)", money: true },
  { key: "soft_floor_percent", label: "Soft floor (% of drawdown above plan floor)" },
  { key: "max_opening_trades", label: "Maximum opening orders per day" },
  { key: "cooldown_loss", label: "Closing loss that starts cooldown ($)", money: true },
  { key: "cooldown_minutes", label: "Cooldown after stop-out (market minutes)" },
  { key: "profit_lock", label: "Daily profit lock ($)", money: true },
]
function pendingValue(value: unknown): string {
  return value != null && typeof value === "object"
    ? Object.entries(value).map(([key, entry]) => `${key.replaceAll("_", " ")}: ${pendingValue(entry)}`).join(" · ")
    : String(value)
}
export function PersonalRules({ risk }: { risk: Risk }) {
  const { trading } = useLive()
  const [editing, setEditing] = useState(false)
  const [limits, setLimits] = useState(false)
  const state = risk.guardrail_state
  return <section aria-label="Personal guardrails" className="space-y-3 rounded-md border border-border p-3">
    <div className="flex flex-wrap items-center justify-between gap-2"><h3 className="text-sm font-medium">Personal guardrails</h3>
      <div className="flex gap-2">{risk.guardrails && <button type="button" className="trade-button" disabled={!trading?.enabled} onClick={() => setEditing(true)}>Edit guardrails</button>}
        <button type="button" className="trade-button" disabled={!trading?.enabled} onClick={() => setLimits(true)}>Edit risk limits</button></div></div>
    <p className="text-xs text-muted">Your rules, separate from the plan. Zero turns a field off. Tightening applies now; disabling or loosening a guardrail waits for the next trading day.</p>
    <PendingSettingsNotice requiresReset={risk.pending_requires_reset} />
    {state && <p className="text-xs tabular">{state.opening_trades} opening orders today · Soft floor {formatMoney(state.soft_floor)}
      {state.cooldown_seconds > 0 && <> · Cooldown {Math.ceil(state.cooldown_seconds / 60)} market minutes left</>}
      {!!state.latched.length && <span className="text-warn"> · Reduce-only: {state.latched.join(", ")}</span>}</p>}
    {risk.guardrails && <dl className="grid gap-x-4 gap-y-1 text-xs sm:grid-cols-2">{fields.map(({ key, label, money }) => <div key={key}>
      <dt className="text-muted">{label}</dt><dd className="tabular">{Number(risk.guardrails![key]) === 0 ? "Off" : money ? formatMoney(String(risk.guardrails![key])) : risk.guardrails![key]}
        {risk.pending_guardrails && risk.pending_guardrails[key] !== risk.guardrails![key] && <span className="text-warn"> → {Number(risk.pending_guardrails[key]) === 0 ? "Off" : money ? formatMoney(String(risk.pending_guardrails[key])) : risk.pending_guardrails[key]} next trading day</span>}</dd>
    </div>)}</dl>}
    {risk.pending_limits && <div className="space-y-1 text-xs"><p className="font-medium text-warn">Pending risk limits · next trading day</p>
      <dl className="grid gap-x-4 gap-y-1 sm:grid-cols-2">{Object.entries(risk.pending_limits).filter(([key, value]) => JSON.stringify(value) !== JSON.stringify(risk.limits[key as keyof typeof risk.limits])).map(([key, value]) =>
        <div key={key}><dt className="text-muted">{key.replaceAll("_", " ")}</dt><dd className="tabular">{pendingValue(value)}</dd></div>)}</dl>
    </div>}
    {editing && trading && risk.guardrails && <GuardrailsEditor risk={risk} onClose={() => setEditing(false)} />}
    {limits && trading && <LimitsEditor initial={risk} trading={trading} onClose={() => setLimits(false)} />}
  </section>
}
function GuardrailsEditor({ risk, onClose }: { risk: Risk; onClose: () => void }) {
  const api = useActionApi()
  const { trading } = useLive()
  const account = useAccount().data
  const unusedPercent = account != null && Number(account.rules.max_drawdown ?? 0) === 0
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const sameSession = useTradingSession()
  const [draft, setDraft] = useState(() => Object.fromEntries(Object.entries(risk.pending_guardrails ?? risk.guardrails!).map(([key, value]) => [key, String(value)])))
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<unknown>()
  const valid = fields.every(({ key, money }) => money ? validMoney(draft[key] ?? "") : /^\d+$/.test(draft[key] ?? "") && Number.isSafeInteger(Number(draft[key]))) &&
    Number(draft.soft_floor_percent) <= 99 && Number(draft.cooldown_minutes) <= 1440
  async function save() {
    if (!trading || busy || !valid) return
    setBusy(true); setError(undefined)
    const guardrails = Object.fromEntries(fields.map(({ key, money }) => [key, money ? draft[key] : Number(draft[key])])) as unknown as Guardrails
    try { await api.updateGuardrails(risk.limits_revision, guardrails, trading.write); if (sameSession()) onClose() }
    catch (failure) { if (sameSession()) setError(failure) }
    finally { if (sameSession()) setBusy(false); void refresh() }
  }
  return <Dialog title="Edit personal guardrails" onClose={onClose}>
    <p className="text-xs text-muted">Use a dollar equity level or a percentage floor. If both are set, the higher wins. Any triggered closing stop (including a trailing stop or OCO stop) starts the configured cooldown; the dollar loss threshold also covers other closing fills. Zero disables a field. Floor percent is 0–99; 100 can put it at the peak and latch at once.</p>
    <form className="space-y-3" onSubmit={(event) => { event.preventDefault(); void save() }}>
      <fieldset disabled={busy} className="grid min-w-0 grid-cols-2 gap-3">{fields.map(({ key, label, money }) => <label key={key} className="trade-label">{label}<input className="trade-input" inputMode={money ? "decimal" : "numeric"} max={key === "soft_floor_percent" ? 99 : undefined} value={draft[key]} onChange={(event) => setDraft({ ...draft, [key]: event.target.value })} />{key === "soft_floor_percent" && unusedPercent && <span className="text-xs text-muted">This plan has no drawdown floor, so a soft floor percent does nothing. Use an absolute soft floor instead.</span>}</label>)}</fieldset>
      <TradingError error={error} />
      {!!error && <p className="text-xs text-muted">If the revision changed, close and reopen this editor to review the latest rules.</p>}
      <button type="submit" className="trade-button" disabled={!valid || busy || !trading || writeBlocked(trading, token)}>Save guardrails</button>
    </form>
  </Dialog>
}
