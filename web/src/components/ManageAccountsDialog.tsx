import { useActionApi } from "../api/action-client"
import { useQuery, useQueryClient } from "@tanstack/react-query"
import { useRef, useState } from "react"

import { useLive } from "../api/live"
import type { AccountListItem, TradingStatus } from "../api/trading-types"
import { useWriteToken } from "../lib/write-token"
import { Dialog } from "./Dialog"
import { TradingError, WriteAccess, writeBlocked } from "./TradingControls"

export function ManageAccountsDialog({ trading, onClose }: { trading: TradingStatus; onClose: () => void }) {
  const api = useActionApi()
  const { account, switchAccount } = useLive()
  const client = useQueryClient()
  const token = useWriteToken()
  const list = useQuery({ queryKey: ["live-accounts"], queryFn: ({ signal }) => api.liveAccounts(signal), staleTime: 0 })
  const [editing, setEditing] = useState<AccountListItem>()
  const [name, setName] = useState("")
  const [deleting, setDeleting] = useState<AccountListItem>()
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<unknown>()
  const busy = useRef(false)
  const blocked = pending || writeBlocked(trading, token)
  async function run(id: string, change: () => Promise<unknown>, removeFromSwitcher = false) {
    if (busy.current || writeBlocked(trading, token)) return
    busy.current = true; setPending(true); setError(undefined)
    try {
      await change()
      if (removeFromSwitcher && account === id) switchAccount("main", "user")
      setEditing(undefined); setDeleting(undefined)
      await client.invalidateQueries({ queryKey: ["live-accounts"] })
    } catch (failure) { setError(failure) }
    finally { busy.current = false; setPending(false) }
  }
  return <Dialog title="Manage accounts" onClose={onClose}>
    <WriteAccess trading={trading} />
    <p className="text-xs text-muted">Archive freezes positions and working orders; unarchive resumes them. Account IDs stay fixed when renamed.</p>
    <TradingError error={list.error ?? error} />
    {list.isLoading && <p>Loading accounts…</p>}
    {[false, true].map((archived) => <section key={String(archived)} className="space-y-2" aria-label={archived ? "Archived accounts" : "Active accounts"}>
      <h3 className="text-xs font-medium text-muted">{archived ? "Archived accounts" : "Active accounts"}</h3>
      {(list.data?.accounts ?? []).filter((item) => Boolean(item.archived) === archived).map((item) => {
        const protectedAccount = item.id === "main" || !!item.sandbox_idle_seconds
        return <div key={item.id} className="space-y-2 rounded-md border border-border p-3">
          <p className="text-sm">{item.name} <span className="text-xs text-muted">· {item.id}</span></p>
          {item.trading.plan && <p className="text-xs text-muted">{item.trading.plan}{item.trading.plan_id ? ` · ${item.trading.plan_id}` : " · custom"}</p>}
          {!protectedAccount && <div className="flex flex-wrap gap-2">
            <button type="button" className="trade-button" disabled={blocked} onClick={() => { setEditing(item); setName(item.name); setDeleting(undefined) }}>Rename {item.name}</button>
            <button type="button" className="trade-button" disabled={blocked}
              onClick={() => void run(item.id, () => api.updateAccount(item.id, { archived: !archived }, trading.write), !archived)}>{archived ? "Unarchive" : "Archive"} {item.name}</button>
            <button type="button" className="trade-button text-danger" disabled={blocked} onClick={() => { setDeleting(item); setEditing(undefined) }}>Delete {item.name}</button>
          </div>}
        </div>
      })}
    </section>)}
    {editing && <form className="space-y-2" onSubmit={(event) => { event.preventDefault(); void run(editing.id, () => api.updateAccount(editing.id, { name: name.trim() }, trading.write)) }}>
      <label className="text-sm">New account name<input className="trade-input" value={name} onChange={(event) => setName(event.target.value)} /></label>
      <button type="submit" className="trade-button" disabled={blocked || !name.trim() || [...name.trim()].length > 64}>Save name</button>
    </form>}
    {deleting && <div className="space-y-2 rounded-md border border-danger p-3" role="alert">
      <p>Delete {deleting.name} ({deleting.id})? It will disappear permanently from the terminal. Its files are retained on the server.</p>
      <p className="text-xs text-muted">An account with positions or working orders must be archived first.</p>
      <div className="flex gap-2"><button type="button" className="trade-button text-danger" disabled={blocked}
        onClick={() => void run(deleting.id, () => api.deleteAccount(deleting.id, trading.write), true)}>Confirm delete</button>
        <button type="button" className="trade-button" disabled={pending} onClick={() => setDeleting(undefined)}>Cancel</button></div>
    </div>}
  </Dialog>
}
