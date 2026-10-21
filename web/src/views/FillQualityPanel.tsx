import { useState } from "react"
import { api } from "../api/client"
import { useFillQuality } from "../api/trading"
import type { Calibration, CalibrationFormat, FillCalibration, FillQualityGroup, PaperOutcome } from "../api/trading-types"
import { TradingError } from "../components/TradingControls"
import { Empty, Panel, Segmented } from "../components/ui"
import { formatMoney, signedMoney } from "../lib/trading"

const places = [["mid_or_better", "Mid or better"], ["mid_plus_one_tick", "Mid +1 tick"], ["inside", "Inside"],
  ["natural", "Natural"], ["beyond_natural", "Beyond"]] as const
const ticks = (value: number | null) => value == null ? "—" : `${value > 0 ? "+" : ""}${value.toFixed(2)}`
const share = (count: number, total: number) => total ? `${Math.round((count / total) * 100)}%` : "—"
const when = new Intl.DateTimeFormat("en-US", { timeZone: "America/New_York", month: "short", day: "numeric", hour: "numeric", minute: "2-digit", second: "2-digit" })
/** "SPXW  261022C05000000" as "SPXW Oct 22 5000C"; anything else as given. */
export function osiLabel(symbol: string): string {
  const match = /^([A-Z0-9]{1,6})\s*(\d{2})(\d{2})(\d{2})([CP])(\d{8})$/.exec(symbol)
  if (!match) return symbol
  const [, root, year, month, day, right, strike] = match
  const date = new Date(Date.UTC(2000 + Number(year), Number(month) - 1, Number(day), 12))
  return `${root} ${date.toLocaleDateString("en-US", { month: "short", day: "numeric", timeZone: "UTC" })} ${Number(strike) / 1000}${right}`
}
/** Most of a broker export fits the 64 KiB request; leave room for the JSON around it. */
const MAX_CSV = 60_000

/** One row per group: its contracts by place, as a share of its contracts, and its means. */
export function FillGroups({ groups, label }: { groups: FillQualityGroup[]; label: string }) {
  return <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label={label}>
    <table className="w-full whitespace-nowrap text-right text-xs tabular" aria-label={label}>
      <thead><tr className="text-muted">
        <th className="p-2 text-left">Group</th><th className="p-2">Fills</th><th className="p-2">Contracts</th>
        {places.map(([key, title]) => <th key={key} className="p-2">{title}</th>)}
        <th className="p-2" title="Contract-weighted mean ticks beyond the mid; positive is worse for the side">vs mid</th>
        <th className="p-2" title="Contract-weighted mean ticks beyond the natural; negative is better than the natural">vs natural</th>
        <th className="p-2" title="Dollars paid beyond the mid, multiplier applied">Cost vs mid</th>
      </tr></thead>
      <tbody>{groups.map((g) => <tr key={g.key} className={g.contracts ? undefined : "text-faint"}>
        <td className="p-2 text-left">{g.label}</td><td className="p-2">{g.fills}</td><td className="p-2">{g.contracts}</td>
        {places.map(([key]) => <td key={key} className="p-2">{share(g.places[key], g.contracts)}</td>)}
        <td className="p-2">{ticks(g.from_mid_ticks)}</td><td className="p-2">{ticks(g.vs_natural_ticks)}</td>
        <td className="p-2">{g.contracts ? signedMoney(g.cost_vs_mid) : "—"}</td>
      </tr>)}</tbody>
    </table>
  </div>
}

export function Suggestion({ suggestion, title }: { suggestion: FillCalibration; title: string }) {
  const settings = [
    suggestion.fill_latency_ms != null ? `${suggestion.fill_latency_ms.toLocaleString("en-US")} ms latency` : "latency not measured",
    `${suggestion.slippage_ticks} slippage ${suggestion.slippage_ticks === 1 ? "tick" : "ticks"}`,
    `${suggestion.impact_ticks} impact ${suggestion.impact_ticks === 1 ? "tick" : "ticks"}`,
    ...(suggestion.inside_fill_percent ? [`inside fills at ${suggestion.inside_fill_percent}%`] : []),
  ]
  const model = { as_displayed: "As displayed", conservative: "Conservative", midpoint: "Inside at midpoint", custom: "Custom rules" }[suggestion.fill_model]
  return <div className="rounded-md border border-border bg-raised/40 p-3 text-xs">
    <div className="text-sm font-medium">{title}: {model}</div>
    <div className="mt-1 tabular">{settings.join(" · ")}</div>
    <ul className="mt-2 list-disc space-y-0.5 pl-4 text-muted">{suggestion.reasons.map((reason) => <li key={reason}>{reason}</li>)}</ul>
  </div>
}

/** The journal's simulated fills against their books, then the real-fill comparison. */
export function FillQualityPanel({ scope }: { scope: "current" | "all" }) {
  const quality = useFillQuality(scope)
  const [by, setBy] = useState<"time_of_day" | "spread" | "dte">("time_of_day")
  const report = quality.data
  return (
    <Panel title="Fill quality" actions={<Segmented label="Group fills by" value={by} onChange={setBy}
      options={[{ value: "time_of_day", label: "Time of day" }, { value: "spread", label: "Spread" }, { value: "dte", label: "DTE" }]} />}>
      {quality.error ? <TradingError error={quality.error} />
        : !report ? <Empty>Loading fill quality…</Empty>
        : <div className="space-y-3">
          <p className="text-xs text-muted">
            {report.measured} simulated {report.measured === 1 ? "fill" : "fills"} placed on the book they traded against, from the mid toward the natural (the far side){report.unmeasured ? `; ${report.unmeasured} recorded without a two-sided book are left out` : ""}.
            Ticks use the natural price's tick size. New York time; spreads in whole ticks; DTE in calendar days.
          </p>
          <FillGroups label="Fill quality" groups={[report.overall, ...report[by]]} />
          <Suggestion title="Suggested Conservative calibration" suggestion={report.suggestion} />
          <RealFills />
        </div>}
    </Panel>
  )
}

