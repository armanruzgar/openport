import { useMemo, useState, type KeyboardEvent } from "react"
import type { Marker } from "./LineChart"
import { extent, labelRows, linear, niceTicks, pad } from "./scale"
import { useSize } from "./useSize"

interface Props {
  bars: { x: number; value: number }[]
  markers?: Marker[]
  height?: number
  formatX: (x: number) => string
  formatY: (y: number) => string
  /** What the chart shows, for assistive technology; its bar count and range otherwise. */
  label?: string
}

const margin = { top: 14, right: 16, bottom: 28, left: 60 }

/// Signed bars along a numeric axis (exposure by strike): positive teal, negative violet.
/// The arrow keys move from bar to bar, and a live region reads each one out.
export function BarChart({ bars, markers = [], height = 280, formatX, formatY, label }: Props) {
  const [ref, { width }] = useSize<HTMLDivElement>()
  const [hover, setHover] = useState<number | null>(null)
  const [keyed, setKeyed] = useState(false)
  const onKey = (event: KeyboardEvent<SVGSVGElement>) => {
    if (!bars.length) return
    if (event.key === "Escape") { setHover(null); return }
    const next = event.key === "Home" ? 0 : event.key === "End" ? bars.length - 1
      : event.key === "ArrowRight" ? (hover == null ? 0 : Math.min(bars.length - 1, hover + 1))
      : event.key === "ArrowLeft" ? (hover == null ? bars.length - 1 : Math.max(0, hover - 1)) : null
    if (next == null) return
    event.preventDefault()
    setHover(next)
    setKeyed(true)
  }

  const layout = useMemo(() => {
    const xDomain = extent(bars.map((b) => b.x))
    const yDomain = extent([0, ...bars.map((b) => b.value)])
    if (!xDomain || !yDomain || width === 0) return null
    const step = bars.length > 1 ? (xDomain[1] - xDomain[0]) / (bars.length - 1) : 1
    const x = linear([xDomain[0] - step / 2, xDomain[1] + step / 2], [margin.left, width - margin.right])
    const y = linear(pad(yDomain, 0.06), [height - margin.bottom, margin.top])
    const barWidth = Math.max(1, Math.abs(x(step) - x(0)) * 0.8)
    const rows = labelRows(markers.map((m) => ({ x: x(m.x), label: m.label })))
    return { x, y, barWidth, rows, xTicks: niceTicks(...xDomain, Math.max(2, Math.floor(width / 110))), yTicks: niceTicks(...y.domain, 5) }
  }, [bars, markers, width, height])

  const hovered = hover == null ? null : bars[hover]

  return (
    <div ref={ref} className="relative w-full select-none" style={{ height }}>
      {layout && (
        <svg width={width} height={height} className="block" onPointerLeave={() => setHover(null)} role="img" aria-roledescription="chart"
          aria-label={label ?? `${bars.length} bars from ${formatX(bars[0]!.x)} to ${formatX(bars[bars.length - 1]!.x)}`}
          tabIndex={0} onKeyDown={onKey} onBlur={() => { if (keyed) { setHover(null); setKeyed(false) } }}>
          {layout.yTicks.map((t) => (
            <g key={t}>
              <line x1={margin.left} x2={width - margin.right} y1={layout.y(t)} y2={layout.y(t)} className={t === 0 ? "stroke-muted" : "stroke-border"} strokeDasharray={t === 0 ? undefined : "2 3"} />
              <text x={margin.left - 8} y={layout.y(t)} dy="0.32em" textAnchor="end" className="fill-muted tabular text-[10px]">
                {formatY(t)}
              </text>
            </g>
          ))}
          {layout.xTicks.map((t) => (
            <text key={t} x={layout.x(t)} y={height - 8} textAnchor="middle" className="fill-muted tabular text-[10px]">
              {formatX(t)}
            </text>
          ))}
          {bars.map((b, i) => {
            const y0 = layout.y(0)
            const y1 = layout.y(b.value)
            return (
              <rect
                key={b.x}
                x={layout.x(b.x) - layout.barWidth / 2}
                y={Math.min(y0, y1)}
                width={layout.barWidth}
                height={Math.max(0.5, Math.abs(y1 - y0))}
                className={b.value >= 0 ? "fill-gex-positive" : "fill-gex-negative"}
                opacity={hover == null || hover === i ? 0.9 : 0.45}
                onPointerEnter={() => { setHover(i); setKeyed(false) }}
              />
            )
          })}
          {markers.map((m, i) => (
            <g key={m.label}>
              <line x1={layout.x(m.x)} x2={layout.x(m.x)} y1={margin.top} y2={height - margin.bottom} style={{ stroke: m.color }} strokeDasharray="4 3" />
              <text x={layout.x(m.x) + 4} y={margin.top + 8 + 12 * layout.rows[i]!} style={{ fill: m.color, stroke: "var(--panel)", strokeWidth: 3, paintOrder: "stroke" }} className="text-[10px]">
                {m.label}
              </text>
            </g>
          ))}
        </svg>
      )}
      {hovered && layout && (
        <div
          className="pointer-events-none absolute top-2 rounded-md border border-border bg-tooltip px-2 py-1 text-[11px] shadow-chart"
          style={{ left: Math.min(layout.x(hovered.x) + 10, width - 150) }}
        >
          <span className="text-muted tabular">{formatX(hovered.x)}</span>{" "}
          <span className="tabular">{formatY(hovered.value)}</span>
        </div>
      )}
      <p className="sr-only" aria-live="polite">{keyed && hovered ? `${formatX(hovered.x)}: ${formatY(hovered.value)}` : ""}</p>
    </div>
  )
}
