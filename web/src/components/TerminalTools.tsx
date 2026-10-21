import { useState } from "react"
import { captureWorkspace, loadWorkspaces, restoreWorkspace, saveWorkspaces } from "../lib/chain-prefs"
import { type Route, formatRoute } from "../lib/route"
import { Dialog } from "./Dialog"

export function HotkeyHelp({ onClose }: { onClose: () => void }) {
  return <Dialog title="Keyboard shortcuts" onClose={onClose}>
    <dl className="grid grid-cols-[auto_1fr] gap-x-5 gap-y-3 text-sm">
      {[["?", "Show this help"], ["Ctrl / ⌘ K", "Search subscribed symbols"], ["1–9", "Switch sidebar page"],
        ["← / →", "Previous / next expiry outside the chain"], ["↑ / ↓", "Move through strikes in the focused chain"],
        ["← / → in chain", "Select calls / puts"], ["B / S", "Buy / sell focused contract; prefill the ticket"],
        ["Enter on a quote", "Open the ticket for that bid or ask"], ["Enter in a ticket", "From a non-editable control, focus the order review; in a field, submit"], ["Esc", "Cancel the ticket or close a dialog"],
        ["F on a position", "Open its existing close confirmation"]].map(([key, text]) => <div key={key} className="contents"><dt><kbd className="tabular">{key}</kbd></dt><dd>{text}</dd></div>)}
    </dl>
    <p className="text-xs text-muted">Trading shortcuts are inactive inside inputs. Tab into a chain or position to select it. Orders and fills are simulated.</p>
  </Dialog>
}
export function Workspaces({ route }: { route: Route }) {
  const [open, setOpen] = useState(false)
  const [list, setList] = useState(loadWorkspaces)
  const [name, setName] = useState("")
  return <>
    <button className="trade-button" onClick={() => { setList(loadWorkspaces()); setOpen(true) }}>Workspaces</button>
    {open && <Dialog title="Saved workspaces" onClose={() => setOpen(false)}>
      <p className="text-xs text-muted">Save this symbol, page, expiry, chain preferences and chart studies in this browser. Saving an existing name replaces it.</p>
      <form className="flex gap-2" onSubmit={(event) => {
        event.preventDefault()
        if (!name.trim() || !route.symbol) return
        const next = [captureWorkspace(name, route), ...list.filter((w) => w.name !== name.trim())].slice(0, 20)
        setList(next); saveWorkspaces(next); setName("")
      }}>
        <input className="trade-input" aria-label="Workspace name" value={name} maxLength={60} onChange={(e) => setName(e.target.value)} />
        <button className="trade-button" disabled={!name.trim() || !route.symbol}>Save</button>
      </form>
      <ul className="space-y-2">{list.map((w) => <li key={w.name} className="flex items-center justify-between gap-2">
        <button className="min-w-0 break-words text-left text-sm text-accent" onClick={() => { restoreWorkspace(w); setOpen(false) }}>{w.name} <span className="text-xs text-muted">{w.route.symbol} · {w.route.view}</span></button>
        <button className="trade-button" aria-label={`Delete workspace ${w.name}`} onClick={() => { const next = list.filter((item) => item.name !== w.name); setList(next); saveWorkspaces(next) }}>Delete</button>
      </li>)}</ul>
    </Dialog>}
  </>
}
export function Popout({ route, label }: { route: Route; label: string }) {
  // A same-origin pop-out keeps its opener so the browser copies this tab's sessionStorage,
  // write token included; noopener (which noreferrer implies) would leave it view-only.
  return <a className="trade-button" href={formatRoute(route)} target="_blank" rel="opener" aria-label={`Open ${label} in new window`}
    onClick={(event) => { event.preventDefault(); window.open(formatRoute(route), "_blank", "popup,width=1200,height=900") }}>↗ {label}</a>
}
