import { useQuery, useQueryClient } from "@tanstack/react-query"
import { useRef, useState } from "react"

import { useActionApi } from "../api/action-client"
import type { AccountGroup, AccountListItem, CopyOrderResponse, TradingStatus } from "../api/trading-types"
import { useWriteToken } from "../lib/write-token"
import { TradingError, writeBlocked } from "./TradingControls"

/** The server's account groups; a server that offers none answers 404, which reads as no groups. */
export function useAccountGroups(enabled = true) {
  const api = useActionApi()
  return useQuery({
    queryKey: ["account-groups"], enabled, retry: false, staleTime: 30_000,
    queryFn: async ({ signal }) => {
      try { return (await api.accountGroups(signal)).groups }
      catch (error) { if ((error as { status?: number }).status === 404) return [] as AccountGroup[]; throw error }
    },
  })
}

export function allocationText(group: AccountGroup) {
  return group.allocation === "equity" ? "split by equity"
    : group.members.map((m) => `${m.account} ×${Number(m.ratio ?? "1")}`).join(", ")
}

/** Choose a group to copy the order to: each member account submits it as its own order. */
export function CopyGroupField({ groups, value, onChange, disabled, unavailable }: {
  groups: AccountGroup[]; value: string; onChange: (group: string) => void; disabled?: boolean
  /** Why this order cannot be copied, such as a then/oco chain. */
  unavailable?: string
}) {
  if (!groups.length) return null
  const chosen = groups.find((g) => g.name === value)
  return <div className="space-y-1">
    <label className="trade-label">Copy to group
      <select className="trade-input" value={unavailable ? "" : value} disabled={disabled || !!unavailable} onChange={(event) => onChange(event.target.value)}>
        <option value="">None · this account only</option>
        {groups.map((g) => <option key={g.name} value={g.name}>{g.name} · {g.members.length} accounts</option>)}
      </select>
    </label>
    {unavailable ? <p className="text-[11px] text-muted">{unavailable}</p>
      : chosen && <p className="text-[11px] text-muted">Each account of {chosen.name} submits it as its own order with its own checks ({allocationText(chosen)}; sizes round down).</p>}
  </div>
}

function message(result: CopyOrderResponse["results"][number]) {
  if (result.skipped) return result.message ?? "Skipped"
  const response = result.response as { error?: { code?: string; message?: string }; order?: { id?: string; status?: string } } | null | undefined
  if (response?.error) return `${response.error.code ?? "Refused"}: ${response.error.message ?? ""}`
  if (response?.order) return `Order ${response.order.id ?? ""} ${response.order.status ?? ""}`.trim()
  return result.ok ? "Accepted" : `HTTP ${result.status ?? "error"}`
}

/** Each member account's outcome of a copied order. */
export function CopyResults({ result }: { result: CopyOrderResponse }) {
  return <section aria-label="Copied order results" className="space-y-1 rounded-md border border-border p-3 text-xs">
    <p className="font-medium">{result.preview ? "Preview" : "Copied"} to {result.group} · {result.results.filter((r) => r.ok).length} of {result.results.length} accounts {result.preview ? "pass" : "accepted"}</p>
    <ul className="space-y-0.5">{result.results.map((r) => <li key={r.account} className={r.ok ? "" : "text-warn"}>
      <span className="tabular">{r.account} · {r.quantity}</span> — {message(r)}</li>)}</ul>
  </section>
}

