import { useQuery } from "@tanstack/react-query"
import { useMemo, useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { ExposureConvention, ExposureSlice } from "../api/types"
import { BarChart } from "../charts/BarChart"
import { Heatmap } from "../charts/Heatmap"
import { CoverageBadge, Empty, Panel, Segmented, Stat } from "../components/ui"
import { oiCoverage } from "../lib/coverage"
import { expiryLabel, fixed, isNum, money, price } from "../lib/format"
import { matchingPayload } from "../lib/payload"

type Metric = "gex" | "vex" | "dex" | "cex" | "vomex"
const metrics: { value: Metric; label: string; name: string; unit: string }[] = [
  { value: "gex", label: "GEX", name: "Gamma", unit: "dollar delta per 1% spot move" },
  { value: "vex", label: "VEX", name: "Vanna", unit: "dollar delta per vol point" },
  { value: "dex", label: "DEX", name: "Delta", unit: "dollar delta" },
  { value: "cex", label: "CEX", name: "Charm", unit: "dollar delta per calendar day" },
  { value: "vomex", label: "VOMEX", name: "Vomma", unit: "dollars per vol point squared" },
]

// Older servers omit the convention or echo the default's deprecated spelling.
const conventionOf = (payload: { convention?: string | null }) =>
  !payload.convention || payload.convention === "open_interest" ? "long_calls_short_puts" : payload.convention

export function ExposureView({ symbol }: { symbol: string }) {
  const version = useLive().version(symbol)
  const [expiries, setExpiries] = useState(8)
  const [window, setWindow] = useState(0.05)
  const [metric, setMetric] = useState<Metric>("gex")
  const [convention, setConvention] = useState<ExposureConvention>("long_calls_short_puts")
  const [slice, setSlice] = useState<ExposureSlice>("all")

  const exposure = useQuery({
    queryKey: ["exposure", symbol, expiries, window, convention, slice, version],
    queryFn: ({ signal }) => api.exposure(symbol, expiries, window, signal, convention, slice),
    placeholderData: (previous) => previous && conventionOf(previous) === convention &&
      (previous.slice ?? "all") === slice ? matchingPayload(previous, symbol) : undefined,
  })
  const publication = matchingPayload(exposure.data, symbol)
  const modeMatches = !publication || conventionOf(publication) === convention &&
    (publication.slice ?? "all") === slice
  const data = modeMatches ? publication : undefined
  const totals = useMemo(() => {
    if (!data || data.exposure.available === false) return []
    return data.strikes.flatMap((strike, i) => {
      const values = data.expiries.map((e) => e[metric]?.[i]).filter(isNum)
      return values.length ? [{ x: strike, value: values.reduce((sum, value) => sum + value, 0) }] : []
    })
  }, [data, metric])
  const nearestStrike = useMemo(() => {
    if (!data || !isNum(data.spot)) return null
    const spot = data.spot
    return data.strikes.reduce<number | null>((best, s) => (best == null || Math.abs(s - spot) < Math.abs(best - spot) ? s : best), null)
  }, [data])

  const selected = metrics.find((entry) => entry.value === metric)!
  const summary = data?.exposure
  const levels = summary?.levels?.[metric] ?? (metric === "gex" && summary ? {
    flip: summary.gamma_flip, positive_wall: summary.call_wall, negative_wall: summary.put_wall,
  } : undefined)
  const markers = [
    ...(isNum(data?.spot) ? [{ x: data.spot, label: `spot ${price(data.spot)}`, color: "var(--warn)" }] : []),
    ...(isNum(levels?.flip) ? [{ x: levels.flip, label: "flip", color: "var(--muted)" }] : []),
  ]
  const scope = slice === "all" ? "All live expiries" : slice === "0dte" ? "Same-day expiries" : "Excluding same-day expiries"

  return (
    <div className="flex flex-col gap-3">
      <div className="flex flex-wrap items-center gap-3">
        <label className="flex items-center gap-2 text-xs text-muted">
          Positioning convention
          <select aria-label="Positioning convention" className="rounded border border-border bg-panel px-2 py-1 text-foreground"
            value={convention} onChange={(event) => setConvention(event.target.value as ExposureConvention)}>
            <option value="long_calls_short_puts">Dealers long calls, short puts (default)</option>
            <option value="short_all">Dealers short all options</option>
            <option value="customer_flow">Customer flow · unavailable</option>
          </select>
        </label>
        <Segmented label="Expiry slice" value={slice} options={[
          { value: "all", label: "All" }, { value: "0dte", label: "0DTE only" }, { value: "ex_0dte", label: "Ex-0DTE" },
        ]} onChange={setSlice} />
        <Segmented label="Metric" value={metric} options={metrics} onChange={setMetric} />
      </div>
      {exposure.isError ? <Empty>{String(exposure.error)}</Empty> : !modeMatches ? <Empty>This server did not return the selected exposure convention and slice.</Empty>
        : !data || !summary ? <Empty>Loading exposure…</Empty> : <>
        <div className="grid grid-cols-2 gap-2 sm:grid-cols-3 lg:grid-cols-5">
          {metrics.map((entry) => <Stat key={entry.value} label={`Net ${entry.label}`} value={money(summary[entry.value])}
            tone={isNum(summary[entry.value]) && summary[entry.value]! < 0 ? "negative" : "positive"}
            hint={`${entry.name}: ${entry.unit}; every strike in the selected expiry slice`} />)}
        </div>
        <div className="grid grid-cols-3 gap-2">
          <Stat label={`${selected.name} flip`} value={price(levels?.flip)} tone="warn" hint="Nearest spot level where the selected metric changes sign, sticky-strike IV; absent when no crossing is found" />
          <Stat label="Positive wall" value={fixed(levels?.positive_wall, 0)} tone="positive" hint={`Strike with largest positive ${selected.label}, full selected slice`} />
          <Stat label="Negative wall" value={fixed(levels?.negative_wall, 0)} tone="negative" hint={`Strike with most negative ${selected.label}, full selected slice`} />
        </div>
        <p className="text-[11px] text-muted">{scope} at snapshot New York date. Totals and levels cover the full selected slice; charts use the display limits below.</p>
        {summary.available === false ? <Empty>{summary.unavailable_reason ?? "Exposure unavailable"}</Empty> : <>
          <Panel title={`${selected.name} exposure by strike · ${selected.unit}`} actions={<>
            {summary.oi_coverage !== undefined && <CoverageBadge coverage={oiCoverage(summary.oi_coverage)} />}
            <Segmented label="Expiries" value={expiries} options={[4, 8, 16, 40].map((n) => ({ value: n, label: `${n} exp` }))} onChange={setExpiries} />
            <Segmented label="Window" value={window} options={[0.02, 0.05, 0.1].map((w) => ({ value: w, label: `±${w * 100}%` }))} onChange={setWindow} />
          </>}>
            {totals.length ? <BarChart bars={totals} markers={markers} height={280} formatX={(x) => fixed(x, 0)} formatY={(y) => money(y)} />
              : <Empty>No exposure cells for this metric and slice.</Empty>}
          </Panel>
          <Panel title="By strike and expiry">
            <Heatmap label={`${selected.name} exposure by strike and expiry, ${selected.unit}`}
              rows={data.strikes} columns={data.expiries.map((e) => ({ id: e.id, label: expiryLabel(e.id) }))}
              values={data.expiries.map((e) => e[metric] ?? data.strikes.map(() => null))}
              highlightRow={nearestStrike} format={(v) => money(v)} formatRow={(r) => fixed(r, 0)} />
          </Panel>
        </>}
      </>}
      <p className="text-[11px] leading-relaxed text-muted">
        Derived positioning model: {convention === "short_all" ? "dealers short calls and puts." : convention === "customer_flow"
          ? "customer flow requires aggressor-side or signed volume data; unsigned volume cannot identify direction."
          : "dealers long calls and short puts (the default OI convention)."} These are assumptions, not observed dealer positions.
        Exposure weights received OI by contract multiplier, with a default half-day time floor. Charm is delta change per calendar day
        at fixed spot and fitted carry. Vomma is price curvature per squared vol point, not a hedge-flow estimate.
      </p>
    </div>
  )
}
