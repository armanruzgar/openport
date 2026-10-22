import { useQuery, useQueryClient } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { AnalysisFormat, AnalysisHolding, AnalysisProfile } from "../api/types"
import { LineChart, type Series } from "../charts/LineChart"
import { ScenarioGrid } from "../components/ScenarioGrid"
import { StressResultPanel } from "../components/StressPanel"
import { Badge, Empty, PageHeader, Panel, Segmented, Stat } from "../components/ui"
import { count, fixed, money, price } from "../lib/format"
import { useWriteToken } from "../lib/write-token"

const statusText: Record<AnalysisHolding["status"], string> = {
  priced: "", unpriced: "no usable quote", no_market_data: "underlying not on this feed",
  not_in_chain: "not in the chain", expired: "expired",
}
const formats: { value: AnalysisFormat; label: string }[] = [
  { value: "auto", label: "Detect from the columns" }, { value: "generic", label: "Generic" }, { value: "tastytrade", label: "tastytrade" },
  { value: "schwab", label: "Schwab" }, { value: "ibkr", label: "IBKR" },
]
const curveColors = ["var(--chart-1)", "var(--chart-2)", "var(--chart-3)", "var(--chart-4)"]

/** P&L curves of an analysis profile, one per horizon. */
export function profileSeries(profile: AnalysisProfile): Series[] {
  return profile.curves.map((curve, index) => ({
    id: String(curve.days), label: curve.days === 0 ? "today" : `+${curve.days}d`, color: curveColors[index % curveColors.length]!,
    points: profile.percent.map((x, i) => ({ x, y: curve.pnl[i] ?? null })),
  }))
}

function ImportForm({ onImported }: { onImported: (id: string) => void }) {
  const trading = useLive().status?.trading
  const token = useWriteToken()
  const mode = trading?.write ?? "disabled"
  const blocked = mode === "disabled" || (mode === "token" && !token)
  const [name, setName] = useState("")
  const [format, setFormat] = useState<AnalysisFormat>("auto")
  const [csv, setCsv] = useState("")
  const [error, setError] = useState<string | null>(null)
  const [pending, setPending] = useState(false)
  async function submit() {
    if (blocked || pending || !name.trim() || !csv.trim()) return
    setPending(true)
    setError(null)
    try {
      const imported = await api.importPortfolio(name.trim(), csv, format, mode)
      setName("")
      setCsv("")
      onImported(imported.id)
    } catch (failure) {
      setError(failure instanceof Error ? failure.message : String(failure))
    } finally {
      setPending(false)
    }
  }
  return (
    <Panel title="Import positions">
      <div className="space-y-2 text-xs">
        <label className="block">
          <span className="text-muted">Name</span>
          <input value={name} onChange={(e) => setName(e.target.value)} maxLength={64} placeholder="IRA at Schwab"
            className="mt-1 w-full rounded-md border border-border bg-background px-2 py-1" />
        </label>
        <label className="block">
          <span className="text-muted">Export format</span>
          <select value={format} onChange={(e) => setFormat(e.target.value as AnalysisFormat)}
            className="mt-1 w-full rounded-md border border-border bg-background px-2 py-1">
            {formats.map((f) => <option key={f.value} value={f.value}>{f.label}</option>)}
          </select>
        </label>
        <input type="file" accept=".csv,text/csv" aria-label="Choose a positions CSV"
          onChange={async (e) => { const file = e.target.files?.[0]; if (file) { setCsv(await file.text()); if (!name) setName(file.name.replace(/\.csv$/i, "")) } }} />
        <textarea value={csv} onChange={(e) => setCsv(e.target.value)} rows={5} aria-label="Positions CSV"
          placeholder={"symbol,quantity,cost\nSPY   261218C00600000,2,4.10\nSPY,100,571.20"}
          className="w-full rounded-md border border-border bg-background p-2 font-mono" />
        <button type="button" disabled={blocked || pending || !name.trim() || !csv.trim()} onClick={() => void submit()}
          className="rounded-md bg-accent px-3 py-1 text-background disabled:opacity-50">{pending ? "Importing…" : "Import for analysis"}</button>
        {blocked && <p className="text-muted">Importing needs write access to this server.</p>}
        {error && <p role="alert" className="text-bearish">{error}</p>}
        <p className="text-[11px] text-muted">
          Generic files need symbol (OSI or ticker) and quantity columns, or underlying, expiry, strike, type and quantity;
          broker exports are read by their column names. Shorts are negative. Files stay on this server, up to 64 KiB.
        </p>
      </div>
    </Panel>
  )
}

