import { useQuery } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { sameAccountPlaceholder } from "../api/trading"
import type { RiskProfile, RiskProfileQuery, SmileDynamics } from "../api/trading-types"
import { LineChart } from "../charts/LineChart"
import { fixed, money } from "../lib/format"
import { betaSources, curveLabel, levelText, parseBetas, profileChart } from "../lib/profile"
import { formatMoney } from "../lib/trading"
import { TradingError } from "./TradingControls"
import { DynamicsNote, DynamicsSelect } from "./ScenarioGrid"

const dateChoices: { value: number | "expiry"; label: string }[] = [
  { value: 0, label: "Today" }, { value: 1, label: "T+1" }, { value: 7, label: "T+7" }, { value: "expiry", label: "Expiry" },
]
const signed = (value: number | null | undefined) => value == null ? "—" : `${value > 0 ? "+" : ""}${fixed(value, 0)}`

export function ProfileResult({ profile }: { profile: RiskProfile }) {
  const chart = profileChart(profile)
  const middle = profile.percent.indexOf(0)
  const weighted = profile.benchmark != null
  return <div className="space-y-3">
    {!profile.complete && <p role="status" className="text-xs text-warn">Profile incomplete · a held contract has no fresh valuation{weighted ? " or an underlying has no beta" : ""}, so no curve is shown.</p>}
    <LineChart series={chart.series} references={chart.references} markers={chart.markers} height={300} marginLeft={70}
      xLabel={chart.byPrice ? `${profile.reference} price` : `${profile.reference} move (%)`}
      formatX={(x) => chart.byPrice ? fixed(x, x >= 1000 ? 0 : 2) : `${x > 0 ? "+" : ""}${fixed(x, 1)}%`} formatY={money} />
    <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Risk profile dates">
      <table className="w-full text-right text-xs tabular whitespace-nowrap">
        <thead className="text-muted"><tr>
          <th className="py-1 pr-2 text-left font-normal">Date</th><th className="pr-2 font-normal">P&amp;L unchanged</th>
          <th className="pr-2 font-normal">Floor below</th><th className="pr-2 font-normal">Floor above</th>
          <th className="pr-2 font-normal">Soft floor below</th><th className="pr-2 font-normal">Soft floor above</th>
          <th className="font-normal">±1σ by then</th>
        </tr></thead>
        <tbody>{profile.curves.map((curve, index) => <tr key={index} className="border-t border-border/40">
          <td className="py-1 pr-2 text-left" title={curve.time}>{curveLabel(curve)}{curve.clamped ? " *" : ""}</td>
          <td className={`pr-2 ${(curve.pnl[middle] ?? 0) < 0 ? "text-bearish" : ""}`}>{profile.complete ? signed(curve.pnl[middle]) : "—"}</td>
          <td className="pr-2">{levelText(curve.down)}</td><td className="pr-2">{levelText(curve.up)}</td>
          <td className="pr-2">{levelText(curve.soft_down)}</td><td className="pr-2">{levelText(curve.soft_up)}</td>
          <td>{curve.horizon.one_sd ? `${fixed(curve.horizon.one_sd.low, 2)} – ${fixed(curve.horizon.one_sd.high, 2)}` : "—"}</td>
        </tr>)}</tbody>
      </table>
    </div>
    {weighted && <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Beta weighting">
      <table className="w-full text-right text-xs tabular whitespace-nowrap">
        <thead className="text-muted"><tr>
          <th className="py-1 pr-2 text-left font-normal">Underlying</th><th className="pr-2 font-normal">Beta to {profile.benchmark}</th>
          <th className="pr-2 text-left font-normal">From</th><th className="pr-2 font-normal">Dollar delta</th><th className="font-normal">Weighted dollar delta</th>
        </tr></thead>
        <tbody>
          {profile.betas.map((beta) => <tr key={beta.underlying} className="border-t border-border/40">
            <td className="py-1 pr-2 text-left">{beta.underlying}</td>
            <td className="pr-2">{beta.beta == null ? "—" : fixed(beta.beta, 2)}</td>
            <td className="pr-2 text-left text-muted">{beta.source ? `${betaSources[beta.source]}${beta.source === "daily" || beta.source === "intraday" ? ` · ${beta.observations} returns` : ""}`
              : <span className="text-warn">No history; enter one</span>}</td>
            <td className="pr-2">{signed(beta.dollar_delta)}</td><td>{signed(beta.weighted_dollar_delta)}</td>
          </tr>)}
          <tr className="border-t border-border font-medium"><td className="py-1 pr-2 text-left">Book</td><td colSpan={3} className="pr-2 text-muted">
            {profile.weighted_delta != null ? `${fixed(profile.weighted_delta, 1)} ${profile.benchmark} deltas` : ""}</td>
            <td>{signed(profile.weighted_dollar_delta)}</td></tr>
        </tbody>
      </table>
    </div>}
    <p className="text-[11px] text-muted">Equity {formatMoney(profile.equity)}{profile.room != null ? ` · ${formatMoney(profile.room)} to the plan floor` : ""}
      {profile.soft_room != null ? ` · ${formatMoney(profile.soft_room)} to the soft floor` : ""}. Dashed lines mark the loss that reaches each floor.
      {" "}{profile.model}. Touch odds run to today's close for today and to each later date otherwise.{profile.curves.some((c) => c.clamped) ? " * volatility clamped." : ""}</p>
    {profile.dynamics && profile.dynamics !== "sticky_strike" && <DynamicsNote dynamics={profile.dynamics} fallback={profile.surface_fallback} />}
  </div>
}

