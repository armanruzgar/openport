import { useState } from "react"
import type { Candle, CandleInterval } from "../api/types"
import { barTime } from "../lib/candles"
import type { Drawing } from "../lib/drawings"
import { price } from "../lib/format"

export function ChartDrawings({ bars, interval, drawings, onChange, onAlert }: {
  bars: readonly Candle[]; interval: CandleInterval; drawings: Drawing[]; onChange: (next: Drawing[]) => void; onAlert: (level: number) => void
}) {
  const [kind, setKind] = useState<"horizontal" | "trend">("horizontal")
  const [level, setLevel] = useState("")
  const [endPrice, setEndPrice] = useState("")
  const [startTime, setStartTime] = useState("")
  const [endTime, setEndTime] = useState("")
  const start = Number(startTime || bars[0]?.t), end = Number(endTime || bars.at(-1)?.t)
  const valid = Number.isFinite(Number(level)) && Number(level) > 0 && (kind === "horizontal" ||
    (Number.isFinite(Number(endPrice)) && Number(endPrice) > 0 && Number.isFinite(start) && Number.isFinite(end) && start !== end))
  return <details className="mt-2 text-xs">
    <summary className="cursor-pointer py-2 text-muted">Drawings · {drawings.length} · keyboard entry and saved lines</summary>
    <form className="mt-2 flex flex-wrap items-end gap-2" onSubmit={(event) => {
      event.preventDefault(); if (!valid) return
      const id = crypto.randomUUID()
      onChange([...drawings, kind === "horizontal" ? { id, kind, price: Number(level) } : { id, kind, start: { time: start, price: Number(level) }, end: { time: end, price: Number(endPrice) } }].slice(-100))
      setLevel(""); setEndPrice("")
    }}>
      <label className="trade-label">Drawing<select className="trade-input" value={kind} onChange={(e) => setKind(e.target.value as typeof kind)}><option value="horizontal">Horizontal line</option><option value="trend">Trendline</option></select></label>
      <label className="trade-label">{kind === "trend" ? "Start price" : "Line price"}<input className="trade-input" inputMode="decimal" value={level} onChange={(e) => setLevel(e.target.value)} /></label>
      {kind === "trend" && <>
        <label className="trade-label">End price<input className="trade-input" inputMode="decimal" value={endPrice} onChange={(e) => setEndPrice(e.target.value)} /></label>
        <label className="trade-label">Start bar<select className="trade-input" value={startTime || String(bars[0]?.t ?? "")} onChange={(e) => setStartTime(e.target.value)}>{bars.map((b) => <option key={b.t} value={b.t}>{barTime(b.t, interval)}</option>)}</select></label>
        <label className="trade-label">End bar<select className="trade-input" value={endTime || String(bars.at(-1)?.t ?? "")} onChange={(e) => setEndTime(e.target.value)}>{bars.map((b) => <option key={b.t} value={b.t}>{barTime(b.t, interval)}</option>)}</select></label>
      </>}
      <button className="trade-button" disabled={!valid}>Add drawing</button>
    </form>
    <ul className="mt-2 max-h-48 overflow-y-auto">{drawings.map((d) => <li key={d.id} className="flex flex-wrap items-center gap-2 border-t border-border py-2">
      <span className="mr-auto">{d.kind === "horizontal" ? `Level ${price(d.price)}` : `Trend ${price(d.start.price)} → ${price(d.end.price)}`}</span>
      {d.kind === "horizontal" && <button className="trade-button" onClick={() => onAlert(d.price)}>Create price alert</button>}
      <button className="trade-button" aria-label={`Remove ${d.kind === "horizontal" ? `line ${d.price}` : "trendline"}`} onClick={() => onChange(drawings.filter((line) => line.id !== d.id))}>Remove</button>
    </li>)}</ul>
    <p className="mt-2 text-faint">Drawings are your annotations, stored per symbol in this browser. Trendlines appear when both anchor times are in the loaded history.</p>
  </details>
}
