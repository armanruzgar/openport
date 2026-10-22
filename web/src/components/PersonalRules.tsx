import { useActionApi } from "../api/action-client"
import { PendingSettingsNotice } from "./PendingSettingsNotice"
import { useState } from "react"

import { useLive } from "../api/live"
import { useAccount, useRefreshTrading, useTradingSession } from "../api/trading"
import type { Guardrails, Risk } from "../api/trading-types"
import { formatMoney, validMoney } from "../lib/trading"
import { tiltText } from "../lib/prop-rules"
import { timestampET } from "../lib/freshness"
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
  { key: "loss_streak", label: "Losing closes in a row that start the cooldown" },
  { key: "size_creep_percent", label: "Size creep: opening order over % of recent median" },
  { key: "size_creep_window", label: "Size creep: recent opening orders (3–100; 0 is 20)" },
]
/** Discipline settings older servers do not have; sent only when the server reports them. */
const discipline: (keyof Guardrails)[] = ["loss_streak", "size_creep_percent", "size_creep_window", "size_creep_block"]
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
      {!!state.latched.length && <span className="text-warn"> · Reduce-only: {state.latched.join(", ")}</span>}
      {!!state.losing_closes && <> · {state.losing_closes} losing {state.losing_closes === 1 ? "close" : "closes"} in a row</>}</p>}
    {!!state?.tilt?.length && <ul aria-label="Tilt flags today" className="space-y-1 text-xs text-warn">
      {state.tilt.map((flag, i) => <li key={i}><span className="tabular text-muted">{timestampET(flag.time)}</span> {tiltText(flag)}</li>)}
    </ul>}
    {risk.guardrails && <dl className="grid gap-x-4 gap-y-1 text-xs sm:grid-cols-2">{fields.filter(({ key }) => risk.guardrails![key] !== undefined).map(({ key, label, money }) => <div key={key}>
      <dt className="text-muted">{label}</dt><dd className="tabular">{Number(risk.guardrails![key]) === 0 ? "Off" : money ? formatMoney(String(risk.guardrails![key])) : risk.guardrails![key]}
        {risk.pending_guardrails && risk.pending_guardrails[key] !== risk.guardrails![key] && <span className="text-warn"> → {Number(risk.pending_guardrails[key]) === 0 ? "Off" : money ? formatMoney(String(risk.pending_guardrails[key])) : risk.pending_guardrails[key]} next trading day</span>}</dd>
    </div>)}
      {risk.guardrails.size_creep_block !== undefined && Number(risk.guardrails.size_creep_percent ?? 0) > 0 && <div>
        <dt className="text-muted">Size creep</dt><dd>{risk.guardrails.size_creep_block ? "Refuses the order" : "Warns and flags tilt"}</dd></div>}
    </dl>}
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
  const known = discipline.filter((key) => risk.guardrails![key] !== undefined)
  const [draft, setDraft] = useState(() => ({ loss_streak: "0", size_creep_percent: "0", size_creep_window: "0", size_creep_block: "false",
    ...Object.fromEntries(Object.entries(risk.pending_guardrails ?? risk.guardrails!).map(([key, value]) => [key, String(value)])) }) as Record<string, string>)
  const shown = fields.filter(({ key }) => !discipline.includes(key) || known.includes(key))
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<unknown>()
  const creep = Number(draft.size_creep_percent), window = Number(draft.size_creep_window), streak = Number(draft.loss_streak)
  const valid = shown.every(({ key, money }) => money ? validMoney(draft[key] ?? "") : /^\d+$/.test(draft[key] ?? "") && Number.isSafeInteger(Number(draft[key]))) &&
    Number(draft.soft_floor_percent) <= 99 && Number(draft.cooldown_minutes) <= 1440 &&
    streak <= 100 && (streak === 0 || Number(draft.cooldown_minutes) > 0) &&
    (creep === 0 || (creep > 100 && creep <= 10000)) && (window === 0 || (window >= 3 && window <= 100)) &&
    (creep > 0 || (window === 0 && draft.size_creep_block !== "true"))
  async function save() {
    if (!trading || busy || !valid) return
    setBusy(true); setError(undefined)
    const guardrails = Object.fromEntries([...shown.map(({ key, money }) => [key, money ? draft[key] : Number(draft[key])]),
      ...(known.includes("size_creep_block") ? [["size_creep_block", draft.size_creep_block === "true"]] : [])]) as unknown as Guardrails
    try { await api.updateGuardrails(risk.limits_revision, guardrails, trading.write); if (sameSession()) onClose() }
    catch (failure) { if (sameSession()) setError(failure) }
    finally { if (sameSession()) setBusy(false); void refresh() }
  }
  return <Dialog title="Edit personal guardrails" onClose={onClose}>
    <p className="text-xs text-muted">Use a dollar equity level or a percentage floor. If both are set, the higher wins. Any triggered closing stop (including a trailing stop or OCO stop) starts the configured cooldown; the dollar loss threshold also covers other closing fills. Zero disables a field. Floor percent is 0–99; 100 can put it at the peak and latch at once.</p>
    <form className="space-y-3" onSubmit={(event) => { event.preventDefault(); void save() }}>
      {known.length > 0 && <p className="text-xs text-muted">Discipline: a losing streak starts the same cooldown; size creep compares each opening order with the median of your recent ones and flags tilt in the journal, or refuses the order when blocking. Opt-in; zero turns each off.</p>}
      <fieldset disabled={busy} className="grid min-w-0 grid-cols-2 gap-3">{shown.map(({ key, label, money }) => <label key={key} className="trade-label">{label}<input className="trade-input" inputMode={money ? "decimal" : "numeric"} max={key === "soft_floor_percent" ? 99 : undefined} value={draft[key]} onChange={(event) => setDraft({ ...draft, [key]: event.target.value })} />{key === "soft_floor_percent" && unusedPercent && <span className="text-xs text-muted">This plan has no drawdown floor, so a soft floor percent does nothing. Use an absolute soft floor instead.</span>}</label>)}
        {known.includes("size_creep_block") && <label className="trade-label flex items-center gap-2"><input type="checkbox" checked={draft.size_creep_block === "true"}
          onChange={(event) => setDraft({ ...draft, size_creep_block: String(event.target.checked) })} />Refuse size creep instead of warning</label>}</fieldset>
      <TradingError error={error} />
      {!!error && <p className="text-xs text-muted">If the revision changed, close and reopen this editor to review the latest rules.</p>}
      <button type="submit" className="trade-button" disabled={!valid || busy || !trading || writeBlocked(trading, token)}>Save guardrails</button>
    </form>
  </Dialog>
}
