import { useQuery } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { Probability } from "../api/types"
import { errorText } from "../lib/errors"
import { fixed } from "../lib/format"
import { parseNumbers } from "../lib/probability"
import { matchingPayload } from "../lib/payload"
import { Panel } from "./ui"

const percent = (value: number | null) => value == null ? "—" : `${(value * 100).toFixed(value < 0.001 && value > 0 ? 2 : 1)}%`
const horizonLabel = (days: number) => days === 0 ? "Today's close" : `${Number.isInteger(days) ? days : days.toFixed(1)} day${days === 1 ? "" : "s"}`

export function ProbabilityTable({ data }: { data: Probability }) {
  const prices = data.horizons[0]?.prices.map((p) => p.price) ?? []
  return <div className="space-y-3">
    <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Probability cones">
      <table className="w-full text-right text-xs tabular whitespace-nowrap">
        <thead className="text-muted"><tr><th className="py-1 pr-2 text-left font-normal">By</th><th className="pr-2 font-normal">σ</th>
          <th className="pr-2 font-normal">−2σ</th><th className="pr-2 font-normal">−1σ</th><th className="pr-2 font-normal">+1σ</th><th className="font-normal">+2σ</th></tr></thead>
        <tbody>{data.horizons.map((h) => <tr key={h.days} className="border-t border-border/40">
          <td className="py-1 pr-2 text-left" title={h.until}>{horizonLabel(h.days)}</td>
          <td className="pr-2">{h.sigma == null ? "—" : `${(h.sigma * 100).toFixed(2)}%`}</td>
          <td className="pr-2">{fixed(h.two_sd?.low, 2)}</td><td className="pr-2">{fixed(h.one_sd?.low, 2)}</td>
          <td className="pr-2">{fixed(h.one_sd?.high, 2)}</td><td>{fixed(h.two_sd?.high, 2)}</td>
        </tr>)}</tbody>
      </table>
    </div>
    {prices.length > 0 && <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Price probabilities">
      <table className="w-full text-right text-xs tabular whitespace-nowrap">
        <thead className="text-muted"><tr><th className="py-1 pr-2 text-left font-normal">By</th>
          {prices.map((price) => <th key={price} className="pr-2 font-normal" colSpan={2}>{fixed(price, 2)}{data.spot != null ? ` (${price >= data.spot ? "above" : "below"})` : ""}</th>)}</tr></thead>
        <tbody>{data.horizons.map((h) => <tr key={h.days} className="border-t border-border/40">
          <td className="py-1 pr-2 text-left">{horizonLabel(h.days)}</td>
          {h.prices.map((odds) => {
            const beyond = data.spot != null && odds.price >= data.spot ? odds.above : odds.below
            return [<td key={`${odds.price}-end`} className="pr-1" title="Finishes beyond the price">{percent(beyond)} end</td>,
              <td key={`${odds.price}-touch`} className="pr-2 text-muted" title="Touches the price before then">{percent(odds.touch)} touch</td>]
          })}
        </tr>)}</tbody>
      </table>
    </div>}
    <p className="text-[11px] text-muted">{data.model}. Spot {fixed(data.spot, 2)} at {data.as_of}. Implied, risk-neutral odds, not a forecast.</p>
  </div>
}

/** Where implied volatility says the underlying can be by each date, and the odds of reaching chosen prices. */
export function ProbabilityPanel({ symbol }: { symbol: string }) {
  const version = useLive().version(symbol)
  const [daysText, setDaysText] = useState("0, 1, 7, 30")
  const [pricesText, setPricesText] = useState("")
  const days = parseNumbers(daysText, 8, 0, 366)
  const prices = parseNumbers(pricesText, 20, Number.MIN_VALUE, 1e9)
  const valid = days != null && days.length > 0 && prices != null
  const odds = useQuery({
    queryKey: ["probability", symbol, version, days, prices],
    queryFn: ({ signal }) => api.probability(symbol, days ?? [], prices ?? [], signal),
    enabled: valid, retry: false,
    placeholderData: (previous) => matchingPayload(previous, symbol),
  })
  return <Panel title="Probability cones" actions={<>
    <label className="text-xs text-muted">Days <input aria-label="Probability horizons in days" className={`trade-input w-32 ${days?.length ? "" : "border-danger"}`}
      value={daysText} onChange={(e) => setDaysText(e.target.value)} /></label>
    <label className="text-xs text-muted">Prices <input aria-label="Prices to reach" className={`trade-input w-40 ${prices ? "" : "border-danger"}`}
      placeholder="e.g. 5900, 6100" value={pricesText} onChange={(e) => setPricesText(e.target.value)} /></label>
  </>}>
    {odds.error ? <p className="text-sm text-warn">{errorText(odds.error)}</p>
      : odds.data ? <ProbabilityTable data={odds.data} /> : <p className="text-sm text-muted">{valid ? "Loading…" : "Enter up to 8 horizons (0–366 days) and up to 20 prices."}</p>}
  </Panel>
}