function Portfolio({ id, onDeleted }: { id: string; onDeleted: () => void }) {
  const trading = useLive().status?.trading
  const token = useWriteToken()
  const mode = trading?.write ?? "disabled"
  const blocked = mode === "disabled" || (mode === "token" && !token)
  const [underlying, setUnderlying] = useState<string | null>(null)
  const detail = useQuery({ queryKey: ["analysis", id], queryFn: ({ signal }) => api.analysisPortfolio(id, signal), refetchInterval: 15_000 })
  const data = detail.data
  const held = [...new Set((data?.holdings ?? []).map((h) => h.underlying))]
  const reference = underlying && held.includes(underlying) ? underlying : held.length === 1 ? held[0]! : null
  const profile = useQuery({
    queryKey: ["analysis-profile", id, reference],
    queryFn: ({ signal }) => api.analysisProfile(id, { ...(reference ? { underlying: reference } : {}), days: [0, 7, 30] }, signal),
    enabled: data != null, refetchInterval: 15_000,
  })
  const grid = useQuery({ queryKey: ["analysis-grid", id], queryFn: ({ signal }) => api.analysisGrid(id, signal), enabled: data != null, refetchInterval: 15_000 })
  const stress = useQuery({ queryKey: ["analysis-stress", id], queryFn: ({ signal }) => api.analysisStress(id, signal), enabled: data != null, refetchInterval: 60_000 })
  const tail = useQuery({ queryKey: ["analysis-var", id], queryFn: ({ signal }) => api.analysisVar(id, signal), enabled: data != null, refetchInterval: 60_000 })
  if (detail.isError) return <Empty>{String(detail.error)}</Empty>
  if (!data) return <Empty>Loading the portfolio…</Empty>
  return (
    <div className="flex flex-col gap-3">
      <p role="note" className="rounded-md border border-accent/40 bg-accent/10 px-3 py-2 text-xs">
        <Badge tone="accent">analysis only</Badge> {data.note}
      </p>
      <div className="grid grid-cols-2 gap-2 sm:grid-cols-4">
        <Stat label="Market value" value={money(data.value)} hint="Option mids and underlying spots from your feed" />
        <Stat label="Unrealized" value={money(data.unrealised)} tone={(data.unrealised ?? 0) < 0 ? "negative" : "positive"} hint="Against the costs in the file, when every line has one" />
        <Stat label="Dollar delta" value={money(data.exposure.aggregate.dollar_delta)} hint="All held underlyings" />
        <Stat label="Vega" value={money(data.exposure.aggregate.vega)} hint="Dollars per vol point" />
      </div>
      {!data.complete && <p className="text-xs text-warn">Not valued: {data.unpriced.join(", ")}. Risk views leave them out and say they are incomplete.</p>}
      <Panel title={`Holdings · ${data.name}`} actions={
        <button type="button" disabled={blocked} className="text-xs text-muted underline decoration-dotted hover:text-bearish disabled:opacity-50"
          onClick={async () => { if (window.confirm(`Delete the analysis portfolio ${data.name}?`)) { await api.deletePortfolio(id, mode); onDeleted() } }}>Delete</button>}>
        <div className="max-h-96 overflow-auto">
          <table className="w-full text-xs tabular">
            <thead className="sticky top-0 bg-panel text-muted"><tr>
              <th className="py-1 text-left font-normal">Holding</th><th className="py-1 text-right font-normal">Qty</th>
              <th className="py-1 text-right font-normal">Cost</th><th className="py-1 text-right font-normal">Mark</th>
              <th className="py-1 text-right font-normal">Value</th><th className="py-1 text-right font-normal">Delta</th>
              <th className="py-1 text-right font-normal">Vega</th><th className="py-1 text-right font-normal">Theta</th>
            </tr></thead>
            <tbody>{data.holdings.map((h) => (
              <tr key={h.symbol} className="border-t border-border/50">
                <td className="py-1 whitespace-nowrap">{h.type === "shares" ? `${h.symbol} shares` : `${h.underlying} ${h.expiry} ${fixed(h.strike, 0)}${h.type === "call" ? "C" : "P"}`}
                  {h.status !== "priced" && <span className="ml-2 text-warn">{statusText[h.status]}</span>}</td>
                <td className="py-1 text-right">{count(h.quantity)}</td>
                <td className="py-1 text-right">{price(h.cost)}</td>
                <td className="py-1 text-right">{price(h.mark)}</td>
                <td className="py-1 text-right">{money(h.value)}</td>
                <td className="py-1 text-right">{fixed(h.delta, 1)}</td>
                <td className="py-1 text-right">{fixed(h.vega, 1)}</td>
                <td className="py-1 text-right">{fixed(h.theta, 1)}</td>
              </tr>))}</tbody>
          </table>
        </div>
        {data.skipped.length > 0 && <details className="mt-2 text-xs text-muted"><summary>{data.skipped.length} lines not imported</summary>
          <ul className="mt-1 list-disc pl-4">{data.skipped.map((line) => <li key={line}>{line}</li>)}</ul></details>}
        <p className="mt-2 text-[11px] text-muted">Delta is in shares, vega and theta in dollars per vol point and per day.</p>
      </Panel>
      <div className="grid gap-3 xl:grid-cols-2">
        <Panel title="Risk profile" actions={held.length > 1 && <Segmented label="Move" value={underlying ?? "benchmark"}
          options={[{ value: "benchmark", label: "SPY-weighted" }, ...held.map((s) => ({ value: s, label: s }))]}
          onChange={(v) => setUnderlying(v === "benchmark" ? null : v)} />}>
          {profile.isError ? <Empty>{String(profile.error)}</Empty> : !profile.data ? <Empty>Loading…</Empty> : (
            <>
              <LineChart series={profileSeries(profile.data)} height={260} formatX={(x) => `${x > 0 ? "+" : ""}${fixed(x, 0)}%`} formatY={(y) => money(y)}
                xLabel={`${profile.data.reference} move`} marginLeft={64} />
              {!profile.data.complete && <p className="text-xs text-warn">Leaves out {[...profile.data.unpriced,
                ...profile.data.betas.filter((b) => b.beta == null).map((b) => `${b.underlying} (no beta to ${profile.data.reference} yet)`)].join(", ") || "holdings it cannot value"}.</p>}
              <p className="mt-1 text-[11px] text-muted">{profile.data.model}</p>
            </>
          )}
        </Panel>
        <Panel title="Scenario grid">
          {grid.isError ? <Empty>{String(grid.error)}</Empty> : !grid.data ? <Empty>Loading…</Empty> : <>
            {grid.data.unpriced.length > 0 && <p className="mb-2 text-xs text-warn">Leaves out {grid.data.unpriced.join(", ")}: no valuation from this feed.</p>}
            <ScenarioGrid scenarios={{ ...grid.data, complete: grid.data.pnl.every((row) => row.every((v) => v != null)) }} />
          </>}
          <p className="mt-1 text-[11px] text-muted">{grid.data?.model}</p>
        </Panel>
      </div>
      <Panel title="Stress tests and value at risk">
        {stress.isError || tail.isError ? <Empty>{String(stress.error ?? tail.error)}</Empty> : !stress.data || !tail.data ? <Empty>Loading…</Empty> : <>
          {!stress.data.complete && <p className="mb-2 text-xs text-warn">Leaves out {stress.data.unpriced.join(", ")}: no valuation from this feed.</p>}
          <StressResultPanel account={false} data={{ stress: stress.data.stress, sticky_strike: tail.data.sticky_strike, atm_vol_change: tail.data.atm_vol_change }} />
          <p className="mt-1 text-[11px] text-muted">{stress.data.model} {tail.data.model}</p>
        </>}
      </Panel>
    </div>
  )
}

