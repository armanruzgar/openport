import { studyPrefs } from "../lib/studies"
import { loadDrawings, saveDrawings, type Drawing, type DrawingPoint } from "../lib/drawings"
import { ChartDrawings } from "./ChartDrawings"
import { Popout } from "./TerminalTools"
import { useQuery } from "@tanstack/react-query"
import { useEffect, useMemo, useState } from "react"
import { useBriefMarket } from "../api/brief"
import { saveBriefLevels, useBriefLevels } from "../lib/brief-preferences"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { useOpenOrders, usePortfolio } from "../api/trading"
import type { Expiry } from "../api/types"
import { bandColor, CandleChart, levelColors } from "../charts/CandleChart"
import { describeAlert, useAlertSettings } from "../lib/alerts"
import { candleIntervals, candleLimit, chartLevels, expectedMove, intervalNames, loadChartPrefs, saveChartPrefs, type ChartLevel, type ChartPrefs } from "../lib/candles"
import { expiryLabel, isNum, price } from "../lib/format"
import { useMediaQuery } from "../lib/media"
import { AlertsDialog } from "./Alerts"
import { Segmented } from "./ui"

const priceText = (x: number) => x.toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 })

function Key({ color, children, title }: { color: string; children: string; title?: string }) {
  return (
    <span className="inline-flex items-center gap-1.5" title={title}>
      <span className="inline-block h-0.5 w-3" style={{ background: color }} />
      {children}
    </span>
  )
}

/**
 * The underlying's candles on Trade, with the account's strikes, armed underlying
 * triggers, price alerts and the selected expiry's expected move drawn over them.
 */
