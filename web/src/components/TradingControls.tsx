import { useRef, useState } from "react"
import { api, ApiError } from "../api/client"
import { activeAccount } from "../lib/active-account"
import type { Status } from "../api/types"
import type { TradingStatus } from "../api/trading-types"
import { fixed } from "../lib/format"
import { useWriteToken, writeToken } from "../lib/write-token"
import { Dialog } from "./Dialog"

export function TradingError({ error }: { error: unknown }) {
  if (!error) return null
  return <div role="alert" className="rounded-md border border-danger/50 p-3 text-sm text-danger [overflow-wrap:anywhere]">
    {error instanceof ApiError ? <>
      <strong>{error.code}</strong>: {error.message}
      {(error.actual != null || error.limit != null) && <div className="mt-1 tabular">Actual {typeof error.actual === "string" ? error.actual : fixed(error.actual, 2)} · Limit {typeof error.limit === "string" ? error.limit : fixed(error.limit, 2)}{error.scope ? ` · ${error.scope}` : ""}</div>}
    </> : error instanceof Error || error instanceof DOMException ? error.message : "Request failed. Please retry."}
  </div>
}

export function WriteAccess({ trading }: { trading?: TradingStatus }) {
  const token = useWriteToken()
  const [open, setOpen] = useState(false)
  const [value, setValue] = useState("")
  const [storageWarning, setStorageWarning] = useState(false)
  if (trading?.write === "disabled") return <p className="text-xs text-warn">Read only · trading writes disabled by server</p>
  if (trading && trading.write !== "token") return null
  return <>
    <button type="button" className="trade-button" onClick={() => { setValue(""); setOpen(true) }}>{token ? "Manage write token" : "Enter write token"}</button>
    {storageWarning && <p className="text-xs text-warn">Session storage unavailable; token kept in memory until this page closes.</p>}
    {open && <Dialog title="Trading write token" onClose={() => setOpen(false)}>
      <p className="text-sm text-muted">Enter the token configured on your server. It is kept only for this browser session.</p>
      <form className="space-y-3" onSubmit={(event) => { event.preventDefault(); setStorageWarning(!writeToken.set(value)); setValue(""); setOpen(false) }}>
        <label className="trade-label">Write token<input autoFocus type="password" autoComplete="off" spellCheck={false} className="trade-input" value={value} onChange={(event) => setValue(event.target.value)} required /></label>
        <div className="flex flex-wrap gap-2">
          <button className="trade-button" type="submit" disabled={!value.trim()}>Save token</button>
          <button className="trade-button" type="button" onClick={() => { setStorageWarning(!writeToken.set("")); setValue(""); setOpen(false) }}>Clear token</button>
        </div>
      </form>
    </Dialog>}
  </>
}

export function writeBlocked(trading: TradingStatus, token: string) {
  return !trading.enabled || trading.write === "disabled" || (trading.write === "token" && !token)
}

/** One notice in the terminal shell, shared by all trading panels. */
export function WatchOnlyNotice({ trading, sandboxes, onCreated }: {
  trading?: TradingStatus | null; sandboxes?: Status["sandboxes"]; onCreated?: (account: string) => void
}) {
  const token = useWriteToken()
  const busy = useRef(false)
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<unknown>()
  const [storageWarning, setStorageWarning] = useState(false)
  async function create() {
    if (busy.current) return
    busy.current = true
    setPending(true)
    setError(undefined)
    try {
      const result = await api.createSandbox()
      activeAccount.set(result.account, "user")
      setStorageWarning(!writeToken.set(result.token, result.account))
      onCreated?.(result.account)
    } catch (failure) { setError(failure) }
    finally { busy.current = false; setPending(false) }
  }
  if (token && writeToken.sandboxAccount() && storageWarning) return <p role="status" className="border-b border-border px-4 py-2 text-xs text-warn">Session storage unavailable; sandbox token kept in memory until this page closes.</p>
  if (trading?.write !== "token" || token) return null
  return <div role="status" aria-label="Watch only" className="flex flex-wrap items-center gap-3 border-b border-border bg-raised px-4 py-2 text-sm text-muted">
    <span>{sandboxes?.enabled ? "Watch only. Try trading on simulated prices in your own sandbox account." : "Watch only. Trading on this server needs its write token."}</span>
    {sandboxes?.enabled && <button type="button" className="trade-button" disabled={pending} onClick={() => void create()}>
      {pending ? "Creating sandbox…" : "Try trading with a sandbox account"}
    </button>}
    <TradingError error={error} />
    <WriteAccess trading={trading} />
  </div>
}