/** Analysis-only portfolios imported from other brokers: valued and stressed here, never traded. */
export function AnalysisView() {
  const queryClient = useQueryClient()
  const list = useQuery({ queryKey: ["analysis-portfolios"], queryFn: ({ signal }) => api.analysisPortfolios(signal) })
  const [selected, setSelected] = useState<string | null>(null)
  const portfolios = list.data?.portfolios ?? []
  const current = selected && portfolios.some((p) => p.id === selected) ? selected : portfolios[0]?.id ?? null
  const refresh = () => queryClient.invalidateQueries({ queryKey: ["analysis-portfolios"] })
  return (
    <div className="flex flex-col gap-3">
      <PageHeader title="Analysis" subtitle="Positions held elsewhere, imported to run OpenPort's risk views on them. Analysis only: they are not paper accounts and cannot be traded." />
      <div className="grid gap-3 xl:grid-cols-[18rem_1fr]">
        <div className="flex flex-col gap-3">
          <Panel title="Portfolios">
            {list.isError ? <Empty>{String(list.error)}</Empty> : portfolios.length ? (
              <ul className="space-y-1 text-sm">{portfolios.map((p) => (
                <li key={p.id}><button type="button" aria-current={p.id === current ? "true" : undefined} onClick={() => setSelected(p.id)}
                  className={`w-full rounded-md px-2 py-1 text-left ${p.id === current ? "bg-raised" : "hover:bg-raised/60"}`}>
                  {p.name}<span className="ml-2 text-[11px] text-muted">{p.holdings} lines · {p.format}</span>
                </button></li>))}</ul>
            ) : <p className="text-xs text-muted">{list.data ? "Nothing imported yet." : "Loading…"}</p>}
          </Panel>
          <ImportForm onImported={(id) => { setSelected(id); void refresh() }} />
        </div>
        {current ? <Portfolio key={current} id={current} onDeleted={() => { setSelected(null); void refresh() }} />
          : <Empty>Import a positions CSV to see its Greeks, risk profile and scenario grid.</Empty>}
      </div>
    </div>
  )
}