const outcome = (o: PaperOutcome) => o.fills ? formatMoney(o.price) : "rests"

/** What a broker export compares with: its fills, the books and the paper models. */
export function CalibrationResult({ result }: { result: Calibration }) {
  const rows = result.rows.slice(0, 100)
  return <div className="space-y-3">
    <p className="text-xs text-muted">
      Read {result.format}: {result.matched} of {result.matched + result.unmatched} {result.matched + result.unmatched === 1 ? "fill" : "fills"} matched a quote at the same time
      {result.skipped.length ? `; ${result.skipped.length} ${result.skipped.length === 1 ? "row" : "rows"} skipped` : ""}.
      With a limit at each real price, as displayed would have filled {result.paper_fills.as_displayed}, Conservative {result.paper_fills.conservative} and
      inside at midpoint {result.paper_fills.midpoint} of the matched fills at once. Nothing was stored, and the account is unchanged.
    </p>
    {result.matched > 0 && <FillGroups label="Real fill quality" groups={[result.real]} />}
    <Suggestion title="Suggested from your real fills" suggestion={result.suggestion} />
    <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Real fills">
      <table className="w-full whitespace-nowrap text-right text-xs tabular" aria-label="Real fills">
        <thead><tr className="text-muted">{["Row", "Time", "Contract", "Side", "Qty", "Price", "Book", "Place", "vs natural", "As displayed", "Conservative", "Midpoint", "Note"]
          .map((h, i) => <th key={h} className={`p-2 ${i < 4 || i === 12 ? "text-left" : ""}`}>{h}</th>)}</tr></thead>
        <tbody>{rows.map((r) => <tr key={r.row}>
          <td className="p-2 text-left">{r.row}</td>
          <td className="p-2 text-left">{r.time ? `${when.format(Date.parse(r.time))} ET` : "date only"}</td>
          <td className="p-2 text-left">{osiLabel(r.symbol)}</td>
          <td className="p-2 text-left">{r.side}</td><td className="p-2">{r.quantity}</td><td className="p-2">{formatMoney(r.price)}</td>
          <td className="p-2">{r.quote ? `${formatMoney(r.quote.bid)} / ${formatMoney(r.quote.ask)} · ${r.quote.source}` : "—"}</td>
          <td className="p-2">{r.place ? places.find(([key]) => key === r.place)?.[1] : "—"}</td>
          <td className="p-2">{ticks(r.vs_natural_ticks)}</td>
          <td className="p-2">{r.quote ? outcome(r.as_displayed) : "—"}</td><td className="p-2">{r.quote ? outcome(r.conservative) : "—"}</td>
          <td className="p-2">{r.quote ? outcome(r.midpoint) : "—"}</td>
          <td className="p-2 text-left text-muted">{r.note ?? ""}</td>
        </tr>)}</tbody>
      </table>
    </div>
    {result.rows.length > rows.length && <p className="text-xs text-muted">Showing the first {rows.length} of {result.rows.length} fills.</p>}
  </div>
}

/** Upload a broker's fills for analysis only; they never reach an account. */
function RealFills() {
  const [format, setFormat] = useState<CalibrationFormat>("auto")
  const [result, setResult] = useState<Calibration | null>(null)
  const [error, setError] = useState<unknown>()
  const [pending, setPending] = useState(false)
  async function analyse(file: File | undefined) {
    if (!file) return
    setError(undefined)
    const text = await file.text()
    if (text.length > MAX_CSV) { setError(new Error(`The file is ${Math.ceil(text.length / 1000)} KB; split exports over 60 KB into several imports.`)); return }
    setPending(true)
    try { setResult(await api.calibrateFills(text, format)) } catch (failure) { setError(failure) } finally { setPending(false) }
  }
  return <section className="space-y-2 border-t border-border pt-3" aria-label="Real fill calibration">
    <h3 className="text-sm font-medium">Calibrate from real fills</h3>
    <p className="text-xs text-muted">
      Import your broker's fills (CSV: generic time, symbol, side, quantity and price columns with optional bid and ask; tastytrade transactions;
      a thinkorswim statement or Schwab transactions; IBKR Flex trades or an activity statement). Each fill is compared with the quote at the same time
      from the recorded days the replay picker lists, or the file's own bid and ask. Analysis only: nothing is stored or traded.
    </p>
    <div className="flex flex-wrap items-end gap-3 text-xs">
      <label className="trade-label">Format
        <select className="trade-input !w-auto" aria-label="Export format" value={format} disabled={pending} onChange={(e) => setFormat(e.target.value as CalibrationFormat)}>
          <option value="auto">Detect</option><option value="generic">Generic</option><option value="tastytrade">tastytrade</option>
          <option value="schwab">Schwab / thinkorswim</option><option value="ibkr">IBKR</option>
        </select>
      </label>
      <label className="trade-label">Broker fills CSV
        <input type="file" accept=".csv,text/csv" className="trade-input" aria-label="Broker fills CSV" disabled={pending}
          onChange={(e) => { void analyse(e.target.files?.[0]); e.target.value = "" }} />
      </label>
      {pending && <span className="text-muted">Comparing…</span>}
    </div>
    <TradingError error={error} />
    {result && <CalibrationResult result={result} />}
  </section>
}
