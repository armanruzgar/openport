import { useQuery } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { sameAccountPlaceholder } from "../api/trading"
import type { Scenarios, SmileDynamics } from "../api/trading-types"
import { formatMoney, scenarioColour, scenarioScale } from "../lib/trading"
import { TradingError } from "./TradingControls"

export const dynamicsChoices: { value: SmileDynamics; label: string }[] = [
  { value: "sticky_strike", label: "Sticky strike" },
  { value: "sticky_moneyness", label: "Sticky moneyness" },
  { value: "sticky_delta", label: "Sticky delta" },
]

/** Which smile dynamics a scenario read used, and any underlying that had no surface to move along. */
export function DynamicsNote({ dynamics, fallback }: { dynamics?: SmileDynamics; fallback?: string[] }) {
  if (!dynamics || dynamics === "sticky_strike") return <p className="text-[11px] text-muted">Sticky strike: each strike keeps its IV, as the account's rules value it.</p>
  return <p className="text-[11px] text-muted">
    {dynamics === "sticky_delta" ? "Sticky delta" : "Sticky moneyness"}: each smile moves with the forward along its fitted SSVI surface, from the contract's own IV
    {dynamics === "sticky_delta" ? "; vol changes apply in delta space" : ""}. Analysis only: rules and margin stay sticky strike.
    {fallback?.length ? <span className="text-warn"> No surface for {fallback.join(", ")}: sticky strike there.</span> : null}
  </p>
}

export function DynamicsSelect({ value, onChange }: { value: SmileDynamics; onChange: (value: SmileDynamics) => void }) {
  return <label className="flex flex-col gap-1 text-muted">Smile
    <select aria-label="Smile dynamics" className="trade-input" value={value} onChange={(e) => onChange(e.target.value as SmileDynamics)}>
      {dynamicsChoices.map((choice) => <option key={choice.value} value={choice.value}>{choice.label}</option>)}
    </select></label>
}

/** The account's grid, or the same grid recomputed under another smile dynamics. */
export function ScenarioPanel({ scenarios }: { scenarios: Scenarios }) {
  const { accountScope, trading } = useLive()
  const [dynamics, setDynamics] = useState<SmileDynamics>("sticky_strike")
  const moved = useQuery({
    queryKey: ["trading", accountScope, "risk-dynamics", trading?.account_version, dynamics],
    queryFn: ({ signal }) => api.riskScenarios(dynamics, signal),
    enabled: dynamics !== "sticky_strike", retry: false,
    placeholderData: (previous, prior) => sameAccountPlaceholder(previous, prior?.queryKey, accountScope),
  })
  const shown = dynamics === "sticky_strike" ? scenarios : moved.data?.scenarios
  return <div className="space-y-2">
    <div className="text-xs"><DynamicsSelect value={dynamics} onChange={setDynamics} /></div>
    <TradingError error={dynamics === "sticky_strike" ? null : moved.error} />
    {shown ? <>
      <ScenarioGrid scenarios={shown} />
      <DynamicsNote dynamics={shown.dynamics} fallback={shown.surface_fallback} />
    </> : !moved.error && <p className="text-sm text-muted">Moving smiles along their surfaces…</p>}
  </div>
}

export function ScenarioGrid({ scenarios }: { scenarios: Scenarios }) {
  const scale = scenarioScale(scenarios.pnl)
  return <div className="space-y-2">
    <p className="text-xs text-muted">Spot change (%) × volatility change (points) · P&amp;L in dollars</p>
    {!scenarios.complete && <p className="text-xs text-warn">Scenario valuation incomplete; unavailable cells are shown as —.</p>}
    <div className="max-w-full overflow-x-auto rounded-md border border-border" tabIndex={0} role="region" aria-label="Spot and volatility scenario grid">
      <table className="w-full border-collapse text-xs tabular">
        <thead><tr className="bg-raised text-muted"><th scope="col" className="p-2 text-left font-normal whitespace-nowrap">Spot / Vol</th>{scenarios.vol_points.map((vol, index) => <th scope="col" className="p-2 font-normal whitespace-nowrap" key={index}>{vol > 0 ? "+" : ""}{vol} vp</th>)}</tr></thead>
        <tbody>{scenarios.spot_percent.map((spot, row) => <tr key={row}>
          <th scope="row" className="bg-raised p-2 text-left font-normal whitespace-nowrap">{spot > 0 ? "+" : ""}{spot}%</th>
          {scenarios.vol_points.map((vol, column) => {
            const value = scenarios.pnl[row]?.[column] ?? null
            const clamped = scenarios.clamped[row]?.[column] ?? false
            const baseline = spot === 0 && vol === 0
            const display = value != null && Number.isFinite(value) ? formatMoney(String(value)) : "—"
            return <td key={column} style={{ background: scenarioColour(value, scale) }}
              title={`${spot}% spot, ${vol} vol points: ${value ?? "unavailable"}${clamped ? " · volatility clamped" : ""}${baseline ? " · unchanged spot and vol" : ""}`}
              className={`border border-border/50 p-2 text-right whitespace-nowrap ${baseline ? "font-semibold outline outline-2 -outline-offset-2 outline-accent" : ""}`}>{display}{clamped ? "*" : ""}</td>
          })}
        </tr>)}</tbody>
      </table>
    </div>
    <div className="flex flex-wrap justify-between gap-2 text-[11px] text-muted"><span><span className="text-bearish">Loss</span> ← 0 → <span className="text-bullish">Gain</span> · symmetric colour scale ±{formatMoney(String(scale))}</span><span>Outlined: unchanged spot and vol · * vol clamped</span></div>
  </div>
}