/** The held book's P&L curve on several dates, against one underlying or beta-weighted to SPY or SPX. */
export function RiskProfilePanel({ underlyings }: { underlyings: string[] }) {
  const { accountScope, trading } = useLive()
  const [scope, setScope] = useState("")
  const [benchmark, setBenchmark] = useState<"SPY" | "SPX">("SPY")
  const [dates, setDates] = useState<(number | "expiry")[]>([0, 1, "expiry"])
  const [iv, setIv] = useState(0)
  const [termScaled, setTermScaled] = useState(false)
  const [range, setRange] = useState(10)
  const [betaText, setBetaText] = useState("")
  const [dynamics, setDynamics] = useState<SmileDynamics>("sticky_strike")
  const betas = parseBetas(betaText)
  const underlying = underlyings.includes(scope) ? scope : ""
  const query: RiskProfileQuery = {
    ...(underlying ? { underlying } : { benchmark, betas: betas ?? {} }),
    ...(termScaled ? { term_scaled: true } : {}),
    days: dateChoices.map((c) => c.value).filter((v) => dates.includes(v)), iv, range,
    ...(dynamics === "sticky_strike" ? {} : { dynamics }),
  }
  const profile = useQuery({
    queryKey: ["trading", accountScope, "risk-profile", trading?.account_version, query],
    queryFn: ({ signal }) => api.riskProfile(query, signal),
    enabled: !!trading?.enabled && underlyings.length > 0, retry: false,
    placeholderData: (previous, prior) => sameAccountPlaceholder(previous, prior?.queryKey, accountScope),
  })
  if (!underlyings.length) return <p className="text-sm text-muted">Nothing held. The profile curves the open positions and shares.</p>
  return <div className="space-y-3">
    <div className="flex flex-wrap items-end gap-3 text-xs">
      <label className="flex flex-col gap-1 text-muted">Against
        <select aria-label="Profile scope" className="trade-input" value={underlying} onChange={(e) => setScope(e.target.value)}>
          <option value="">Whole book, beta-weighted</option>
          {underlyings.map((symbol) => <option key={symbol} value={symbol}>{symbol} only</option>)}
        </select></label>
      {!underlying && <label className="flex flex-col gap-1 text-muted">Benchmark
        <select aria-label="Beta benchmark" className="trade-input" value={benchmark} onChange={(e) => setBenchmark(e.target.value as "SPY" | "SPX")}>
          <option>SPY</option><option>SPX</option>
        </select></label>}
      <fieldset className="flex flex-col gap-1 text-muted"><legend>Dates</legend>
        <div className="flex gap-1">{dateChoices.map((choice) => {
          const on = dates.includes(choice.value)
          return <button key={choice.label} type="button" aria-pressed={on} className={`trade-button ${on ? "ring-1 ring-accent" : ""}`}
            onClick={() => setDates((current) => on ? (current.length > 1 ? current.filter((v) => v !== choice.value) : current) : [...current, choice.value])}>{choice.label}</button>
        })}</div></fieldset>
      <label className="flex flex-col gap-1 text-muted">IV change (vol points)
        <input aria-label="IV change in vol points" type="number" className="trade-input w-24" step={1} min={-50} max={100} value={iv}
          onChange={(e) => { const v = Number(e.target.value); if (Number.isFinite(v) && Math.abs(v) <= 100) setIv(v) }} /></label>
      <label className="flex gap-2 text-muted"><input type="checkbox" checked={termScaled} onChange={(e) => setTermScaled(e.target.checked)} />Term-scaled IV shock</label>
      <label className="flex flex-col gap-1 text-muted">Range
        <select aria-label="Profile range" className="trade-input" value={range} onChange={(e) => setRange(Number(e.target.value))}>
          {[3, 5, 10, 20, 40].map((r) => <option key={r} value={r}>±{r}%</option>)}
        </select></label>
      <DynamicsSelect value={dynamics} onChange={setDynamics} />
      {!underlying && <label className="flex flex-col gap-1 text-muted">Betas (optional)
        <input aria-label="Beta overrides" className={`trade-input w-40 ${betas ? "" : "border-danger"}`} placeholder="QQQ:1.2, IWM:1.1" value={betaText}
          onChange={(e) => setBetaText(e.target.value)} /></label>}
    </div>
    <TradingError error={profile.error} />
    {profile.data ? <ProfileResult profile={profile.data} /> : !profile.error && <p className="text-sm text-muted">Loading profile…</p>}
  </div>
}
