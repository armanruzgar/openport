import { useQuery } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { VolatilityScanner, VolatilityScannerRow } from "../api/types"
import { Empty, Panel } from "../components/ui"
import { metricMark, volPoints as vp } from "../lib/volatility"

type Sort = "symbol" | "iv30" | "iv_rank" | "iv_percentile" | "vrp" | "rr25" | "rr25_rank" | "term_ratio" | "today_percent" | "today_points"
const columns: { key: Sort; label: string; percent?: boolean }[] = [
  { key: "symbol", label: "Symbol" }, { key: "iv30", label: "IV30" }, { key: "iv_rank", label: "IV rank", percent: true },
  { key: "iv_percentile", label: "IV percentile", percent: true }, { key: "vrp", label: "IV − RV21" },
  { key: "rr25", label: "RR25" }, { key: "rr25_rank", label: "RR25 rank", percent: true },
  { key: "term_ratio", label: "9d/30d" }, { key: "today_percent", label: "Today %" }, { key: "today_points", label: "Today points" },
]
export function sortedScanner(rows: VolatilityScannerRow[], key: Sort, descending: boolean): VolatilityScannerRow[] {
  return [...rows].sort((a, b) => {
    const x = a[key], y = b[key]
    if (x == null) return y == null ? a.symbol.localeCompare(b.symbol) : 1
    if (y == null) return -1
    const order = typeof x === "number" && typeof y === "number" ? x - y : String(x).localeCompare(String(y))
    return (descending ? -order : order) || a.symbol.localeCompare(b.symbol)
  })
}
export function VolatilityScannerTable({ data, onSymbol }: { data: VolatilityScanner; onSymbol: (symbol: string) => void }) {
  const [sort, setSort] = useState<Sort>("symbol")
  const [descending, setDescending] = useState(false)
  return <Panel title="Cross-underlying volatility scanner">
    <p className="mb-3 text-xs text-muted">{data.simulated ? "Simulated data" : data.provider} · each row retains its own market timestamp.</p>
    <div className="overflow-auto"><table aria-label="Volatility scanner" className="w-full whitespace-nowrap text-right text-xs tabular">
      <thead className="text-muted"><tr>{columns.map(column => <th key={column.key} aria-sort={sort === column.key ? descending ? "descending" : "ascending" : "none"} className="px-3 py-2 font-normal">
        <button type="button" onClick={() => { setSort(column.key); setDescending(sort === column.key ? !descending : column.key !== "symbol") }} className="hover:text-foreground">{column.label}{sort === column.key ? descending ? " ↓" : " ↑" : ""}</button>
      </th>)}<th className="px-3 py-2 font-normal">Coverage / market time</th></tr></thead>
      <tbody>{sortedScanner(data.rows, sort, descending).map(row => <tr key={row.symbol} className="border-t border-border/40" title={row.reason?.replaceAll("_", " ")}>
        {columns.map(column => <td key={column.key} className="px-3 py-2">{column.key === "symbol" ? <button type="button" onClick={() => onSymbol(row.symbol)} className="text-accent underline decoration-dotted">{row.symbol}</button>
          : column.key === "iv30" ? `${vp(row.iv30)}${metricMark(row)}${row.iv_basis === "own_atm" ? " ATM" : ""}`
          : column.percent ? row[column.key] == null ? "—" : `${vp((row[column.key] as number) * 100, 1)}%`
          : vp(row[column.key] as number | null, column.key === "term_ratio" ? 3 : 2)}</td>)}
        <td className="px-3 py-2 text-muted"><div>{row.feed_state} · IV {row.history_sessions}/252 · RR {row.rr25_sessions}/252{row.proxy_sessions > 0 && ` · ${row.proxy_sessions} ${row.proxy_name} proxy`}</div><div>{row.as_of ?? "No snapshot"} · RV through {row.realized_as_of ?? "—"}</div></td>
      </tr>)}</tbody>
    </table></div>
    {!data.rows.length && <Empty>No subscribed symbols.</Empty>}
    <p className="mt-3 text-[11px] text-muted">Vol points; ranks are percentages. IV30 uses model-free IV, falling back to ATM when marked. IV − RV21 uses that same IV basis and completed close returns. Today requires an unexpired same-day settlement. † Truncated strip; ≈ proxy or smile interpolation. Missing values sort last in either direction.</p>
  </Panel>
}
export function VolatilityScannerView({ onSymbol }: { onSymbol: (symbol: string) => void }) {
  const live = useLive()
  const query = useQuery({ queryKey: ["volatility-scanner", live.source], queryFn: ({ signal }) => api.volatilityScanner(signal), refetchInterval: 60_000 })
  return query.data ? <VolatilityScannerTable data={query.data} onSymbol={onSymbol} /> : <Empty>{query.isError ? String(query.error) : "Loading volatility scanner…"}</Empty>
}
