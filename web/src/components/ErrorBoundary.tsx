import { Component, type ReactNode } from "react"

/**
 * This browser's saved drafts and layout, which a page reads as it renders: what-if and
 * Analyze candidates, chart drawings, strategy presets, chain and chart settings and
 * workspaces. Accounts, alerts, rule packs and the write token are not among them.
 */
const savedLayout = ["openport.what-if.", "openport.analyze.", "openport.drawings.", "openport.strategy-presets.",
  "openport.chain.", "openport.chart", "openport.brief-levels", "openport.workspaces"]

/** Removes the saved drafts and layout; returns how many entries went. Nothing on the server changes. */
export function clearSavedLayout(storage: Storage = window.localStorage): number {
  const keys: string[] = []
  for (let i = 0; i < storage.length; i++) {
    const key = storage.key(i)
    if (key != null && savedLayout.some((prefix) => key.startsWith(prefix))) keys.push(key)
  }
  keys.forEach((key) => storage.removeItem(key))
  return keys.length
}

interface Props {
  children: ReactNode
  /** What failed, as the message names it: a page, or the whole terminal. */
  label: string
  /** A new key (another page or symbol) renders the children again. */
  resetKey?: unknown
}
interface State { error: Error | null; resetKey: unknown }

/**
 * A render error inside stays inside: React 19 unmounts the whole root on an uncaught
 * one, which would leave a blank page with no navigation, kill switch or flatten. This
 * shows the error, and ways to recover, in the failed part's place.
 */
export class ErrorBoundary extends Component<Props, State> {
  state: State = { error: null, resetKey: this.props.resetKey }

  static getDerivedStateFromError(error: unknown): Partial<State> {
    return { error: error instanceof Error ? error : new Error(String(error)) }
  }

  static getDerivedStateFromProps(props: Props, state: State): Partial<State> | null {
    return Object.is(props.resetKey, state.resetKey) ? null : { error: null, resetKey: props.resetKey }
  }

  render() {
    const { error } = this.state
    if (!error) return this.props.children
    const reset = () => {
      try { clearSavedLayout() } catch { /* storage unavailable: nothing saved to clear */ }
      this.setState({ error: null })
    }
    return <div role="alert" className="space-y-2 rounded-lg border border-danger/50 bg-danger/5 p-4 text-sm">
      <p className="font-medium text-danger">{this.props.label} could not be displayed.</p>
      <p className="text-xs text-muted [overflow-wrap:anywhere]">{error.message || error.name}</p>
      <div className="flex flex-wrap gap-2">
        <button type="button" className="trade-button" onClick={() => this.setState({ error: null })}>Try again</button>
        <button type="button" className="trade-button" onClick={() => window.location.reload()}>Reload</button>
        <button type="button" className="trade-button" onClick={reset}>Reset saved layout</button>
      </div>
      <p className="text-[11px] text-muted">Reset saved layout clears this browser's saved what-if and Analyze candidates, chart drawings,
        strategy presets, chain and chart settings and workspaces, then tries again. Accounts, orders and alerts on the server are not touched.</p>
    </div>
  }
}
