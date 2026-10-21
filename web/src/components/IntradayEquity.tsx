import { useQuery } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { Account, EquityRisk } from "../api/trading-types"
import { LineChart } from "../charts/LineChart"
import { intradayEquity, riskHistory } from "../lib/equity"
import { money } from "../lib/format"
import { TradingError } from "./TradingControls"
import { Panel } from "./ui"

const clock = new Intl.DateTimeFormat("en-US", { hour: "numeric", minute: "2-digit", timeZone: "America/New_York" })
export function IntradayEquity({ account }: { account: Account }) {
  const { accountScope, trading } = useLive()
  const history = useQuery({ queryKey: ["trading", accountScope, "equity", trading?.account_version], queryFn: ({ signal }) => api.equity(signal),
    enabled: !!trading?.enabled || !!account.damaged, refetchInterval: 30_000, retry: false })
  const [selected, setSelected] = useState("")
  const [metric, setMetric] = useState<keyof EquityRisk>("dollar_delta")
  const [allDays, setAllDays] = useState(false)
  const day = selected || account.evaluation.day
  const samples = history.data?.samples ?? []
  const days = [...new Set([account.evaluation.day, ...samples.map((s) => s.day)])].sort().reverse()
  const chart = intradayEquity(samples, day, day === account.evaluation.day ? account.evaluation.attempt : undefined)
  const greeks = riskHistory(samples, metric, allDays ? undefined : day)
  return <Panel title="Intraday equity" actions={<label className="text-xs text-muted">Day <select aria-label="Equity day" className="trade-input" value={day} onChange={(event) => setSelected(event.target.value)}>{days.map((date) => <option key={date}>{date}</option>)}</select></label>}>
    <TradingError error={history.error} />
    {history.data?.error && <p className="text-xs text-warn">Equity history storage: {history.data.error}{history.data.error_time ? ` · ${history.data.error_time}` : ""}{history.data.error_market_time ? ` · market ${history.data.error_market_time}` : ""}{history.data.error_recovered ? " · storage recovered" : " · storage not recovered"}</p>}
    {chart.series.length ? <LineChart {...chart} height={280} marginLeft={70} formatX={(x) => clock.format(x)} formatY={money} />
      : <p className="py-8 text-center text-sm text-muted">{history.isLoading ? "Loading equity history…" : "No persisted equity samples for this day."}</p>}
    <section className="mt-4 space-y-2">
      <div className="flex flex-wrap items-center gap-3"><label className="trade-label">Portfolio risk
        <select aria-label="Risk history metric" className="trade-input" value={metric} onChange={(e) => setMetric(e.target.value as keyof EquityRisk)}>
          {([['dollar_delta', 'Dollar delta'], ['dollar_gamma_1pct', 'Dollar gamma / 1%'], ['vega', 'Vega / point'], ['theta', 'Theta / day'],
            ['delta_utilisation', 'Delta utilisation'], ['vega_utilisation', 'Vega utilisation'], ['gamma_utilisation', 'Gamma utilisation'],
            ['theta_utilisation', 'Theta utilisation'], ['grid_utilisation', 'Grid-loss utilisation'], ['short_vega_utilisation', 'Short-vega utilisation']] as const).map(([value, label]) => <option key={value} value={value}>{label}</option>)}
        </select></label>
        <label className="flex gap-2 text-xs"><input type="checkbox" checked={allDays} onChange={(e) => setAllDays(e.target.checked)} />Across days</label>
      </div>
      {greeks.length ? <LineChart series={greeks} height={220} marginLeft={70}
        formatX={(x) => allDays ? new Date(x).toLocaleDateString("en-US", { timeZone: "America/New_York" }) : clock.format(x)}
        formatY={(y) => metric.endsWith("utilisation") ? `${(100 * y).toFixed(0)}%` : money(y)} />
        : <p className="text-xs text-muted">No persisted risk observations for this period.</p>}
      <p className="text-[11px] text-muted">Greeks at observed market-time marks; utilisation includes open-order reservations. Older rows and interim fills have no reconstructed Greeks.</p>
    </section>
    <p className="mt-2 text-[11px] text-muted">Paper account equity · one-minute market-time marks and fills. Gaps remain where marks were unavailable or the server was down. Dashed lines show the plan floor, target and, for end-of-day plans, tomorrow's floor if the day closed now.</p>
  </Panel>
}
