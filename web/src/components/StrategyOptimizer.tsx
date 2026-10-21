import { useQuery } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import type { Expiry, SearchRank, SearchStructure, StrategyCandidate, StrategySearchQuery } from "../api/types"
import { deliversShares } from "../lib/trading"
import { expiryLabel, fixed, isNum, pct } from "../lib/format"
import { TradingError } from "./TradingControls"

const structures: { value: SearchStructure; label: string }[] = [
  { value: "vertical", label: "Verticals" }, { value: "condor", label: "Iron condors" }, { value: "butterfly", label: "Butterflies" },
  { value: "calendar", label: "Calendars" }, { value: "single", label: "Singles" }, { value: "covered", label: "Covered calls" },
]
const ranks: { value: SearchRank; label: string }[] = [
  { value: "expected", label: "Expected P&L, your view" }, { value: "risk_neutral", label: "Expected P&L, market's view" },
  { value: "pop", label: "Probability of profit" }, { value: "return_on_risk", label: "Return on risk" },
]
const dollars = (x: number | null | undefined) => isNum(x) ? `${x < 0 ? "−" : ""}$${Math.abs(x).toLocaleString("en-US", { maximumFractionDigits: 0 })}` : "—"

/** The query a filled-in form asks for; null with a reason while it is incomplete. */
export function searchQuery(form: { target: string; budget: string; date: string; expiries: string[]; structures: SearchStructure[];
  rank: SearchRank; vol: string; price: "natural" | "mid" }): StrategySearchQuery | { reason: string } {
  const target = Number(form.target), budget = Number(form.budget), vol = Number(form.vol || 0)
  if (!(target > 0)) return { reason: "Enter the price you expect." }
  if (!(budget >= 0.01)) return { reason: "Enter the most one candidate may lose, in dollars." }
  if (!Number.isFinite(vol) || vol < -50 || vol > 100) return { reason: "The volatility change must be −50 to +100 vol points." }
  if (!form.structures.length) return { reason: "Pick at least one structure." }
  if (form.expiries.length > 4) return { reason: "Pick at most four expiries." }
  return { target, budget, ...(form.date ? { date: form.date } : {}), ...(form.expiries.length ? { expiries: form.expiries } : {}),
    structures: form.structures, rank: form.rank, ...(vol ? { vol_points: vol } : {}), price: form.price, limit: 25 }
}

/**
 * The strategy optimizer: candidates on the chosen expiries ranked for a target
 * price and date within a loss budget, searched on the server. Each result can be
 * drawn on the Analyze chart or sent to its ticket.
 */
