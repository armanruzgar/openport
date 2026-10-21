import { useEffect, type ReactNode } from "react"
import { useLive } from "../api/live"
import { useAccount, usePortfolio } from "../api/trading"
import type { Account } from "../api/trading-types"
import { signedPercent } from "../lib/format"
import { payoutsVisible } from "../lib/payouts"
import { navigableViews, type View } from "../lib/route"
import { formatMoney, ratio, signedMoney, subtractMoney } from "../lib/trading"
import { AccountSwitcher } from "./AccountSwitcher"
import { Badge, Meter, toneOf, toneText } from "./ui"

export const viewLabels: Record<View, string> = {
  "vol-scanner": "Vol scanner",
  backtest: "Backtest", brief: "Brief", dashboard: "Dashboard", chain: "Trade", positions: "Positions", orders: "Orders", journal: "Journal",
  playbooks: "Playbooks", rules: "Rules", payouts: "Payouts", smile: "Volatility", exposure: "Exposure", replay: "Replay", engine: "Status",
}

const icons: Partial<Record<View, ReactNode>> = {
  backtest: <path d="M4 20V4m0 16h16M8 16v-5m5 5V7m5 9V4" />,
  brief: <path d="M5 3h14v18H5zM8 7h8M8 11h8M8 15h5" />,
  dashboard: <path d="M4 13h6V4H4zm10 7h6v-9h-6zM4 20h6v-4H4zm10-11h6V4h-6z" />,
  chain: <path d="M4 18V6m0 12h16M8 14l3-3 3 2 5-6" />,
  positions: <path d="M12 3 3 8l9 5 9-5-9-5Zm-9 9 9 5 9-5M3 16l9 5 9-5" />,
  orders: <path d="M8 6h12M8 12h12M8 18h12M4 6h.01M4 12h.01M4 18h.01" />,
  journal: <path d="M4 5h16v15H4zM4 10h16M9 3v4m6-4v4" />,
  rules: <path d="M12 3 5 6v5c0 4.5 3 8 7 10 4-2 7-5.5 7-10V6l-7-3Zm-3 9 2 2 4-4" />,
  playbooks: <path d="M4 5c3-1 5-1 8 1 3-2 5-2 8-1v14c-3-1-5-1-8 1-3-2-5-2-8-1zM12 6v14" />,
  payouts: <path d="M3 7h18v11H3zM3 11h18M7 15h4m6 0h.01M6 7l9-3 1 3" />,
  replay: <path d="M4 12a8 8 0 1 0 2.3-5.7M4 4v3h3M10 9l5 3-5 3z" />,
  engine: <path d="M3 12h4l3-8 4 16 3-8h4" />,
}
function Icon({ view }: { view: View }) {
  return <svg viewBox="0 0 24 24" className="h-4 w-4 shrink-0" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">{icons[view]}</svg>
}

export function evaluationBadge(account: Pick<Account, "evaluation" | "rules"> | undefined, plan?: string | null) {
  if (!account) return null
  const { evaluation } = account
  if (!evaluation.enabled) return <Badge tone="neutral">{plan ? "Practice" : "Paper"}</Badge>
  if (evaluation.status === "active" && evaluation.day_lock) return <Badge tone="warn" title="A plan limit locked trading until the next trading day">locked today</Badge>
  if (account.rules.phase === "funded" && evaluation.status === "active") return <Badge tone="positive">funded</Badge>
  const tone = evaluation.status === "passed" ? "positive" : evaluation.status === "failed" ? "negative" : "accent"
  return <Badge tone={tone}>{evaluation.status}</Badge>
}

