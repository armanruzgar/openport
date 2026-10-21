import { useQuery } from "@tanstack/react-query"
import { useMemo, useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { LineChart, type Series } from "../charts/LineChart"
import { VolatilityDetails, VolatilitySummary } from "../components/VolatilityMetrics"
import { VolatilityHistory } from "../components/VolatilityHistory"
import { ProbabilityPanel } from "../components/ProbabilityPanel"
import { volatilityTerm, volPoints } from "../lib/volatility"
import { SviTable } from "../components/SviTable"
import { Empty, Panel, Segmented } from "../components/ui"
import { days, expiryLabel, fixed, isNum, pct, price, vol } from "../lib/format"
import { comparisonLabels, comparisonSeries, smileSeries, type SmileMode } from "../lib/svi"
import { matchingPayload } from "../lib/payload"

// Natural tenors, placed on the square-root time axis.
const tenorTicks = [7, 30, 91, 182, 365, 730, 1826].map(Math.sqrt)
const tenor = (d: number) => (d < 25 ? "1w" : d < 60 ? "1m" : d < 120 ? "3m" : d < 250 ? "6m" : d < 500 ? "1y" : d < 1000 ? "2y" : "5y")

export function SmileView({ symbol }: { symbol: string }) {
  const live = useLive()
  const version = live.version(symbol)
  const [expiries, setExpiries] = useState(6)
  const [window, setWindow] = useState(0.1)
  const [mode, setMode] = useState<SmileMode>("all")
  const [axis, setAxis] = useState<"strike" | "moneyness">("strike")
  const [comparisonExpiry, setComparisonExpiry] = useState("")

  const surface = useQuery({
    queryKey: ["surface", symbol, expiries, window, version],
    queryFn: ({ signal }) => api.surface(symbol, expiries, window, signal),
    placeholderData: (previous) => matchingPayload(previous, symbol),
  })
  const summary = useQuery({
    queryKey: ["summary", symbol, version],
    queryFn: ({ signal }) => api.summary(symbol, signal),
    placeholderData: (previous) => matchingPayload(previous, symbol),
  })
  const volatility = useQuery({
    queryKey: ["volatility", symbol, version, live.account],
    queryFn: ({ signal }) => api.volatility(symbol, signal),
    refetchInterval: 60_000,
    placeholderData: (previous, query) => query?.queryKey[3] === live.account ? matchingPayload(previous, symbol) : undefined,
  })
  const volatilityData = matchingPayload(volatility.data, symbol)
  const history = useQuery({
    queryKey: ["volatility-series", symbol, Math.floor(Date.parse(volatilityData?.as_of ?? "") / 60_000) || 0],
    queryFn: ({ signal }) => api.series(symbol, signal),
    placeholderData: previous => matchingPayload(previous, symbol),
    staleTime: 60_000,
    refetchInterval: 60_000,
  })
  const surfaceData = matchingPayload(surface.data, symbol)
  const summaryData = matchingPayload(summary.data, symbol)
  const surfaceHistory = useQuery({
    queryKey: ["surface-history", symbol, Math.floor(Date.parse(surfaceData?.as_of ?? "") / 60_000) || 0],
    queryFn: ({ signal }) => api.surfaceHistory(symbol, signal),
    staleTime: 60_000,
  })
  const comparisonData = matchingPayload(surfaceHistory.data, symbol)

  const shown = useMemo(() => (surfaceData?.expiries ?? []).filter((e) => (e.days ?? 0) > 1 / 24), [surfaceData])
  const smiles = useMemo(() => smileSeries(shown, axis, mode, { spot: surfaceData?.spot ?? null, window }, surfaceData?.ssvi),
    [shown, axis, mode, surfaceData?.spot, surfaceData?.ssvi, window])
  const selectedComparison = shown.find(e => e.id === comparisonExpiry) ?? shown[0]
  const comparisons = comparisonSeries(selectedComparison, comparisonData, axis, { spot: surfaceData?.spot ?? null, window })

  const term = useMemo<Series[]>(() => {
    if (volatilityData) return volatilityTerm(volatilityData)
    // Square-root time axis: a week and five years both stay readable.
    const points = (summaryData?.expiries ?? [])
      .filter((e) => (e.days ?? 0) > 1 / 24 && isNum(e.atm_iv))
      .map((e) => ({ x: Math.sqrt(e.days ?? 0), y: (e.atm_iv ?? 0) * 100 }))
    return [{ id: "atm", label: "ATM IV (summary)", color: "var(--chart-2)", points }]
  }, [summaryData, volatilityData])

  const spot = surfaceData?.spot
  const markers = axis === "strike"
    ? isNum(spot) ? [{ x: spot, label: `spot ${price(spot)}`, color: "var(--warn)" }] : []
    : [{ x: 0, label: "forward", color: "var(--warn)" }]

  if (surface.isError) return <Empty>{String(surface.error)}</Empty>
  return (
    <div className="grid gap-3 xl:grid-cols-[2fr_1fr]">
      {volatilityData ? <VolatilitySummary data={volatilityData} /> : <div className="text-xs text-muted xl:col-span-2">{volatility.isError ? `Volatility metrics unavailable: ${String(volatility.error)}` : "Loading volatility metrics…"}</div>}
      {volatilityData && <VolatilityHistory data={volatilityData} history={matchingPayload(history.data, symbol)} error={history.isError ? String(history.error) : undefined} />}
      <Panel
        title="Volatility smile"
        actions={
          <>
            <Segmented label="Smile display" value={mode}
              options={[{ value: "market", label: "Market" }, { value: "svi", label: "SVI" }, { value: "ssvi", label: "SSVI" }, { value: "all", label: "All" }]}
              onChange={setMode} />
            <Segmented label="Expiries" value={expiries} options={[3, 6, 10].map((n) => ({ value: n, label: `${n} exp` }))} onChange={setExpiries} />
            <Segmented label="Window" value={window} options={[0.05, 0.1, 0.2].map((w) => ({ value: w, label: `±${w * 100}%` }))} onChange={setWindow} />
            <Segmented
              label="Axis"
              value={axis}
              options={[
                { value: "strike", label: "Strike" },
                { value: "moneyness", label: "ln(K/F)" },
              ]}
              onChange={setAxis}
            />
          </>
        }
      >
        {smiles.length ? (
          <>
            <LineChart
              series={smiles}
              markers={markers}
              height={380}
              formatX={(x) => (axis === "strike" ? fixed(x, 0) : fixed(x, 3))}
              formatY={(y) => vol(y, 1)}
              xLabel={axis === "strike" ? "strike" : "log-moneyness"}
            />
            <p className="mt-2 text-[11px] text-muted">
              Dots: OTM market IV. Solid: SVI. Dashed: SSVI. The nearest expiry's market bid-ask range is shaded.
              Fits use all eligible quotes; SVI arbitrage checks cover a finite grid.
            </p>
          </>
        ) : (
          <Empty>{surfaceData ? "No usable points or fits for this selection." : "Loading smiles…"}</Empty>
        )}
        <SviTable ssvi={surfaceData?.ssvi} expiries={shown} violations={surfaceData?.calendar_violations ?? []} />
      </Panel>

      <div className="flex flex-col gap-3">
        <Panel title="Model-free / ATM term structure">
          {term[0]?.points.length ? (
            <LineChart
              series={term}
              height={220}
              formatX={(x) => tenor(x * x)}
              formatY={(y) => volPoints(y, 1)}
              xTicks={tenorTicks}
            />
          ) : (
            <Empty>Loading…</Empty>
          )}
          <p className="text-[11px] text-muted">{volatilityData && <><span style={{ color: "var(--chart-1)" }}>Model-free IV{volatilityData.mfiv.expiries.some(e => e.proxy) ? " ≈" : ""}</span> · </>}<span style={{ color: "var(--chart-2)" }}>ATM IV{volatilityData?.atm.expiries.some(e => e.source !== "svi") ? " ≈" : ""}</span> · <span className="text-warn">dots: truncated †</span></p>
        </Panel>
        <Panel title="Forwards">
          <div className="max-h-72 overflow-auto">
            <table className="w-full text-xs">
              <thead className="sticky top-0 bg-panel text-muted">
                <tr>
                  <th className="py-1 text-left font-normal">Expiry</th>
                  <th className="py-1 text-right font-normal">Days</th>
                  <th className="py-1 text-right font-normal">Forward</th>
                  <th className="py-1 text-right font-normal">Rate</th>
                  <th className="py-1 text-right font-normal">ATM vol</th>
                </tr>
              </thead>
              <tbody className="tabular">
                {(summaryData?.expiries ?? []).map((e) => (
                  <tr key={e.id} className="border-t border-border/40">
                    <td className="py-0.5">{expiryLabel(e.id, true)}</td>
                    <td className="py-0.5 text-right">{days(e.days)}</td>
                    <td className="py-0.5 text-right">{price(e.forward)}</td>
                    <td className="py-0.5 text-right" title={e.rate_fitted ? "fitted" : "borrowed from longer expiries"}>
                      {pct(e.rate, 2)}
                      {e.rate_fitted ? "" : "*"}
                    </td>
                    <td className="py-0.5 text-right">{vol(e.atm_iv)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </Panel>
      </div>
      {volatilityData && <VolatilityDetails data={volatilityData} />}
      <Panel title="Historical smile comparison · same listed expiry" className="xl:col-span-2" actions={<select aria-label="Comparison expiry" value={selectedComparison?.id ?? ""} onChange={event => setComparisonExpiry(event.target.value)} className="rounded border border-border bg-panel px-2 py-1 text-xs">
        {shown.map(e => <option key={e.id} value={e.id}>{expiryLabel(e.id, true)}</option>)}
      </select>}>
        <LineChart series={comparisons} height={280} formatX={x => fixed(x, axis === "strike" ? 0 : 3)} formatY={y => vol(y, 1)} xLabel={axis === "strike" ? "strike" : "log-moneyness"} />
        <div className="mt-2 space-y-1 text-[11px] text-muted">
          <p>Current sample: {surfaceData?.as_of ?? "unavailable"}</p>
          {surfaceHistory.isError ? <p>{String(surfaceHistory.error)}</p> : !comparisonData ? <p>Loading surface history…</p> : !comparisonData.enabled ? <p>Surface collection is disabled for this source.</p> : comparisonData.comparisons.map(c => <p key={c.label}>{comparisonLabels[c.label]} · {c.date}: {c.snapshot ? `sample ${c.snapshot.as_of}${c.snapshot.expiries.some(e => e.id === selectedComparison?.id && e.svi) ? "" : " · no fit for this expiry"}` : "no close snapshot"}</p>)}
          <p>Derived SVI curves at each sample's own tenor and forward, clipped to its calibration range. Close samples come from the final five session minutes; timestamps show actual observations. Comparisons do not hold time to expiry constant. Flagged fits remain diagnostic.</p>
        </div>
      </Panel>
      <ProbabilityPanel symbol={symbol} />
    </div>
  )
}
