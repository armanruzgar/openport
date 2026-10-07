import { useActionApi } from "../api/action-client"
import { CopySettingsPicker } from "./CopySettingsPicker"
import { ManageAccountsDialog } from "./ManageAccountsDialog"
import { useQuery } from "@tanstack/react-query"
import { useRef, useState } from "react"

import { useLive } from "../api/live"
import { useAccount, usePlans } from "../api/trading"
import type { FeeModel, FillModel, TradingStatus } from "../api/trading-types"
import { FillModelPicker } from "./FillModelPicker"
import { FeeModelPicker } from "./FeeModelPicker"
import { lockReason, offeredPlans } from "../lib/payouts"
import { isSandboxToken, useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { Badge } from "./ui"
import { marginRequest, MarginSettings, planMargin, type MarginChoice } from "./MarginSettings"
import { planFacts } from "./ResetDialog"
import { TradingError, WriteAccess, writeBlocked } from "./TradingControls"

/**
 * Each account is its own evaluation or practice book on the same market: its own
 * journal, rules, orders and positions. The switcher picks the one the terminal acts on.
 */
export function AccountSwitcher() {
  const api = useActionApi()
  const { accounts, account, switchAccount, trading, source, switchSource } = useLive()
  const sandbox = isSandboxToken(useWriteToken())
  const idle = accounts.find((item) => item.id === account)?.sandbox_idle_seconds
  const history = useQuery({ enabled: !sandbox, queryKey: ["replay-listing"], queryFn: ({ signal }) => api.replay(signal), staleTime: 5_000 })
  const [creating, setCreating] = useState(false)
  const [managing, setManaging] = useState(false)
  if (!trading || !accounts.length) return null
  return (
    <div className="space-y-1">
      <label className="block text-[10px] uppercase tracking-wide text-muted" htmlFor="account-switcher">Account</label>
      <select id="account-switcher" className="trade-input !py-1 text-xs" value={source.startsWith("history:") ? source : account}
        onChange={(event) => {
          const id = event.target.value
          if (id === "+new") setCreating(true)
          else if (id.startsWith("history:")) switchSource(id as `history:${string}`, "user")
          else { if (source.startsWith("history:")) switchSource("live", "user"); switchAccount(id, "user") }
        }}>
        {accounts.map((a) => (
          <option key={a.id} value={a.id}>{source.startsWith("history:") ? "Live main account" : a.name}{source.startsWith("history:") ? "" : a.trading.plan ? ` · ${a.trading.plan}${a.trading.plan_id ? ` · ${a.trading.plan_id}` : ""}` : ""}{a.trading.enabled ? "" : " (unavailable)"}</option>
        ))}
        {(!sandbox ? history.data?.history ?? [] : []).map((run) => <option key={run.id} value={`history:${run.id}`}>Replay · {run.file} · {run.demo ? "simulated" : "recording"} · {run.result}</option>)}
        {source === "live" && !sandbox && <option value="+new">New account…</option>}
      </select>
      {source === "live" && !sandbox && <button type="button" className="trade-button text-xs" onClick={() => setManaging(true)}>Manage accounts…</button>}
      {!!idle && <p className="text-[11px] text-muted">Sandbox · removed after {idle % 3600 === 0 ? `${idle / 3600} h` : `${idle} s`} unused</p>}
      {source !== "live" && <Badge tone="neutral">replay{source.startsWith("history:") ? " · read-only" : ""}</Badge>}
      {creating && <NewAccountDialog trading={trading} onClose={() => setCreating(false)}
        onCreated={(id) => { setCreating(false); switchAccount(id, "user") }} />}
      {managing && <ManageAccountsDialog trading={trading} onClose={() => setManaging(false)} />}
    </div>
  )
}

/** Name an account and pick its plan; funded plans unlock from a passed evaluation in an existing account. */
export function NewAccountDialog({ trading, onClose, onCreated }: { trading: TradingStatus; onClose: () => void; onCreated: (id: string) => void }) {
  const api = useActionApi()
  const { accounts } = useLive()
  const plans = usePlans()
  const account = useAccount().data
  const token = useWriteToken()
  const [name, setName] = useState(`Account ${accounts.length + 1}`)
  const [choice, setChoice] = useState("")
  const [fillModel, setFillModel] = useState<FillModel>("as_displayed")
  const [copyFrom, setCopyFrom] = useState("")
  const [feeModel, setFeeModel] = useState<FeeModel>("flat")
  const [margin, setMargin] = useState<MarginChoice>()
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<unknown>()
  const busy = useRef(false)
  const list = offeredPlans(plans.data?.plans ?? [], account)
  const selected = list.find((plan) => plan.id === choice && !plan.unlocked_by) ?? null
  const trimmed = name.trim()
  async function submit() {
    if (!selected || !trimmed || [...trimmed].length > 64 || busy.current || writeBlocked(trading, token)) return
    busy.current = true
    setPending(true)
    setError(undefined)
    try {
      const response = await api.createAccount({ name: trimmed, plan: selected.id,
        ...(fillModel !== "as_displayed" ? { fill_model: fillModel } : {}),
        ...(copyFrom ? { copy_settings_from: copyFrom } : {}),
        ...(feeModel === "itemized" ? { fee_model: feeModel } : {}),
        ...marginRequest(margin ?? planMargin(selected.rules), planMargin(selected.rules)) }, trading.write)
      onCreated(response.account.id)
    } catch (failure) {
      setError(failure)
    } finally {
      busy.current = false
      setPending(false)
    }
  }
  return (
    <Dialog title="New account" onClose={onClose}>
      <p className="text-sm text-muted">
        A new account trades the same market with its own journal, rules, orders and positions. The others keep
        running: their orders still work and their rules still apply.
      </p>
      <WriteAccess trading={trading} />
      <TradingError error={plans.error} />
      <form className="space-y-3" onSubmit={(event) => { event.preventDefault(); void submit() }}>
        <label className="block space-y-1 text-sm">
          <span className="text-muted">Name</span>
          <input className="trade-input" value={name} onChange={(event) => setName(event.target.value)} aria-label="Account name" />
        </label>
        <fieldset className="space-y-2" disabled={pending}>
          <legend className="mb-1 text-[11px] font-medium uppercase tracking-wide text-muted">Plan</legend>
          {list.map((plan) => (
            <label key={plan.id} className={`block cursor-pointer rounded-md border p-3 ${choice === plan.id ? "border-accent bg-accent/5" : "border-border hover:border-muted"}`}>
              <div className="flex items-start gap-2">
                <input type="radio" name="plan" value={plan.id} disabled={!!plan.unlocked_by} checked={choice === plan.id} onChange={() => setChoice(plan.id)}
                  className="mt-1 accent-[var(--accent)]" />
                <div className="min-w-0">
                  <div className="text-sm font-medium">{plan.name} <span className="text-xs text-muted">· {plan.id}</span></div>
                  <div className="mt-0.5 text-xs text-muted">{lockReason(plan, list, undefined) ?? plan.summary}</div>
                  <div className="mt-1 text-[11px] text-faint">{planFacts(plan).join(" · ")}</div>
                </div>
              </div>
            </label>
          ))}
        </fieldset>
        <FillModelPicker value={fillModel} onChange={setFillModel} disabled={pending} />
        <CopySettingsPicker value={copyFrom} onChange={setCopyFrom} disabled={pending} />
        <FeeModelPicker value={feeModel} onChange={setFeeModel} disabled={pending} flat={trading.fee_per_contract} />
        <MarginSettings value={margin ?? planMargin(selected?.rules)} onChange={setMargin} disabled={pending} />
        {plans.isLoading && <p className="text-sm text-muted">Loading plans…</p>}
        <TradingError error={error} />
        <button type="submit" className="trade-button" disabled={!selected || !trimmed || [...trimmed].length > 64 || pending || writeBlocked(trading, token)}>
          {pending ? "Creating…" : selected ? `Create ${trimmed || "account"}` : "Choose a plan"}
        </button>
      </form>
    </Dialog>
  )
}
