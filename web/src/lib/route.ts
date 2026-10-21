import { useCallback, useSyncExternalStore } from "react"

export const views = ["chart", "dashboard", "chain", "positions", "orders", "journal", "rules", "playbooks", "payouts", "smile", "exposure", "replay", "engine", "brief", "backtest", "vol-scanner"] as const
export type View = (typeof views)[number]
/** Sidebar order; number keys switch between them. */
export const primaryViews = ["dashboard", "chain", "positions", "orders", "journal", "rules", "playbooks", "brief", "payouts"] as const satisfies readonly View[]
/** Market analytics sit beside the chain as the Trade page's tabs. */
export const tradeViews = ["chain", "smile", "exposure", "vol-scanner"] as const satisfies readonly View[]
/** Pages that exist only with a paper-trading server. */
export const accountViews: readonly View[] = ["dashboard", "positions", "orders", "journal", "rules", "playbooks", "payouts"]
/** Sidebar pages that apply: account pages need trading, and Payouts is shown only when offered. */
export function navigableViews(trading: boolean, payouts: boolean): View[] {
  return primaryViews.filter((v) => (trading || !accountViews.includes(v)) && (v !== "payouts" || payouts))
}
/** Links from before the simulator redesign keep working. */
const aliases: Record<string, View> = { portfolio: "positions" }

export interface Route {
  symbol: string | null
  view: View
  expiry: string | null
}

/// "#/SPX/chain/2026-10-05PM" <-> { symbol, view, expiry }. Keeping state in the
/// hash makes every screen linkable and survives a reload.
export function parseRoute(hash: string): Route {
  const [symbol, view = "", expiry] = hash.replace(/^#\/?/, "").split("/")
  try {
    return {
      symbol: symbol ? decodeURIComponent(symbol).toUpperCase() : null,
      view: (views as readonly string[]).includes(view) ? (view as View) : aliases[view] ?? "dashboard",
      expiry: expiry ? decodeURIComponent(expiry) : null,
    }
  } catch {
    return { symbol: null, view: "dashboard", expiry: null }
  }
}

export function formatRoute(route: Route): string {
  const parts = [route.symbol ?? "", route.view]
  if (route.expiry) parts.push(route.expiry)
  return `#/${parts.map(encodeURIComponent).join("/")}`
}

function subscribe(callback: () => void) {
  window.addEventListener("hashchange", callback)
  return () => window.removeEventListener("hashchange", callback)
}

export function useRoute(): [Route, (patch: Partial<Route>) => void] {
  const hash = useSyncExternalStore(subscribe, () => window.location.hash, () => "")
  const route = parseRoute(hash)
  const navigate = useCallback((patch: Partial<Route>) => {
    const next = { ...parseRoute(window.location.hash), ...patch }
    const target = formatRoute(next)
    if (target !== window.location.hash) window.location.hash = target
  }, [])
  return [route, navigate]
}