/** Always-visible account figures, like a broker's account strip. */
export function AccountSummary() {
  const account = useAccount().data
  const portfolio = usePortfolio().data
  if (!account) return <p className="px-1 text-xs text-faint">Loading account…</p>
  const e = account.evaluation
  const total = subtractMoney(e.equity, e.starting_balance)
  const change = ratio(total, e.starting_balance)
  const target = e.target_equity != null ? ratio(e.profit, subtractMoney(e.target_equity, e.starting_balance)) : null
  const buffer = e.floor != null && e.drawdown_buffer != null
    ? ratio(e.drawdown_buffer, subtractMoney(e.peak, e.floor)) : null
  const payout = account.payout
  const rows: [string, string | null | undefined, boolean?][] = [
    ["Buying power", account.buying_power.available],
    ["Unrealized", portfolio?.unrealised, true],
    ["Realized", portfolio?.realised, true],
    ["Fees", portfolio ? `-${portfolio.fees}` : null],
  ]
  return (
    <section aria-label="Account summary" className="space-y-3">
      <div className="flex items-center justify-between gap-2">
        <span className="truncate text-xs font-medium" title={account.rules.plan ?? "Paper account"}>{account.rules.plan ?? "Paper account"}</span>
        {evaluationBadge(account, account.rules.plan)}
      </div>
      <div>
        <div className="text-[10px] uppercase tracking-wide text-muted">Equity</div>
        <div className="text-lg font-medium tabular">{formatMoney(e.equity)}</div>
        <div className={`text-xs tabular ${toneText[toneOf(total)]}`}>
          {signedMoney(total)}{change != null && ` (${signedPercent(change, 2)})`}
          <span className="text-faint"> total</span>
        </div>
      </div>
      <dl className="space-y-1 text-[11px]">
        {rows.map(([label, value, signed]) => (
          <div key={label} className="flex justify-between gap-2">
            <dt className="text-muted">{label}</dt>
            <dd className={`tabular ${signed ? toneText[toneOf(value)] : ""}`}>{signed ? signedMoney(value) : formatMoney(value)}</dd>
          </div>
        ))}
      </dl>
      {e.enabled && (
        <div className="space-y-2">
          {target != null && <div>
            <div className="mb-1 flex justify-between text-[10px] uppercase tracking-wide text-muted"><span>Target</span><span className="tabular">{Math.max(0, target * 100).toFixed(0)}%</span></div>
            <Meter value={target} tone="positive" label="Progress to profit target" />
          </div>}
          {payout && <div>
            <div className="mb-1 flex justify-between text-[10px] uppercase tracking-wide text-muted"><span>Payout days</span><span className="tabular">{payout.qualifying_days} / {payout.required_days}</span></div>
            <Meter value={payout.qualifying_days / payout.required_days} tone="positive" label="Qualifying days toward the next payout" />
          </div>}
          {buffer != null && <div>
            <div className="mb-1 flex justify-between text-[10px] uppercase tracking-wide text-muted"><span>Floor buffer</span><span className="tabular">{formatMoney(e.drawdown_buffer)}</span></div>
            <Meter value={buffer} tone={buffer < 0.25 ? "negative" : buffer < 0.5 ? "warn" : "positive"} label="Buffer above the drawdown floor" />
          </div>}
        </div>
      )}
    </section>
  )
}

export function Sidebar({ view, onView, open, onClose }: { view: View; onView: (view: View) => void; open: boolean; onClose: () => void }) {
  const { trading } = useLive()
  const account = useAccount().data
  const current = view === "smile" || view === "exposure" || view === "vol-scanner" ? "chain" : view
  const items = navigableViews(trading != null, payoutsVisible(account))
  useEffect(() => {
    if (!open) return
    const onKey = (event: KeyboardEvent) => { if (event.key === "Escape") onClose() }
    window.addEventListener("keydown", onKey)
    return () => window.removeEventListener("keydown", onKey)
  }, [open, onClose])
  const link = (v: View, index?: number) => (
    <button key={v} type="button" onClick={() => { onView(v); onClose() }} aria-current={current === v ? "page" : undefined}
      title={index != null ? `${viewLabels[v]} (${index + 1})` : viewLabels[v]}
      className={`flex w-full items-center gap-2.5 rounded-md px-2.5 py-1.5 text-sm transition-colors ${
        current === v ? "bg-raised text-foreground" : "text-muted hover:bg-raised/60 hover:text-foreground"}`}>
      <Icon view={v} />{viewLabels[v]}
    </button>
  )
  return (
    <>
      {open && <div className="fixed inset-0 z-30 bg-black/50 lg:hidden" onClick={onClose} aria-hidden="true" />}
      <aside aria-label="Navigation" className={`fixed inset-y-0 left-0 z-40 flex w-60 flex-col border-r border-border bg-panel max-lg:transition-transform lg:sticky lg:top-0 lg:h-dvh lg:translate-x-0 ${open ? "translate-x-0" : "-translate-x-full"}`}>
        <a href="#/" className="flex items-center gap-2 border-b border-border px-4 py-3 font-semibold tracking-tight">
          <svg viewBox="0 0 32 32" className="h-5 w-5 text-accent" aria-hidden="true">
            <rect width="32" height="32" rx="7" className="fill-raised" />
            <path d="M6 22 C11 22 12 10 16 10 C20 10 21 22 26 22" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" />
            <circle cx="16" cy="10" r="2.2" fill="currentColor" />
          </svg>
          OpenPort
        </a>
        <nav className="flex-1 space-y-4 overflow-y-auto px-2 py-3" aria-label="Pages">
          <div className="space-y-0.5">
            <div className="px-2.5 pb-1 text-[10px] font-medium uppercase tracking-wider text-faint">{trading ? "Account" : "Market"}</div>
            {items.map((v, index) => link(v, index))}
          </div>
          <div className="space-y-0.5">
            <div className="px-2.5 pb-1 text-[10px] font-medium uppercase tracking-wider text-faint">System</div>
            {link("replay")}
            {trading && link("backtest")}
            {link("engine")}
          </div>
        </nav>
        {trading && <div className="space-y-3 border-t border-border p-3"><AccountSwitcher /><AccountSummary /></div>}
      </aside>
    </>
  )
}
