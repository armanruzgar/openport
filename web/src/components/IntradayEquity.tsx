import { scenarioMarkers } from "../lib/practice"
import { useQuery } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { Account } from "../api/trading-types"
import { LineChart } from "../charts/LineChart"
import { intradayEquity } from "../lib/equity"
import { money } from "../lib/format"
import { TradingError } from "./TradingControls"
import { Panel } from "./ui"

const clock = new Intl.DateTimeFormat("en-US", { hour: "numeric", minute: "2-digit", timeZone: "America/New_York" })
export function IntradayEquity({ account }: { account: Account }) {
  const { accountScope, trading, replay } = useLive()
  const history = useQuery({ queryKey: ["trading", accountScope, "equity", trading?.account_version], queryFn: ({ signal }) => api.equity(signal),
    enabled: !!trading?.enabled || !!account.damaged, refetchInterval: 30_000, retry: false })
  const [selected, setSelected] = useState("")
  const day = selected || account.evaluation.day
  const samples = history.data?.samples ?? []
  const days = [...new Set([account.evaluation.day, ...samples.map((s) => s.day)])].sort().reverse()
  const chart = intradayEquity(samples, day, day === account.evaluation.day ? account.evaluation.attempt : undefined)
  const points = chart.series.flatMap(s => s.points.map(p => p.x))
  chart.markers.push(...scenarioMarkers(replay?.events ?? [], Math.min(...points), Math.max(...points)))
  return <Panel title="Intraday equity" actions={<label className="text-xs text-muted">Day <select aria-label="Equity day" className="trade-input" value={day} onChange={(event) => setSelected(event.target.value)}>{days.map((date) => <option key={date}>{date}</option>)}</select></label>}>
    <TradingError error={history.error} />
    {history.data?.error && <p className="text-xs text-warn">Equity history storage: {history.data.error}{history.data.error_time ? ` · ${history.data.error_time}` : ""}{history.data.error_market_time ? ` · market ${history.data.error_market_time}` : ""}{history.data.error_recovered ? " · storage recovered" : " · storage not recovered"}</p>}
    {chart.series.length ? <LineChart {...chart} height={280} marginLeft={70} formatX={(x) => clock.format(x)} formatY={money} />
      : <p className="py-8 text-center text-sm text-muted">{history.isLoading ? "Loading equity history…" : "No persisted equity samples for this day."}</p>}
    <p className="mt-2 text-[11px] text-muted">Paper account equity · one-minute market-time marks and fills. Gaps remain where marks were unavailable or the server was down. Dashed lines show the plan floor, target and, for end-of-day plans, tomorrow's floor if the day closed now.</p>
  </Panel>
}