/** Create, replace and delete account groups (admin scope). */
export function AccountGroupsEditor({ trading, accounts }: { trading: TradingStatus; accounts: AccountListItem[] }) {
  const api = useActionApi()
  const client = useQueryClient()
  const token = useWriteToken()
  const groups = useAccountGroups()
  const [name, setName] = useState("")
  const [allocation, setAllocation] = useState<"ratio" | "equity">("ratio")
  const [ratios, setRatios] = useState<Record<string, string>>({})
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<unknown>()
  const busy = useRef(false)
  const eligible = accounts.filter((a) => !a.archived && !a.sandbox_idle_seconds)
  const members = eligible.filter((a) => ratios[a.id] != null)
  const validName = /^[a-z0-9]+(-[a-z0-9]+)*$/.test(name) && name.length <= 40
  const validRatios = allocation === "equity" || members.every((m) => /^\d+(\.\d{1,4})?$/.test(ratios[m.id]!) && Number(ratios[m.id]) > 0 && Number(ratios[m.id]) <= 100)
  const blocked = pending || writeBlocked(trading, token)
  async function run(change: () => Promise<unknown>) {
    if (busy.current || blocked) return
    busy.current = true; setPending(true); setError(undefined)
    try { await change(); await client.invalidateQueries({ queryKey: ["account-groups"] }) }
    catch (failure) { setError(failure) }
    finally { busy.current = false; setPending(false) }
  }
  const edit = (group: AccountGroup) => {
    setName(group.name); setAllocation(group.allocation)
    setRatios(Object.fromEntries(group.members.map((m) => [m.account, m.ratio ?? "1"])))
  }
  return <section aria-label="Account groups" className="space-y-2">
    <h3 className="text-xs font-medium text-muted">Account groups</h3>
    <p className="text-xs text-muted">A ticket can copy one order to every account of a group. Each account submits it as its own order, with its own checks and journal. Changing groups needs an admin token.</p>
    <TradingError error={groups.error ?? error} />
    {(groups.data ?? []).map((group) => <div key={group.name} className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-border p-2 text-sm">
      <span>{group.name} <span className="text-xs text-muted">· {allocationText(group)}</span></span>
      <span className="flex gap-2">
        <button type="button" className="trade-button" disabled={blocked} onClick={() => edit(group)}>Edit {group.name}</button>
        <button type="button" className="trade-button text-danger" disabled={blocked}
          onClick={() => void run(() => api.deleteAccountGroup(group.name, trading.write))}>Delete {group.name}</button>
      </span>
    </div>)}
    <form className="space-y-2 rounded-md border border-border p-3" onSubmit={(event) => {
      event.preventDefault()
      void run(() => api.putAccountGroup(name, { allocation, members: members.map((m) => allocation === "ratio" ? { account: m.id, ratio: ratios[m.id]! } : { account: m.id }) }, trading.write))
    }}>
      <label className="text-sm">Group name<input className="trade-input" value={name} onChange={(event) => setName(event.target.value.toLowerCase())} placeholder="desk" /></label>
      <label className="text-sm">Allocation
        <select className="trade-input" value={allocation} onChange={(event) => setAllocation(event.target.value as "ratio" | "equity")}>
          <option value="ratio">Fixed ratio · order size × each ratio</option>
          <option value="equity">By equity · order size split by marked equity</option>
        </select>
      </label>
      <fieldset className="space-y-1"><legend className="text-xs text-muted">Members</legend>
        {eligible.map((a) => <div key={a.id} className="flex items-center gap-2 text-sm">
          <label className="flex flex-1 items-center gap-2"><input type="checkbox" checked={ratios[a.id] != null}
            onChange={(event) => setRatios((current) => { const next = { ...current }; if (event.target.checked) next[a.id] = "1"; else delete next[a.id]; return next })} />{a.name} <span className="text-xs text-muted">· {a.id}</span></label>
          {allocation === "ratio" && ratios[a.id] != null && <input aria-label={`Ratio for ${a.name}`} className="trade-input w-20" inputMode="decimal" value={ratios[a.id]}
            onChange={(event) => setRatios((current) => ({ ...current, [a.id]: event.target.value }))} />}
        </div>)}
      </fieldset>
      <button type="submit" className="trade-button" disabled={blocked || !validName || !members.length || members.length > 16 || !validRatios}>Save group</button>
    </form>
  </section>
}
