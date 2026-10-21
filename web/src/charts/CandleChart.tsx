import { useEffect, useId, useMemo, useRef, useState, type PointerEvent } from "react"
import type { Candle, CandleInterval } from "../api/types"
import { barTime, clampWindow, priceDomain, sessionBreak, timeTicks, type ChartLevel } from "../lib/candles"
import { linear, niceTicks } from "./scale"
import { useSize } from "./useSize"

/** A price range until a future time, such as the expected move by an expiry. */
export interface PriceBand { lo: number; hi: number; label: string }

interface Props {
  bars: readonly Candle[]
  interval: CandleInterval
  levels?: readonly ChartLevel[]
  band?: PriceBand | null
  height: number
  formatPrice: (x: number) => string
  /** Accessible name, e.g. "SPX 5-minute candles". */
  label: string
  /** Fixed width, for rendering without layout (tests). */
  width?: number
}

const margin = { top: 8, right: 66, bottom: 22, left: 4 }
export const levelColors: Record<ChartLevel["kind"], string> = { long: "var(--chart-4)", short: "var(--chart-2)", trigger: "var(--warn)", alert: "var(--accent)", brief: "var(--chart-5)" }
export const bandColor = "var(--chart-7)"
const minBarWidth = 3
const tagHeight = 16
/** A band joins the price scale while it spans at most this many times the bars' range; beyond that its edges are pinned. */
const bandScale = 3

export interface AxisTag { price: number; y: number; text: string; color: string; filled: boolean }

/** Axis tags moved apart just enough not to overlap, kept inside [top, bottom]. */
export function stackTags(tags: readonly AxisTag[], top: number, bottom: number, gap = tagHeight): AxisTag[] {
  const out = [...tags].sort((a, b) => a.y - b.y).map((tag) => ({ ...tag }))
  for (let i = 1; i < out.length; i++) out[i]!.y = Math.max(out[i]!.y, out[i - 1]!.y + gap)
  if (out.length) out[out.length - 1]!.y = Math.min(out[out.length - 1]!.y, bottom)
  for (let i = out.length - 2; i >= 0; i--) out[i]!.y = Math.min(out[i]!.y, out[i + 1]!.y - gap)
  if (out.length && out[0]!.y < top) {
    const shift = top - out[0]!.y
    for (const tag of out) tag.y += shift
  }
  return out
}

interface Pin { text: string; color: string; above: boolean; rank: number }

/** Levels and band edges beyond the price scale, stacked at the edge they lie past, nearest first. */
function pins(domain: readonly [number, number], levels: readonly ChartLevel[], band: PriceBand | null | undefined, format: (x: number) => string): Pin[] {
  const all = levels.map((level) => ({ price: level.price, text: level.label, color: levelColors[level.kind] }))
  if (band && band.hi > band.lo)
    all.push({ price: band.hi, text: `+1σ ${format(band.hi)}`, color: bandColor }, { price: band.lo, text: `−1σ ${format(band.lo)}`, color: bandColor })
  const side = (above: boolean) => all.filter((pin) => above ? pin.price > domain[1] : pin.price < domain[0])
    .sort((a, b) => above ? a.price - b.price : b.price - a.price)
    .map((pin, rank) => ({ text: pin.text, color: pin.color, above, rank }))
  return [...side(true), ...side(false)]
}

