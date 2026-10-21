import type { Volatility, VolatilitySeries } from "../api/types"
import { LineChart, type Series } from "../charts/LineChart"
import { Panel } from "./ui"
import { volPoints as vp } from "../lib/volatility"

const percent = (value: number | null | undefined) => value == null ? "—" : `${vp(100 * value, 1)}%`
const dateLabel = (time: number) => new Date(time * 1000).toLocaleDateString("en-US", { timeZone: "America/New_York", month: "short", day: "numeric", year: "2-digit" })

export function IvHistoryBadge({ data }: { data: Volatility }) {
  const basis = data.history_basis
  return <div className="flex flex-wrap items-center gap-2 text-xs">
    <span className="rounded-full border border-border bg-panel px-2 py-1 tabular">IV rank {percent(data.iv_rank)}</span>
    <span className="text-muted">z-score {vp(data.iv_z_score)}</span>
    <span className="text-muted">Percentile {percent(data.iv_percentile)} · {data.history_sessions ?? 0} of 252 sessions · {basis?.current === "own_atm" ? "ATM 30d fallback" : basis?.current === "own_mfiv" ? "Model-free IV 30d" : "Current IV unavailable"}</span>
    <span className="text-muted">{basis?.own_sessions ?? 0} local{data.proxy?.used ? ` + ${basis?.proxy_sessions ?? 0} ${data.proxy.name} proxy sessions · proxy starts ${data.proxy.start}` : " sessions"}</span>
  </div>
}

export function historyLines(history: VolatilitySeries): Series[] {
  return ([
    ["mfiv30", "Local model-free IV · 30d", "var(--chart-1)"],
    ["atm30", "Local ATM IV · 30d", "var(--chart-2)"],
    ["rv21", "Close RV · trailing 21 sessions", "var(--chart-3)"],
    ["proxy_iv30", "Cboe index proxy · 30d", "var(--warn)"],
  ] as const).map(([field, label, color]) => ({ id: field, label, color, dashed: field === "proxy_iv30",
    points: history.rows.map(row => ({ x: row.t, y: row[field] ?? null })) }))
}

