import { useCallback, useEffect, useState } from "react"
import { loadChartPrefs, saveChartPrefs, type ChartPrefs } from "./candles"
import { formatRoute, parseRoute, type Route } from "./route"

export interface ChainPrefs {
  window: number
  greeks: boolean
  liquidity: boolean
  mode: "single" | "strategy"
  stacked: boolean
}
export const defaultChainPrefs: ChainPrefs = { window: .05, greeks: false, liquidity: true, mode: "single", stacked: false }
const key = (symbol: string) => `openport.chain.${symbol.toUpperCase()}`
function validated(saved: Partial<ChainPrefs> | null): ChainPrefs {
  return {
    window: [0, .02, .05, .1].includes(saved?.window ?? -1) ? saved!.window! : .05,
    greeks: saved?.greeks === true, liquidity: saved?.liquidity !== false,
    mode: saved?.mode === "strategy" ? "strategy" : "single", stacked: saved?.stacked === true,
  }
}
export function loadChainPrefs(symbol: string): ChainPrefs {
  try { return validated(JSON.parse(localStorage.getItem(key(symbol)) ?? "null")) } catch { return { ...defaultChainPrefs } }
}
export function saveChainPrefs(symbol: string, prefs: ChainPrefs) {
  try { localStorage.setItem(key(symbol), JSON.stringify(prefs)) } catch { /* Keep the in-memory choice. */ }
}
export function useChainPrefs(symbol: string) {
  const [state, setState] = useState(() => ({ symbol, prefs: loadChainPrefs(symbol) }))
  const prefs = state.symbol === symbol ? state.prefs : loadChainPrefs(symbol)
  useEffect(() => {
    const reload = () => setState({ symbol, prefs: loadChainPrefs(symbol) })
    reload()
    window.addEventListener("openport:workspace", reload)
    window.addEventListener("storage", reload)
    return () => { window.removeEventListener("openport:workspace", reload); window.removeEventListener("storage", reload) }
  }, [symbol])
  const update = useCallback((patch: Partial<ChainPrefs>) => setState((current) => {
    const next = { ...(current.symbol === symbol ? current.prefs : loadChainPrefs(symbol)), ...patch }
    saveChainPrefs(symbol, next)
    return { symbol, prefs: next }
  }), [symbol])
  return [prefs, update] as const
}

export interface Workspace { name: string; route: Route; chain: ChainPrefs; chart: ChartPrefs }
const workspaceKey = "openport.workspaces"
export function loadWorkspaces(): Workspace[] {
  try {
    const saved: unknown = JSON.parse(localStorage.getItem(workspaceKey) ?? "[]")
    if (!Array.isArray(saved)) return []
    return saved.filter((w) => w && typeof w.name === "string" && typeof w.route?.symbol === "string" && w.chart && w.chain)
      .slice(0, 20).map((w) => ({ name: w.name.slice(0, 60), route: parseRoute(formatRoute(w.route)), chain: validated(w.chain), chart: w.chart }))
  } catch { return [] }
}
export function saveWorkspaces(workspaces: Workspace[]) {
  try { localStorage.setItem(workspaceKey, JSON.stringify(workspaces.slice(0, 20))) } catch { /* The current session still works. */ }
}
export function captureWorkspace(name: string, route: Route): Workspace {
  return { name: name.trim().slice(0, 60), route, chain: loadChainPrefs(route.symbol ?? ""), chart: loadChartPrefs() }
}
export function restoreWorkspace(workspace: Workspace) {
  if (workspace.route.symbol) saveChainPrefs(workspace.route.symbol, workspace.chain)
  saveChartPrefs(workspace.chart)
  window.dispatchEvent(new Event("openport:workspace"))
  window.location.hash = formatRoute(workspace.route)
}
