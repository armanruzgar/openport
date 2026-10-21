import { useQuery, useQueryClient } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { EarningsMove } from "../api/types"
import { fixed, pct } from "../lib/format"
import { useWriteToken } from "../lib/write-token"
import { Badge, Empty, Panel } from "./ui"

const signedPct = (x: number | null) => (x == null ? "—" : `${x > 0 ? "+" : ""}${pct(x, 1)}`)

/** Over or under: whether the straddle priced a larger move than came. */
export function pricedText(move: EarningsMove): string {
  if (move.priced == null) return move.implied == null ? "waiting for the straddle" : "waiting for the close"
  return move.priced === "over" ? "over-priced" : "under-priced"
}

/** Earnings implied versus realized moves for one underlying, with an import for past quarters. */
export function EarningsPanel({ symbol }: { symbol: string }) {
  const queryClient = useQueryClient()
  const trading = useLive().status?.trading
  const token = useWriteToken()
  const mode = trading?.write ?? "disabled"
  const history = useQuery({
    queryKey: ["earnings", symbol],
    queryFn: ({ signal }) => api.earnings(symbol, signal),
    refetchInterval: 60_000,
  })
  const [open, setOpen] = useState(false)
  const [csv, setCsv] = useState("")
  const [result, setResult] = useState<string | null>(null)
  const [pending, setPending] = useState(false)
  const blocked = mode === "disabled" || (mode === "token" && !token)
  async function submit() {
    if (!csv.trim() || pending || blocked) return
    setPending(true)
    setResult(null)
    try {
      const imported = await api.importEarnings(csv, mode)
      setResult(`Imported ${imported.added}${imported.skipped.length ? `; skipped ${imported.skipped.join("; ")}` : ""}.`)
      setCsv("")
      await queryClient.invalidateQueries({ queryKey: ["earnings"] })
    } catch (error) {
      setResult(String(error instanceof Error ? error.message : error))
    } finally {
      setPending(false)
    }
  }
  const data = history.data
  const summary = data?.summary
  return (
    <Panel title="Earnings: implied vs realized" className="xl:col-span-2"
      actions={<button type="button" className="text-xs text-muted underline decoration-dotted hover:text-foreground" onClick={() => setOpen(!open)}>
        {open ? "Close import" : "Import past events"}</button>}>
      {history.isError ? <Empty>{String(history.error)}</Empty> : !data ? <Empty>Loading earnings history…</Empty> : <>
        {summary && summary.events > 0 && <p className="mb-2 text-xs">
          The straddle priced more than the realized move in <span className="font-medium tabular">{summary.over} of {summary.events}</span> events
          ({pct(summary.over_rate, 0)}); average implied {pct(summary.mean_implied, 1)} against realized {pct(summary.mean_realized, 1)}.
          {summary.imported > 0 && <span className="text-muted"> {summary.imported} imported.</span>}
        </p>}
        {data.moves.length ? (
          <div className="max-h-72 overflow-auto">
            <table className="w-full text-xs tabular">
              <thead className="sticky top-0 bg-panel text-muted"><tr>
                <th className="py-1 text-left font-normal">Event</th><th className="py-1 text-right font-normal">Implied</th>
                <th className="py-1 text-right font-normal">Realized</th><th className="py-1 text-left font-normal pl-3">Straddle</th>
                <th className="py-1 text-left font-normal pl-3">Priced</th><th className="py-1 text-left font-normal pl-3">Source</th>
              </tr></thead>
              <tbody>{data.moves.map((m) => (
                <tr key={`${m.symbol}-${m.date}`} className="border-t border-border/50">
                  <td className="py-1 whitespace-nowrap">{m.date} {m.session === "after_close" ? "after close" : "before open"}{m.label ? ` · ${m.label}` : ""}</td>
                  <td className="py-1 text-right">{m.implied == null ? "—" : pct(m.implied, 1)}</td>
                  <td className="py-1 text-right">{signedPct(m.realized)}</td>
                  <td className="py-1 pl-3 whitespace-nowrap text-muted">{m.straddle != null ? `${fixed(m.strike, 0)} straddle ${fixed(m.straddle, 2)} · ${m.expiry}` : "—"}</td>
                  <td className={`py-1 pl-3 ${m.priced === "over" ? "text-bearish" : m.priced === "under" ? "text-bullish" : "text-muted"}`}>{pricedText(m)}</td>
                  <td className="py-1 pl-3">{m.source === "imported" ? <Badge tone="warn" title="Entered from a CSV, not measured here">imported</Badge> : <Badge>recorded</Badge>}</td>
                </tr>))}</tbody>
            </table>
          </div>
        ) : <Empty>No earnings events for {symbol} yet. Events come from the daemon's --event-calendar; import past ones below.</Empty>}
        {data.error && <p className="mt-2 text-xs text-warn">{data.error}</p>}
      </>}
      {open && <div className="mt-3 space-y-2 border-t border-border pt-3">
        <label className="block text-xs text-muted" htmlFor="earnings-csv">
          CSV with the header <code>symbol,date,session,implied_move,realized_move,label</code>; moves in percent (session before_open or after_close).
        </label>
        <textarea id="earnings-csv" value={csv} onChange={(e) => setCsv(e.target.value)} rows={4}
          placeholder={"symbol,date,session,implied_move,realized_move,label\nAAPL,2026-07-30,after_close,4.1,-2.3,Q3"}
          className="w-full rounded-md border border-border bg-background p-2 font-mono text-xs" />
        <div className="flex items-center gap-2">
          <input type="file" accept=".csv,text/csv" aria-label="Choose an earnings CSV" className="text-xs"
            onChange={async (e) => { const file = e.target.files?.[0]; if (file) setCsv(await file.text()) }} />
          <button type="button" disabled={blocked || pending || !csv.trim()} onClick={() => void submit()}
            className="rounded-md bg-accent px-3 py-1 text-xs text-background disabled:opacity-50">{pending ? "Importing…" : "Import"}</button>
          {blocked && <span className="text-xs text-muted">Importing needs write access.</span>}
        </div>
        {result && <p role="status" className="text-xs">{result}</p>}
      </div>}
      <p className="mt-2 text-[11px] text-muted">{data?.model ?? ""}</p>
    </Panel>
  )
}
