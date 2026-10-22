import { useQuery } from "@tanstack/react-query"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { HistoricalRisk, StressResult, TailRisk } from "../api/trading-types"
import { formatMoney } from "../lib/trading"
import { TradingError } from "./TradingControls"

const amount = (value: number | null) => value == null ? "—" : formatMoney(String(value))
function Tail({ label, risk }: { label: string; risk: TailRisk }) {
  return <section className="space-y-2">
    <h4 className="text-xs font-medium">{label} · {risk.observations} paired daily moves</h4>
    {risk.complete ? <><dl className="grid grid-cols-2 gap-2 text-xs tabular">
      {([["VaR 95%", risk.var95], ["ES 95%", risk.es95], ["VaR 99%", risk.var99], ["ES 99%", risk.es99]] as const).map(([label, value]) =>
        <div key={label}><dt className="text-muted">{label}</dt><dd>{amount(value)}</dd></div>)}
    </dl>{risk.tail99_reason && <p className="text-xs text-muted">99% unavailable: {risk.tail99_reason}.</p>}</> : <p className="text-xs text-muted">Unavailable: needs fresh valuations and at least 20 aligned daily moves{label.includes("ATM") ? " with observed ATM IV changes" : ""}.</p>}
    {risk.first && <p className="text-xs text-muted">{risk.first} – {risk.last}</p>}
  </section>
}
/** Stress rows without an account carry no floor or margin, and the panel leaves those columns out. */
export type StressRows = Pick<HistoricalRisk, "sticky_strike" | "atm_vol_change"> & {
  stress: (Omit<StressResult, "floor_breach" | "margin"> & Partial<Pick<StressResult, "floor_breach" | "margin">>)[]
}
export function StressResultPanel({ data, account = true }: { data: HistoricalRisk | StressRows; account?: boolean }) {
  return <div className="space-y-4">
    <section className="space-y-3 rounded-md border border-border p-3">
      <h3 className="text-xs font-medium">Historical simulation · 1-day VaR and expected shortfall</h3>
      <div className="grid gap-3 sm:grid-cols-2"><Tail label="Sticky strike" risk={data.sticky_strike} /><Tail label="ATM vol change" risk={data.atm_vol_change} /></div>
      <p className="text-xs text-muted">Derived from up to 252 stored closes; no distribution fit. The 95% figures need 20 aligned moves and the 99% figures 100. Loss estimates are not guarantees or loss limits.</p>
    </section>
    <section className="space-y-3 rounded-md border border-border p-3">
      <h3 className="text-xs font-medium">Historical stress</h3>
      <div className="overflow-x-auto"><table className="w-full text-xs tabular text-right">
        <thead className="text-muted"><tr><th className="text-left">Shock</th><th>P&amp;L</th>{account && <><th>Floor breach</th><th>Margin after</th></>}</tr></thead>
        <tbody>{data.stress.map((stress) => <tr key={stress.id} className="border-t border-border">
          <td className="py-2 text-left"><details><summary>{stress.name}</summary><p className="max-w-lg text-muted">{stress.from} → {stress.to} · {stress.label}. {stress.source_note}</p>
            {stress.sources.map((source, i) => <a key={source} href={source} target="_blank" rel="noreferrer" className="mr-2 underline">Source {i + 1}</a>)}
            {stress.missing_underlyings.length > 0 && <p className="text-warn">No shock for {stress.missing_underlyings.join(", ")}</p>}</details></td>
          <td>{amount(stress.pnl)}{stress.clamped ? "*" : ""}</td>{account && <><td>{stress.floor_breach == null ? "—" : stress.floor_breach ? "Yes" : "No"}</td>
          <td>{stress.margin == null ? "—" : formatMoney(stress.margin)}</td></>}
        </tr>)}</tbody>
      </table></div>
      <p className="text-xs text-muted">Current held book under historical index shocks, derived from public closes. Unsupported underlyings leave the whole result unavailable.{account ? " Margin excludes working orders." : ""} * volatility floored.</p>
    </section>
  </div>
}
export function HistoricalRiskPanel() {
  const { accountScope, trading } = useLive()
  const result = useQuery({ queryKey: ["trading", accountScope, "risk-history", trading?.account_version],
    queryFn: ({ signal }) => api.riskHistory(signal), retry: false })
  return <><TradingError error={result.error} />{result.data && <StressResultPanel data={result.data} />}</>
}
