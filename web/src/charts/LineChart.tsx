import { useMemo, useState, type PointerEvent } from "react"
import { extent, labelRows, linear, niceTicks, pad } from "./scale"
import { useSize } from "./useSize"

export interface SeriesPoint {
  x: number
  y: number | null
  lo?: number | null
  hi?: number | null
}

export interface Series {
  id: string
  label: string
  color: string
  points: SeriesPoint[]
  band?: boolean
  dots?: boolean
  dashed?: boolean
  /** A fine dotted stroke, distinct from dashed. */
  dotted?: boolean
  /** Shade between the line and the bottom of the plot. */
  area?: boolean
}

export interface Marker {
  x: number
  label: string
  color: string
}

/** A horizontal level such as a target or floor, always kept in view. */
export interface Reference {
  y: number
  label: string
  color: string
}

interface Props {
  series: Series[]
  markers?: Marker[]
  references?: Reference[]
  height?: number
  formatX: (x: number) => string
  formatY: (y: number) => string
  xLabel?: string
  /** Tick positions for a transformed axis; round numbers are chosen otherwise. */
  xTicks?: number[]
  /** Left margin for wide y labels such as dollar balances. */
  marginLeft?: number
}

const margin = { top: 12, right: 16, bottom: 28, left: 52 }

/// Multi-series line chart with optional bid/ask bands, vertical markers,
/// horizontal reference levels and a crosshair that reads out every series.
export function LineChart({ series, markers = [], references = [], height = 320, formatX, formatY, xLabel, xTicks, marginLeft = margin.left }: Props) {
  const [ref, { width }] = useSize<HTMLDivElement>()
  const [hoverX, setHoverX] = useState<number | null>(null)
  const left = marginLeft

  const layout = useMemo(() => {
    const xs = series.flatMap((s) => s.points.map((p) => p.x))
    const ys = [...series.flatMap((s) =>
      s.points.flatMap((p) => (s.band ? [p.y, p.lo, p.hi] : [p.y])),
    ), ...references.map((r) => r.y)]
    const xDomain = extent(xs)
    const yDomain = extent(ys)
    if (!xDomain || !yDomain || width === 0) return null
    const x = linear(xDomain, [left, width - margin.right])
    const y = linear(pad(yDomain, 0.08), [height - margin.bottom, margin.top])
    const ticks = xTicks
      ? xTicks.filter((t) => t >= xDomain[0] && t <= xDomain[1])
      : niceTicks(...xDomain, Math.max(2, Math.floor(width / 110)))
    const rows = labelRows(markers.map((m) => ({ x: x(m.x), label: m.label })))
    return { x, y, rows, xTicks: ticks, yTicks: niceTicks(...y.domain, 5) }
  }, [series, references, markers, width, height, xTicks, left])

  const area = (points: SeriesPoint[]) => {
    if (!layout) return ""
    const valid = points.filter((p) => p.y != null && Number.isFinite(p.y))
    if (valid.length < 2) return ""
    const bottom = (height - margin.bottom).toFixed(1)
    const line = valid.map((p, i) => `${i ? "L" : "M"}${layout.x(p.x).toFixed(1)},${layout.y(p.y as number).toFixed(1)}`).join("")
    return `${line}L${layout.x(valid[valid.length - 1]!.x).toFixed(1)},${bottom}L${layout.x(valid[0]!.x).toFixed(1)},${bottom}Z`
  }

  const path = (points: SeriesPoint[], pick: (p: SeriesPoint) => number | null | undefined) => {
    if (!layout) return ""
    let d = ""
    let pen = false
    for (const p of points) {
      const v = pick(p)
      if (v == null || !Number.isFinite(v)) {
        pen = false
        continue
      }
      d += `${pen ? "L" : "M"}${layout.x(p.x).toFixed(1)},${layout.y(v).toFixed(1)}`
      pen = true
    }
    return d
  }

  const band = (points: SeriesPoint[]) => {
    if (!layout) return ""
    const valid = points.filter((p) => p.lo != null && p.hi != null && Number.isFinite(p.lo) && Number.isFinite(p.hi))
    if (valid.length < 2) return ""
    const top = valid.map((p, i) => `${i ? "L" : "M"}${layout.x(p.x).toFixed(1)},${layout.y(p.hi as number).toFixed(1)}`)
    const bottom = [...valid].reverse().map((p) => `L${layout.x(p.x).toFixed(1)},${layout.y(p.lo as number).toFixed(1)}`)
    return `${top.join("")}${bottom.join("")}Z`
  }

  const onMove = (event: PointerEvent<SVGSVGElement>) => {
    if (!layout) return
    const rect = event.currentTarget.getBoundingClientRect()
    setHoverX(layout.x.invert(event.clientX - rect.left))
  }

  const readout =
    hoverX == null
      ? []
      : series.map((s) => {
          let best: SeriesPoint | undefined
          for (const p of s.points) {
            if (p.y == null) continue
            if (!best || Math.abs(p.x - hoverX) < Math.abs(best.x - hoverX)) best = p
          }
          return { series: s, point: best }
        })
  const anchor = readout.find((r) => r.point)?.point

  return (
    <div ref={ref} className="relative w-full select-none" style={{ height }}>
      {layout && (
        <svg width={width} height={height} onPointerMove={onMove} onPointerLeave={() => setHoverX(null)} className="block">
          {layout.yTicks.map((t) => (
            <g key={`y${t}`}>
              <line x1={left} x2={width - margin.right} y1={layout.y(t)} y2={layout.y(t)} className="stroke-border" strokeDasharray="2 3" />
              <text x={left - 8} y={layout.y(t)} dy="0.32em" textAnchor="end" className="fill-muted tabular text-[10px]">
                {formatY(t)}
              </text>
            </g>
          ))}
          {references.map((r) => (
            <g key={`ref-${r.label}`} aria-label={`${r.label} ${formatY(r.y)}`}>
              <line x1={left} x2={width - margin.right} y1={layout.y(r.y)} y2={layout.y(r.y)} style={{ stroke: r.color }} strokeDasharray="6 4" strokeWidth={1.3} />
              <text x={width - margin.right - 4} y={layout.y(r.y) - 5} textAnchor="end" style={{ fill: r.color }} className="tabular text-[10px]">
                {r.label} {formatY(r.y)}
              </text>
            </g>
          ))}
          {layout.xTicks.map((t) => (
            <text key={`x${t}`} x={layout.x(t)} y={height - 8} textAnchor="middle" className="fill-muted tabular text-[10px]">
              {formatX(t)}
            </text>
          ))}
          {xLabel && (
            <text x={width - margin.right} y={height - 8} textAnchor="end" className="fill-faint text-[10px]">
              {xLabel}
            </text>
          )}
          {markers.map((m, i) => (
            <g key={`${m.x}-${m.label}-${i}`}>
              <line x1={layout.x(m.x)} x2={layout.x(m.x)} y1={margin.top} y2={height - margin.bottom} style={{ stroke: m.color }} strokeDasharray="4 3" strokeOpacity={0.8} />
              <text x={layout.x(m.x) + 4} y={margin.top + 10 + 12 * layout.rows[i]!} style={{ fill: m.color, stroke: "var(--panel)", strokeWidth: 3, paintOrder: "stroke" }} className="text-[10px]">
                {m.label}
              </text>
            </g>
          ))}
          {series.map((s) =>
            s.band ? <path key={`band-${s.id}`} d={band(s.points)} style={{ fill: s.color }} fillOpacity={0.12} /> : null,
          )}
          {series.map((s) =>
            s.area ? <path key={`area-${s.id}`} d={area(s.points)} style={{ fill: s.color }} fillOpacity={0.1} /> : null,
          )}
          {series.map((s) => s.dots ? (
            <g key={s.id} aria-label={s.label}>
              {s.points.map((p, i) => p.y != null && Number.isFinite(p.y) && Number.isFinite(p.x) ? (
                <circle key={i} cx={layout.x(p.x)} cy={layout.y(p.y)} r={2.2} style={{ fill: s.color }} fillOpacity={0.75} />
              ) : null)}
            </g>
          ) : (
            <path key={s.id} aria-label={s.label} d={path(s.points, (p) => p.y)} fill="none" style={{ stroke: s.color }} strokeWidth={1.6} strokeDasharray={s.dashed ? "5 3" : s.dotted ? "1.5 3" : undefined} strokeLinejoin="round" />
          ))}
          {anchor && hoverX != null && (
            <line x1={layout.x(anchor.x)} x2={layout.x(anchor.x)} y1={margin.top} y2={height - margin.bottom} className="stroke-muted" strokeOpacity={0.5} />
          )}
          {readout.map(({ series: s, point }) =>
            point && point.y != null ? (
              <circle key={`dot-${s.id}`} cx={layout.x(point.x)} cy={layout.y(point.y)} r={3} style={{ fill: s.color }} className="stroke-background" strokeWidth={1.5} />
            ) : null,
          )}
        </svg>
      )}
      {anchor && hoverX != null && layout && (
        <div
          className="pointer-events-none absolute top-2 rounded-md border border-border bg-tooltip px-2 py-1.5 text-[11px] shadow-chart"
          style={{ left: Math.min(layout.x(anchor.x) + 12, width - 170) }}
        >
          <div className="mb-1 text-muted tabular">{formatX(anchor.x)}</div>
          {readout.map(({ series: s, point }) =>
            point && point.y != null ? (
              <div key={s.id} className="flex items-center justify-between gap-3">
                <span className="flex items-center gap-1.5">
                  <span className="inline-block h-1.5 w-1.5 rounded-full" style={{ background: s.color }} />
                  {s.label}
                </span>
                <span className="tabular">{formatY(point.y)}</span>
              </div>
            ) : null,
          )}
        </div>
      )}
    </div>
  )
}
