import { useQuery } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { parseBetas } from "../lib/profile"
import type { SmileDynamics } from "../api/trading-types"
import { DynamicsNote, DynamicsSelect, ScenarioGrid } from "./ScenarioGrid"
import { TradingError } from "./TradingControls"

export function ScenarioExplorer() {
  const { accountScope, trading } = useLive()
  const [mode, setMode] = useState("parallel")
  const [benchmark, setBenchmark] = useState("SPY")
  const [given, setGiven] = useState("")
  const [days, setDays] = useState(0)
  const [term, setTerm] = useState(false)
  const [dynamics, setDynamics] = useState<SmileDynamics>("sticky_strike")
  const betas = mode === "beta" ? parseBetas(given) : {}
  const query = `mode=${mode}&benchmark=${benchmark}&days=${days}&term_scaled=${term}${dynamics === "sticky_strike" ? "" : `&dynamics=${dynamics}`}&betas=${encodeURIComponent(Object.entries(betas ?? {}).map(([s, b]) => `${s}:${b}`).join(","))}`.replace(/&betas=$/, "")
  const result = useQuery({ queryKey: ["trading", accountScope, "scenario-analysis", trading?.account_version, query],
    queryFn: ({ signal }) => api.scenarios(query, signal), enabled: betas != null, retry: false })
  return <section className="space-y-3 rounded-md border border-border p-3">
    <h3 className="text-xs font-medium">Scenario explorer</h3>
    <div className="flex flex-wrap items-end gap-3">
      <label className="trade-label">Spot moves<select aria-label="Scenario mode" className="trade-input" value={mode} onChange={(e) => setMode(e.target.value)}>
        <option value="parallel">Same percent</option><option value="beta">Beta to benchmark</option></select></label>
      {mode === "beta" && <><label className="trade-label">Benchmark<select aria-label="Scenario benchmark" className="trade-input" value={benchmark} onChange={(e) => setBenchmark(e.target.value)}><option>SPY</option><option>SPX</option></select></label>
        <label className="trade-label">Beta overrides<input aria-label="Scenario betas" className="trade-input" value={given} placeholder="QQQ:1.2" onChange={(e) => setGiven(e.target.value)} /></label></>}
      <label className="trade-label">Days forward<input aria-label="Scenario days" className="trade-input" type="number" min={0} max={366} value={days}
        onChange={(e) => setDays(Math.max(0, Math.min(366, Number(e.target.value) || 0)))} /></label>
      <label className="flex gap-2 text-xs"><input type="checkbox" checked={term} onChange={(e) => setTerm(e.target.checked)} />Term-scaled vol shock</label>
      <div className="text-xs"><DynamicsSelect label="Scenario smile dynamics" value={dynamics} onChange={setDynamics} /></div>
    </div>
    <TradingError error={result.error} />
    {betas == null && <p className="text-xs text-warn">Use SYMBOL:BETA pairs separated by commas, with betas from -10 to 10.</p>}
    {betas != null && result.data?.slices[0] && <ScenarioGrid scenarios={result.data.slices[0]} />}
    {betas != null && result.data && <DynamicsNote dynamics={result.data.dynamics} fallback={result.data.surface_fallback} />}
    {mode === "beta" && <p className="text-xs text-muted">Beta to {benchmark}: {result.data?.betas.map((b) => `${b.underlying} ${b.beta == null ? "unavailable" : b.beta.toFixed(2)} (${b.source ?? "no history"})`).join(" · ")}</p>}
    <p className="text-xs text-muted">Derived P&amp;L at T+{days}. Term scaling multiplies the vol shock by √(30 / remaining days), with a one-day minimum. Order checks and max-loss rules keep the configured same-percent, zero-day grid.</p>
  </section>
}
