export interface LinearScale {
  (x: number): number
  invert(y: number): number
  domain: [number, number]
  range: [number, number]
}

export function linear(domain: [number, number], range: [number, number]): LinearScale {
  const [d0, d1] = domain
  const [r0, r1] = range
  const span = d1 - d0 || 1
  const scale = ((x: number) => r0 + ((x - d0) / span) * (r1 - r0)) as LinearScale
  scale.invert = (y: number) => d0 + ((y - r0) / (r1 - r0 || 1)) * span
  scale.domain = domain
  scale.range = range
  return scale
}

/// Round tick positions (1, 2, 2.5 or 5 times a power of ten) covering [min, max].
export function niceTicks(min: number, max: number, count = 5): number[] {
  if (!Number.isFinite(min) || !Number.isFinite(max)) return []
  if (min === max) return [min]
  const raw = (max - min) / Math.max(1, count)
  const power = 10 ** Math.floor(Math.log10(raw))
  const step = ([1, 2, 2.5, 5, 10].map((m) => m * power).find((s) => s >= raw) ?? 10 * power)
  const start = Math.ceil(min / step) * step
  // Count the ticks instead of adding steps until past max: across a span of a few
  // ulps a step is below the doubles' spacing, so adding it never moves the tick.
  const last = Math.floor((max - start) / step + 1e-9)
  if (!(last >= 0)) return min < max ? [min] : []
  const ticks: number[] = []
  for (let i = 0; i <= Math.min(last, 1000); i++) {
    const tick = Number((start + i * step).toPrecision(12))
    if (tick !== ticks.at(-1)) ticks.push(tick)
  }
  return ticks
}

export function extent(values: Iterable<number | null | undefined>): [number, number] | null {
  let lo = Infinity
  let hi = -Infinity
  for (const v of values) {
    if (v == null || !Number.isFinite(v)) continue
    if (v < lo) lo = v
    if (v > hi) hi = v
  }
  return lo <= hi ? [lo, hi] : null
}

export function pad([lo, hi]: [number, number], fraction = 0.05): [number, number] {
  const span = hi - lo || Math.abs(hi) || 1
  return [lo - span * fraction, hi + span * fraction]
}

/**
 * A row for each marker's label, left to right, so a label that would run into
 * the one before it drops to the next row: labels start 4px right of their
 * marker and take about `charWidth` a character.
 */
export function labelRows(markers: { x: number; label: string }[], charWidth = 6): number[] {
  const rows = markers.map(() => 0)
  const ends: number[] = []  // where each row's last label ends
  for (const i of markers.map((_, i) => i).sort((a, b) => markers[a]!.x - markers[b]!.x)) {
    const start = markers[i]!.x + 4
    let row = 0
    while (row < ends.length && start < ends[row]! + 6) row++
    ends[row] = start + markers[i]!.label.length * charWidth
    rows[i] = row
  }
  return rows
}