export function VolatilityHistory({ data, history, error }: { data: Volatility; history?: VolatilitySeries; error?: string }) {
  const vrp = data.ex_post_vrp
  const firstProxy = history?.rows.find(row => row.proxy_iv30 != null)
  return <div className="grid gap-3 xl:col-span-2 xl:grid-cols-2">
    <Panel title="IV and realized history · daily">
      {error ? <p className="text-xs text-warn">{error}</p> : !history ? <p className="text-xs text-muted">Loading history…</p> : <>
        <LineChart series={historyLines(history)} height={230} formatX={dateLabel} formatY={value => vp(value, 1)} />
        <p className="mt-2 text-[11px] text-muted">Vol points. Last available regular-session minute; gaps stay missing. {firstProxy ? `${firstProxy.sources.proxy_iv30 ?? "Index"} proxy from ${dateLabel(firstProxy.t)}, shown separately before local history.` : "Local history only where available."} RV uses completed daily closes.</p>
      </>}
    </Panel>
    <Panel title="25Δ skew · 30d history">
      <LineChart series={(["rr25", "bf25"] as const).map((field, i) => ({ id: field, label: `Local ${field.toUpperCase()} · vol points`, color: `var(--chart-${i + 2})`, points: (history?.rows ?? []).map(row => ({ x: row.t, y: row[field] ?? null })) }))} height={230} formatX={dateLabel} formatY={value => vp(value, 1)} />
      <p className="mt-2 text-[11px] text-muted">RR is call IV minus put IV; BF is their average minus ATM IV. {data.skew.delta_convention}.</p>
    </Panel>
    <Panel title="Skew and term history ranks">
      <div className="overflow-x-auto"><table aria-label="Skew and term ranks" className="w-full text-right text-xs tabular">
        <thead className="text-muted"><tr>{["Metric", "Current", "Rank", "Percentile", "z-score", "Sessions"].map(label => <th key={label} className="px-2 py-1 font-normal">{label}</th>)}</tr></thead>
        <tbody>{[["rr25", "RR25 (vp)"], ["bf25", "BF25 (vp)"], ["ratio9_30", "MFIV 9d/30d"], ["ratio30_93", "MFIV 30d/93d"]].map(([field, label]) => {
          const rank = data.history_ranks?.[field!]
          return <tr key={field} className="border-t border-border/40"><td className="px-2 py-1">{label}</td><td>{vp(rank?.current, 3)}</td><td>{percent(rank?.rank)}</td><td>{percent(rank?.percentile)}</td><td>{vp(rank?.z_score)}</td><td>{rank?.sessions ?? 0} / 252</td></tr>
        })}</tbody>
      </table></div>
      <p className="mt-2 text-[11px] text-muted">Completed local sessions only; no index proxies for skew or term. Rank is unclamped. Percentile counts strictly lower values; z-score uses population deviation. A flat window has no rank or z-score.</p>
    </Panel>
    <Panel title="Term ratio history">
      <LineChart series={(["ratio9_30", "ratio30_93"] as const).map((field, i) => ({ id: field, label: i ? "MFIV 30d / 93d" : "MFIV 9d / 30d", color: `var(--chart-${i + 1})`, points: (history?.rows ?? []).map(row => ({ x: row.t, y: row[field] ?? null })) }))} height={230} formatX={dateLabel} formatY={value => vp(value, 3)} references={[{ y: 1, label: "flat", color: "var(--muted)" }]} />
    </Panel>
    {history?.rows.some(row => row.vix != null) && <Panel title={`VIX − ${data.symbol} model-free IV · 30d`} className="xl:col-span-2">
      <LineChart series={[{ id: "vix-minus-mfiv", label: "VIX daily close − local MFIV30 (vol points)", color: "var(--chart-3)", points: history.rows.map(row => ({ x: row.t, y: row.vix_minus_mfiv ?? null })) }]} height={200} formatX={dateLabel} formatY={value => vp(value, 2)} references={[{ y: 0, label: "zero", color: "var(--muted)" }]} />
      <p className="mt-2 text-[11px] text-muted">Same completed session, using the last available local sample and the VIX daily close; quotes need not be simultaneous. Gaps stay missing. {data.symbol === "SPY" && "VIX references SPX options; this is a cross-product comparison. "}Live measured differences are pending validation; no comparison is fabricated when observations are absent.</p>
    </Panel>}
    {vrp && <Panel title={`Ex-post variance risk premium${vrp.proxy ? " · SPX proxy for SPY" : " · SPX"}`} className="xl:col-span-2">
      <LineChart series={[
        { id: "own", label: "Local MFIV² − following RV²", color: "var(--chart-1)", points: vrp.points.map(point => ({ x: Date.parse(`${point.date}T16:00:00Z`) / 1000, y: point.source === "own_mfiv" ? point.variance : null })) },
        { id: "proxy", label: "VIX² − following RV² · proxy", color: "var(--warn)", dashed: true, points: vrp.points.map(point => ({ x: Date.parse(`${point.date}T16:00:00Z`) / 1000, y: point.source === "VIX" ? point.variance : null })) },
      ]} references={[{ y: 0, label: "zero", color: "var(--muted)" }]} height={230} formatX={dateLabel} formatY={value => vp(value, 4)} />
      <div className="overflow-x-auto"><table aria-label="Ex-post VRP summaries" className="w-full whitespace-nowrap text-right text-xs tabular">
        <thead className="text-muted"><tr>{["Window / observations", "Mean variance", "Median variance", "Mean IV − RV (vp)", "Median IV − RV (vp)", "Positive months"].map(label => <th key={label} className="px-2 py-1 font-normal">{label}</th>)}</tr></thead>
        <tbody>{vrp.summaries.map(row => <tr key={row.years} className="border-t border-border/40">
          <td className="px-2 py-1" title={`${row.from || "—"} to ${row.to || "—"}`}>{row.years}y · {row.observations} ({row.proxy_observations} proxy)</td>
          <td>{vp(row.mean_variance, 5)}</td><td>{vp(row.median_variance, 5)}</td><td>{vp(row.mean_vol_points)}</td><td>{vp(row.median_vol_points)}</td><td>{percent(row.positive_share)}</td>
        </tr>)}</tbody>
      </table></div>
      <p className="mt-2 text-[11px] text-muted">{vrp.implied_source}. {vrp.realized_source}. {vrp.convention}. Windows may have partial history; dates are available on each row. Unfinished months are excluded.</p>
    </Panel>}
  </div>
}