export function PriceChart({ symbol, spot, expiry }: { symbol: string; spot: number | null; expiry: Expiry | null }) {
  const live = useLive()
  const version = live.version(symbol)
  const [prefs, setPrefs] = useState(loadChartPrefs)
  const update = (patch: Partial<ChartPrefs>) => setPrefs((current) => {
    const next = { ...current, ...patch }
    saveChartPrefs(next)
    return next
  })
  useEffect(() => {
    const reload = () => setPrefs(loadChartPrefs())
    window.addEventListener("openport:workspace", reload)
    return () => window.removeEventListener("openport:workspace", reload)
  }, [])
  const studies = studyPrefs(prefs.studies)
  const [drawings, setDrawings] = useState(() => loadDrawings(symbol))
  const [drawMode, setDrawMode] = useState<"horizontal" | "trend" | null>(null)
  const [anchor, setAnchor] = useState<DrawingPoint | null>(null)
  useEffect(() => { setDrawings(loadDrawings(symbol)); setDrawMode(null); setAnchor(null) }, [symbol])
  const changeDrawings = (next: Drawing[]) => { setDrawings(next); saveDrawings(symbol, next) }
  const draw = (point: DrawingPoint) => {
    if (drawMode === "trend" && !anchor) { setAnchor(point); return }
    if (drawMode === "trend" && anchor?.time === point.time) return
    const drawing: Drawing = drawMode === "trend" && anchor ? { id: crypto.randomUUID(), kind: "trend", start: anchor, end: point }
      : { id: crypto.randomUUID(), kind: "horizontal", price: point.price }
    changeDrawings([...drawings, drawing].slice(-100)); setDrawMode(null); setAnchor(null)
  }
  const height = useMediaQuery("(min-width: 640px)") ? 300 : 220
  const candles = useQuery({
    queryKey: ["candles", symbol, prefs.interval, version],
    queryFn: ({ signal }) => api.candles(symbol, prefs.interval, candleLimit, signal),
    placeholderData: (previous) => previous?.symbol === symbol && previous.interval === prefs.interval ? previous : undefined,
    // Vendor history arrives between the feed's own updates.
    refetchInterval: 60_000,
    enabled: !prefs.hidden,
  })
  const showBrief = useBriefLevels()
  const brief = useBriefMarket(symbol, showBrief && !prefs.hidden, false)
  const positions = usePortfolio().data?.positions
  const orders = useOpenOrders().data?.orders
  const alerts = useAlertSettings().prices
  const [alerting, setAlerting] = useState<{ level: number | null } | null>(null)
  const levels = useMemo(() => [...chartLevels(symbol, positions ?? [], orders ?? []),
    ...alerts.filter((a) => a.symbol === symbol).map((a): ChartLevel => ({ price: a.level, label: `Alert: ${describeAlert(a)}`, kind: "alert" }))],
  [symbol, positions, orders, alerts])
  const displayedLevels = showBrief ? [...levels, ...brief.levels] : levels
  const move = expectedMove(spot, expiry?.atm_iv, expiry?.days)
  const band = move != null && isNum(spot) && expiry
    ? { lo: spot - move, hi: spot + move, label: `±1σ by ${expiryLabel(expiry.id)}: ±${price(move)}` }
    : null
  const data = candles.data
  const bars = data?.symbol === symbol && data.interval === prefs.interval ? data.bars : []
  const kinds = new Set(levels.map((level) => level.kind))
  const name = `${intervalNames[prefs.interval]} candles`
  return (
    <section className="min-w-0 rounded-lg border border-border bg-panel" aria-label={`${symbol} chart`}>
      <header className={`flex flex-wrap items-center justify-between gap-2 px-3 py-2 ${prefs.hidden ? "" : "border-b border-border"}`}>
        <h2 className="text-xs font-medium uppercase tracking-wide text-muted">
          {symbol} <span className="normal-case text-faint">{name}</span>
        </h2>
        <div className="flex flex-wrap items-center gap-2">
          <Popout route={{ symbol, view: "chart", expiry: expiry?.id ?? null }} label="chart" />
          <button type="button" onClick={() => setAlerting({ level: spot })} className="rounded-md border border-border px-2 py-1 text-xs text-muted hover:text-foreground">Alert</button>
          {!prefs.hidden && <label className="flex items-center gap-1 text-xs text-muted"><input type="checkbox" checked={showBrief} onChange={(event) => saveBriefLevels(event.target.checked)} />Brief levels</label>}
          {!prefs.hidden && <Segmented label="Chart interval" value={prefs.interval} options={candleIntervals} onChange={(interval) => update({ interval })} />}
          <button type="button" aria-expanded={!prefs.hidden} onClick={() => update({ hidden: !prefs.hidden })}
            className="rounded-md border border-border px-2 py-1 text-xs text-muted hover:text-foreground">
            {prefs.hidden ? "Show chart" : "Hide"}
          </button>
        </div>
      </header>
      {!prefs.hidden && (
        <div className="p-3">
          <div className="mb-2 flex flex-wrap items-center gap-3 text-xs">
            {(["sma", "ema", "bollinger", "vwap"] as const).map((study) => <label key={study} className="flex items-center gap-1">
              <input type="checkbox" checked={studies[study]} onChange={(event) => update({ studies: { ...studies, [study]: event.target.checked } })} />
              {study === "bollinger" ? "Bollinger ±2σ" : study.toUpperCase()}
            </label>)}
            <label className="flex items-center gap-1">Period<input className="trade-input !w-16" aria-label="Study period" type="number" min={2} max={200} value={studies.period}
              onChange={(event) => { const period = Number(event.target.value); if (Number.isInteger(period) && period >= 2 && period <= 200) update({ studies: { ...studies, period } }) }} /></label>
            <button className="trade-button" aria-pressed={drawMode === "horizontal"} onClick={() => { setDrawMode(drawMode === "horizontal" ? null : "horizontal"); setAnchor(null) }}>Draw level</button>
            <button className="trade-button" aria-pressed={drawMode === "trend"} onClick={() => { setDrawMode(drawMode === "trend" ? null : "trend"); setAnchor(null) }}>Draw trendline</button>
            {drawMode && <button className="trade-button" onClick={() => { setDrawMode(null); setAnchor(null) }}>Cancel drawing</button>}
          </div>
          {drawMode && <p role="status" className="mb-2 text-xs text-accent">{drawMode === "horizontal" ? "Tap the chart to place a horizontal line." : anchor ? "Tap a different bar for the second anchor." : "Tap the first trendline anchor, then a different bar."}</p>}
          {studies.vwap && <p className="mb-2 text-xs text-muted">VWAP requires bar volume. The current candle endpoint supplies OHLC only; no VWAP is inferred.</p>}
          {(studies.sma || studies.ema || studies.bollinger) && <p className="mb-2 text-[11px] text-muted">Derived from loaded closes · {studies.period}-bar warm-up · EMA seeded with SMA · Bollinger uses population deviation.</p>}
          {bars.length ? (
            <CandleChart key={`${symbol}/${prefs.interval}`} bars={bars} interval={prefs.interval} levels={displayedLevels} band={band}
              studies={studies} drawings={drawings} onDrawPoint={drawMode ? draw : undefined} height={height} formatPrice={priceText} label={`${symbol} ${name}`} />
          ) : (
            <div className="flex items-center justify-center px-4 text-center text-sm text-muted" style={{ height }}>
              {candles.isError ? String(candles.error)
                : candles.isPending ? "Loading bars…"
                : "No bars yet. They build from the feed's prices as it updates, and Cboe's history fills in when it arrives."}
            </div>
          )}
          <ChartDrawings key={`drawings/${symbol}/${prefs.interval}`} bars={bars} interval={prefs.interval} drawings={drawings} onChange={changeDrawings} onAlert={(level) => setAlerting({ level })} />
          <div className="mt-2 flex flex-wrap items-center gap-x-3 gap-y-1 text-[11px] text-muted">
            {kinds.has("long") && <Key color={levelColors.long} title="Strikes you hold, net long">Long strike</Key>}
            {kinds.has("short") && <Key color={levelColors.short} title="Strikes you hold, net short">Short strike</Key>}
            {kinds.has("trigger") && <Key color={levelColors.trigger} title="Armed orders that wait for the underlying to cross a level">Trigger</Key>}
            {kinds.has("alert") && <Key color={levelColors.alert} title="Price alerts set in this browser">Alert</Key>}
            {showBrief && <Key color={levelColors.brief}>Brief levels</Key>}
            {band && <Key color={bandColor} title={`One standard deviation by ${expiry?.expiry}: spot × ATM IV × √(days / 365)`}>Expected move</Key>}
            <span className="ml-auto text-faint">ET, market-data time · drag to pan, pinch or ⌘/Ctrl-scroll to zoom, double-click to reset</span>
          </div>
        </div>
      )}
      {alerting && <AlertsDialog initial={{ symbol, level: alerting.level }} onClose={() => setAlerting(null)} />}
    </section>
  )
}