export function StrategyOptimizer({ symbol, spot, expiries, onAnalyze, onTicket }: {
  symbol: string; spot: number | null; expiries: Expiry[]
  onAnalyze: (candidate: StrategyCandidate) => void; onTicket: (candidate: StrategyCandidate) => void
}) {
  const [target, setTarget] = useState(spot != null ? spot.toFixed(spot >= 1000 ? 0 : 2) : "")
  const [date, setDate] = useState("")
  const [budget, setBudget] = useState("1000")
  const [chosen, setChosen] = useState<string[]>([])
  const [picked, setPicked] = useState<SearchStructure[]>(structures.map((s) => s.value).filter((s) => s !== "covered" || deliversShares(symbol)))
  const [rank, setRank] = useState<SearchRank>("expected")
  const [vol, setVol] = useState("0")
  const [price, setPrice] = useState<"natural" | "mid">("natural")
  const [submitted, setSubmitted] = useState<StrategySearchQuery | null>(null)
  const query = searchQuery({ target, budget, date, expiries: chosen, structures: picked, rank, vol, price })
  const search = useQuery({
    queryKey: ["optimizer", symbol, submitted],
    queryFn: ({ signal }) => api.optimizer(symbol, submitted!, signal),
    enabled: submitted != null, retry: false, staleTime: Infinity,
  })
  const toggle = <T,>(list: T[], value: T) => list.includes(value) ? list.filter((v) => v !== value) : [...list, value]
  const result = search.data
  return <section aria-label="Strategy optimizer" className="space-y-3 text-sm">
    <form className="flex flex-wrap items-end gap-3 text-xs" onSubmit={(event) => { event.preventDefault(); if (!("reason" in query)) setSubmitted(query) }}>
      <label className="flex flex-col gap-1 text-muted">Target price
        <input aria-label="Target price" className="trade-input w-28" inputMode="decimal" value={target} onChange={(e) => setTarget(e.target.value)} /></label>
      <label className="flex flex-col gap-1 text-muted">By
        <input aria-label="Target date" type="date" className="trade-input" value={date} onChange={(e) => setDate(e.target.value)} /></label>
      <label className="flex flex-col gap-1 text-muted">Risk budget ($)
        <input aria-label="Risk budget in dollars" className="trade-input w-28" inputMode="decimal" value={budget} onChange={(e) => setBudget(e.target.value)} /></label>
      <label className="flex flex-col gap-1 text-muted">Your IV change (vp)
        <input aria-label="Your volatility change in vol points" type="number" className="trade-input w-24" step={1} value={vol} onChange={(e) => setVol(e.target.value)} /></label>
      <label className="flex flex-col gap-1 text-muted">Rank by
        <select aria-label="Rank by" className="trade-input" value={rank} onChange={(e) => setRank(e.target.value as SearchRank)}>
          {ranks.map((r) => <option key={r.value} value={r.value}>{r.label}</option>)}</select></label>
      <label className="flex flex-col gap-1 text-muted">Enter at
        <select aria-label="Entry price" className="trade-input" value={price} onChange={(e) => setPrice(e.target.value as "natural" | "mid")}>
          <option value="natural">Natural (far sides)</option><option value="mid">Mid</option></select></label>
      <fieldset className="flex flex-col gap-1 text-muted"><legend>Structures</legend>
        <div className="flex flex-wrap gap-1">{structures.map((s) => {
          const on = picked.includes(s.value), unavailable = s.value === "covered" && !deliversShares(symbol)
          return <button key={s.value} type="button" aria-pressed={on} disabled={unavailable} title={unavailable ? `${symbol} has no shares` : undefined}
            className={`trade-button ${on ? "ring-1 ring-accent" : ""}`} onClick={() => setPicked((list) => toggle(list, s.value))}>{s.label}</button>
        })}</div></fieldset>
      {expiries.length > 0 && <fieldset className="flex flex-col gap-1 text-muted"><legend>Expiries (up to 4; none picks the first on or after the date)</legend>
        <div className="flex max-w-xl flex-wrap gap-1">{expiries.slice(0, 16).map((e) => {
          const on = chosen.includes(e.id)
          return <button key={e.id} type="button" aria-pressed={on} disabled={!on && chosen.length >= 4}
            className={`trade-button ${on ? "ring-1 ring-accent" : ""}`} onClick={() => setChosen((list) => toggle(list, e.id))}>{expiryLabel(e.id, true)}</button>
        })}</div></fieldset>}
      <button type="submit" className="trade-button border-accent" disabled={"reason" in query || search.isFetching}>{search.isFetching ? "Searching…" : "Search"}</button>
    </form>
    {"reason" in query && <p className="text-xs text-muted">{query.reason}</p>}
    <TradingError error={search.error} />
    {result && <div className="space-y-2">
      <p className="text-xs text-muted">{result.candidates.length} of {result.within_budget} within {dollars(result.budget)} ·
        {" "}{result.evaluated.toLocaleString("en-US")} valued{result.truncated ? ` (an even sample of ${result.considered.toLocaleString("en-US")})` : ""}
        {result.unbounded ? ` · ${result.unbounded} with unlimited loss left out` : ""} · valued by {result.target_date} for {fixed(result.target_price, 2)}
        {result.skipped.length ? ` · skipped ${result.skipped.map((s) => `${s.expiry} (${s.reason})`).join(", ")}` : ""}</p>
      {result.candidates.length ? <div className="relative max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Optimizer results">
        <table className="w-full text-right text-xs tabular whitespace-nowrap">
          <thead className="text-muted"><tr>
            <th className="py-1 pr-2 text-left font-normal">Strategy</th><th className="pr-2 font-normal">Net / unit</th>
            <th className="pr-2 font-normal">Max loss</th><th className="pr-2 font-normal">Max profit</th>
            <th className="pr-2 font-normal">Breakevens</th>
            <th className="pr-2 font-normal" title="Probability of profit: market's risk-neutral distribution / yours">PoP mkt / you</th>
            <th className="pr-2 font-normal" title="Expected P&L per unit: market's distribution / yours">E[P&amp;L] mkt / you</th>
            <th className="pr-2 font-normal">Return on risk</th><th className="pr-2 font-normal">Units</th><th className="font-normal"><span className="sr-only">Actions</span></th>
          </tr></thead>
          <tbody>{result.candidates.map((c) => <tr key={c.name} className="border-t border-border/40">
            <td className="py-1 pr-2 text-left">{c.name}{c.shares ? ` · +${c.shares} sh` : ""}</td>
            <td className="pr-2">{fixed(c.net, 2)}</td><td className="pr-2">{dollars(c.max_loss)}</td>
            <td className="pr-2">{c.max_profit == null ? "Unlimited" : dollars(c.max_profit)}</td>
            <td className="pr-2">{c.breakevens.map((b) => fixed(b, 2)).join(", ") || "—"}</td>
            <td className="pr-2">{pct(c.pop.risk_neutral, 0)} / {pct(c.pop.user, 0)}</td>
            <td className={`pr-2 ${c.expected.user < 0 ? "text-bearish" : ""}`}>{dollars(c.expected.risk_neutral)} / {dollars(c.expected.user)}</td>
            <td className="pr-2">{pct(c.return_on_risk, 1)}</td><td className="pr-2">{c.quantity}</td>
            <td className="space-x-1"><button type="button" className="trade-button" onClick={() => onAnalyze(c)} aria-label={`Analyze ${c.name}`}>Analyze</button>
              <button type="button" className="trade-button" onClick={() => onTicket(c)} aria-label={`Ticket for ${c.name}`}>Ticket</button></td>
          </tr>)}</tbody>
        </table>
      </div> : <p className="text-xs text-muted">No candidate fits the budget. Raise it, or widen the structures and expiries.</p>}
      <p className="text-[11px] text-muted">{result.label}. {result.model}</p>
    </div>}
  </section>
}
