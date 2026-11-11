import { useLayoutEffect, useMemo, useRef, useState } from "react"

interface Props {
  label?: string
  rows: number[]  // strikes, drawn high to low
  columns: { id: string; label: string }[]
  values: (number | null)[][]  // values[column][row]
  highlightRow?: number | null  // the strike nearest spot
  format: (v: number) => string
  formatRow: (r: number) => string
}

/// Strike x expiry grid, coloured on a diverging scale around zero. Intensity uses a
/// square root so a few huge cells do not wash out everything else.
export function Heatmap({ label = "Exposure by strike and expiry", rows, columns, values, highlightRow, format, formatRow }: Props) {
  const [hover, setHover] = useState<{ row: number; col: number } | null>(null)
  const order = useMemo(() => rows.map((_, i) => i).reverse(), [rows])
  // Saturate at the 98th percentile of |value| so a handful of huge cells (the
  // monthly expiry at the money) do not wash out everything else.
  const scale = useMemo(() => {
    const magnitudes: number[] = []
    for (const column of values) for (const v of column) if (v != null && v !== 0) magnitudes.push(Math.abs(v))
    if (magnitudes.length === 0) return 1
    magnitudes.sort((a, b) => a - b)
    return magnitudes[Math.floor(0.98 * (magnitudes.length - 1))] || 1
  }, [values])

  // Open with the spot row in the middle of the view and follow it as spot crosses
  // strikes, until the trader scrolls the grid: from then on the view stays theirs.
  const scroller = useRef<HTMLDivElement>(null)
  const spotRow = useRef<HTMLTableRowElement>(null)
  const centred = useRef<number | null>(null)
  useLayoutEffect(() => {
    const container = scroller.current
    const row = spotRow.current
    if (!container || !row) return
    if (centred.current != null && Math.abs(container.scrollTop - centred.current) > 1) return
    container.scrollTop = row.offsetTop - container.clientHeight / 2
    centred.current = container.scrollTop
  }, [highlightRow])

  const colour = (v: number | null) => {
    if (v == null || v === 0) return "var(--transparent)"
    const t = Math.sqrt(Math.min(1, Math.abs(v) / scale))
    const token = v > 0 ? "--gex-positive" : "--gex-negative"
    return `color-mix(in srgb, var(${token}) ${(0.08 + 0.85 * t) * 100}%, var(--panel))`
  }

  const hovered = hover ? values[hover.col]?.[hover.row] ?? null : null

  return (
    <div className="relative">
      <div ref={scroller} className="relative overflow-auto rounded-md border border-border" style={{ maxHeight: 520 }}>
        <table tabIndex={0} aria-label={label} className="w-full border-collapse text-[11px] tabular">
          <thead className="sticky top-0 z-10 bg-panel">
            <tr>
              <th scope="col" className="px-2 py-1 text-left font-normal text-muted">Strike</th>
              {columns.map((c) => (
                <th key={c.id} scope="col" className="px-1 py-1 text-center font-normal text-muted whitespace-nowrap">
                  {c.label}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {order.map((row) => (
              <tr
                key={rows[row]}
                ref={rows[row] === highlightRow ? spotRow : undefined}
                className={rows[row] === highlightRow ? "outline outline-1 outline-warn/70 -outline-offset-1" : ""}
              >
                <th scope="row" className="sticky left-0 bg-panel px-2 py-0.5 text-left font-normal text-muted">{formatRow(rows[row] ?? 0)}</th>
                {columns.map((c, col) => {
                  const v = values[col]?.[row] ?? null
                  return (
                    <td
                      key={c.id}
                      aria-label={`${c.id}, strike ${formatRow(rows[row] ?? 0)}: ${v == null ? "no data" : format(v)}`}
                      className="h-5 min-w-12 border-l border-background/60"
                      style={{ background: colour(v) }}
                      onPointerEnter={() => setHover({ row, col })}
                      onPointerLeave={() => setHover(null)}
                    />
                  )
                })}
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      {hover && (
        <div className="pointer-events-none absolute right-3 top-2 rounded-md border border-border bg-tooltip px-2 py-1 text-[11px] shadow-chart tabular">
          {columns[hover.col]?.label} · {formatRow(rows[hover.row] ?? 0)} ·{" "}
          <span className={hovered != null && hovered < 0 ? "text-gex-negative" : "text-gex-positive"}>
            {hovered == null ? "–" : format(hovered)}
          </span>
        </div>
      )}
    </div>
  )
}