/// Candlesticks with a price axis on the right, horizontal levels (pinned to the
/// nearer edge when out of range) and a crosshair. A band is shaded in a strip
/// after the latest bar, the time it describes, with its edges drawn across.
/// Dragging or a sideways swipe pans; pinching or scrolling with Ctrl or Cmd held
/// zooms, so plain scrolling still moves the page; a double click returns to the
/// latest bars.
export function CandleChart({ bars, interval, levels = [], band, height, formatPrice, label, width: fixedWidth }: Props) {
  const [ref, size] = useSize<HTMLDivElement>()
  const width = fixedWidth ?? size.width
  const plotW = Math.max(0, width - margin.left - margin.right)
  const plotH = Math.max(0, height - margin.top - margin.bottom)
  const bottom = margin.top + plotH
  const right = margin.left + plotW
  const shaded = band != null && band.hi > band.lo
  const strip = shaded ? Math.min(56, Math.round(plotW * 0.08)) : 0
  const barsW = plotW - strip
  const fit = Math.max(20, Math.floor(barsW / 7))
  const [view, setView] = useState<{ span: number | null; offset: number }>({ span: null, offset: 0 })
  const [hover, setHover] = useState<{ index: number; y: number } | null>(null)
  const drag = useRef<{ x: number; offset: number } | null>(null)
  const clip = `candles-${useId().replace(/:/g, "")}`

  const win = clampWindow(bars.length, Math.min(view.span ?? fit, Math.max(10, Math.floor(barsW / minBarWidth))), view.offset)
  const step = barsW / Math.max(1, win.span)
  const visible = useMemo(() => bars.slice(win.start, win.end), [bars, win.start, win.end])
  const domain = useMemo(() => {
    const range = priceDomain(visible)
    if (!range || !band || !(band.hi > band.lo) || band.hi - band.lo > bandScale * (range[1] - range[0])) return range
    const pad = (band.hi - band.lo) * 0.04
    return [Math.min(range[0], band.lo - pad), Math.max(range[1], band.hi + pad)] as [number, number]
  }, [visible, band])
  const y = useMemo(() => domain ? linear(domain, [bottom, margin.top]) : null, [domain, bottom])
  const xAt = (index: number) => margin.left + (index - win.start + 0.5) * step

  // Wheel gestures need a non-passive listener to keep them from the page.
  const latest = useRef({ win, step, total: bars.length })
  latest.current = { win, step, total: bars.length }
  useEffect(() => {
    const element = ref.current
    if (!element) return
    const onWheel = (event: WheelEvent) => {
      const { win: current, step: width, total } = latest.current
      if (Math.abs(event.deltaX) > Math.abs(event.deltaY)) {
        event.preventDefault()
        // Pan from the unrounded offset, kept in range, so small swipes add up.
        setView((previous) => ({ span: current.span,
          offset: Math.max(0, Math.min(total - current.span, previous.offset)) - event.deltaX / Math.max(width, 1e-9) }))
      } else if (event.ctrlKey || event.metaKey) {
        event.preventDefault()
        setView({ span: current.span * (event.deltaY > 0 ? 1.15 : 1 / 1.15), offset: current.offset })
      }
    }
    element.addEventListener("wheel", onWheel, { passive: false })
    return () => element.removeEventListener("wheel", onWheel)
  }, [ref])

  const candles = useMemo(() => {
    if (!y) return null
    const body = Math.max(1, Math.min(step * 0.7, 14))
    return visible.map((bar, k) => {
      const cx = margin.left + (k + 0.5) * step
      const color = bar.c >= bar.o ? "var(--bullish)" : "var(--bearish)"
      const top = y(Math.max(bar.o, bar.c))
      return (
        <g key={bar.t}>
          <line x1={cx} x2={cx} y1={y(bar.h)} y2={y(bar.l)} style={{ stroke: color }} strokeWidth={1} />
          <rect x={cx - body / 2} y={top} width={body} height={Math.max(1, y(Math.min(bar.o, bar.c)) - top)} style={{ fill: color }} />
        </g>
      )
    })
  }, [visible, y, step])

  const ticks = useMemo(() => timeTicks(visible, interval, 70 / Math.max(step, 1e-9)), [visible, interval, step])
  const breaks = useMemo(() => visible.flatMap((bar, k) =>
    k > 0 && sessionBreak(visible[k - 1]!, bar, interval) ? [k] : []), [visible, interval])

  const last = bars[bars.length - 1]
  const focus = hover ? bars[hover.index] : last
  const before = hover ? bars[hover.index - 1] : bars[bars.length - 2]
  const change = focus && before ? focus.c - before.c : null
  const lastColor = last && last.c < last.o ? "var(--bearish)" : "var(--bullish)"

  const inRange = (price: number) => y != null && price >= y.domain[0] && price <= y.domain[1]
  const tags = y ? stackTags([
    ...levels.filter((level) => inRange(level.price))
      .map((level) => ({ price: level.price, y: y(level.price), text: formatPrice(level.price), color: levelColors[level.kind], filled: false })),
    ...(shaded ? [band!.hi, band!.lo].filter(inRange).map((edge) => ({ price: edge, y: y(edge), text: formatPrice(edge), color: bandColor, filled: false })) : []),
    ...(last && inRange(last.c) ? [{ price: last.c, y: y(last.c), text: formatPrice(last.c), color: lastColor, filled: true }] : []),
  ], margin.top, bottom) : []

  const onMove = (event: PointerEvent<SVGSVGElement>) => {
    const rect = event.currentTarget.getBoundingClientRect()
    const px = event.clientX - rect.left
    if (drag.current) {
      setView({ span: win.span, offset: drag.current.offset + (event.clientX - drag.current.x) / step })
      return
    }
    if (px < margin.left || px > margin.left + barsW || !visible.length) return setHover(null)
    const index = Math.min(win.end - 1, Math.max(win.start, win.start + Math.floor((px - margin.left) / step)))
    setHover({ index, y: Math.min(bottom, Math.max(margin.top, event.clientY - rect.top)) })
  }

  const tag = (at: number, text: string, color: string, filled: boolean) => (
    <g transform={`translate(${right},${at})`}>
      <rect x={1} y={-tagHeight / 2} width={margin.right - 2} height={tagHeight} rx={2} style={filled ? { fill: color } : { fill: "var(--panel)", stroke: color }} />
      <text x={6} dy="0.32em" className="tabular text-[10px]" style={{ fill: filled ? "var(--panel)" : color }}>{text}</text>
    </g>
  )

  return (
    <div className="w-full select-none">
      {/* The bar under the cursor, or the latest. */}
      <div className="flex min-h-[18px] flex-wrap gap-x-2 text-[11px] tabular text-muted" aria-live="off">
        {focus && (
          <>
            <span className="text-foreground">{barTime(focus.t, interval)}</span>
            <span>O <span className="text-foreground">{formatPrice(focus.o)}</span></span>
            <span>H <span className="text-foreground">{formatPrice(focus.h)}</span></span>
            <span>L <span className="text-foreground">{formatPrice(focus.l)}</span></span>
            <span>C <span className="text-foreground">{formatPrice(focus.c)}</span></span>
            {change != null && before && (
              <span className={change >= 0 ? "text-bullish" : "text-bearish"}>
                {change >= 0 ? "+" : "−"}{formatPrice(Math.abs(change))} ({change >= 0 ? "+" : "−"}{Math.abs(change / before.c * 100).toFixed(2)}%)
              </span>
            )}
          </>
        )}
      </div>
      <div ref={ref} className="relative w-full" style={{ height }}>
        {width > 0 && (
          <svg width={width} height={height} role="img" aria-label={label} className="block"
            style={{ touchAction: "pan-y", cursor: drag.current ? "grabbing" : "crosshair" }}
            onPointerMove={onMove}
            onPointerLeave={() => { if (!drag.current) setHover(null) }}
            onPointerDown={(event) => {
              if (event.button !== 0) return
              event.currentTarget.setPointerCapture(event.pointerId)
              drag.current = { x: event.clientX, offset: win.offset }
            }}
            onPointerUp={() => { drag.current = null }}
            onPointerCancel={() => { drag.current = null }}
            onDoubleClick={() => setView({ span: null, offset: 0 })}>
            <defs>
              <clipPath id={clip}><rect x={margin.left} y={margin.top} width={plotW} height={plotH} /></clipPath>
            </defs>
            {y && niceTicks(...y.domain, Math.max(3, Math.floor(plotH / 44))).map((t) => (
              <g key={`y${t}`}>
                <line x1={margin.left} x2={right} y1={y(t)} y2={y(t)} className="stroke-border" strokeDasharray="2 3" />
                {/* Tags carry their own price; a tick label beneath one would be unreadable. */}
                {!tags.some((tag) => Math.abs(tag.y - y(t)) < tagHeight) && (
                  <text x={right + 6} y={y(t)} dy="0.32em" className="fill-muted tabular text-[10px]">{formatPrice(t)}</text>
                )}
              </g>
            ))}
            {breaks.map((k) => (
              <line key={`break${k}`} x1={margin.left + k * step} x2={margin.left + k * step} y1={margin.top} y2={bottom} className="stroke-border" />
            ))}
            {ticks.map((t) => (
              <text key={`x${t.index}`} x={margin.left + (t.index + 0.5) * step} y={height - 6} textAnchor="middle"
                className={`tabular text-[10px] ${t.major ? "fill-foreground" : "fill-muted"}`}>{t.label}</text>
            ))}
            <line x1={right} x2={right} y1={margin.top} y2={bottom} className="stroke-border" />
            <g clipPath={`url(#${clip})`}>
              {y && shaded && (band!.hi <= y.domain[1] || band!.lo >= y.domain[0]) && (
                <g aria-label={band!.label}>
                  <rect x={margin.left + barsW} width={strip} y={y(band!.hi)} height={Math.max(0, y(band!.lo) - y(band!.hi))} style={{ fill: bandColor }} fillOpacity={0.14} />
                  {[band!.hi, band!.lo].map((edge) => (
                    <line key={edge} x1={margin.left} x2={right} y1={y(edge)} y2={y(edge)} style={{ stroke: bandColor }} strokeOpacity={0.7} strokeDasharray="3 4" />
                  ))}
                  {band!.hi <= y.domain[1] && (
                    <text x={right - 4} y={y(band!.hi) < margin.top + 14 ? y(band!.hi) + 12 : y(band!.hi) - 4} textAnchor="end" style={{ fill: bandColor }} className="text-[10px]">{band!.label}</text>
                  )}
                </g>
              )}
              {candles}
              {y && levels.map((level) => inRange(level.price) ? (
                <g key={`${level.kind}${level.price}${level.label}`} aria-label={level.label}>
                  <line x1={margin.left} x2={right} y1={y(level.price)} y2={y(level.price)} style={{ stroke: levelColors[level.kind] }} strokeDasharray="6 4" strokeWidth={1.2} />
                  <text x={margin.left + 4} y={y(level.price) - 4} style={{ fill: levelColors[level.kind] }} className="text-[10px]">{level.label}</text>
                </g>
              ) : null)}
              {y && last && (
                <line x1={margin.left} x2={right} y1={y(last.c)} y2={y(last.c)} style={{ stroke: lastColor }} strokeOpacity={0.7} strokeDasharray="1 2" />
              )}
            </g>
            {tags.map((t) => <g key={`tag${t.price}${t.color}${t.filled}`}>{tag(t.y, t.text, t.color, t.filled)}</g>)}
            {y && pins(y.domain, levels, band, formatPrice).map((pin) => (
              <text key={`${pin.above ? "up" : "down"}${pin.rank}`} x={margin.left + barsW - 4} textAnchor="end" style={{ fill: pin.color }} className="text-[10px]" aria-label={pin.text}
                y={pin.above ? margin.top + 10 + pin.rank * 12 : bottom - 4 - pin.rank * 12}>{pin.above ? "↑" : "↓"} {pin.text}</text>
            ))}
            {y && hover && (
              <g className="pointer-events-none">
                <line x1={xAt(hover.index)} x2={xAt(hover.index)} y1={margin.top} y2={bottom} className="stroke-muted" strokeOpacity={0.6} strokeDasharray="3 3" />
                <line x1={margin.left} x2={right} y1={hover.y} y2={hover.y} className="stroke-muted" strokeOpacity={0.6} strokeDasharray="3 3" />
                {tag(hover.y, formatPrice(y.invert(hover.y)), "var(--muted)", true)}
              </g>
            )}
          </svg>
        )}
      </div>
    </div>
  )
}
