/** Fixed-height rows with overscan; spacer rows preserve native table layout. */
export function rowWindow(count: number, scrollTop: number, height: number, rowHeight: number, overscan = 5) {
  const total = Math.max(0, Math.floor(count))
  const size = Math.max(1, rowHeight)
  const start = Math.max(0, Math.min(total, Math.floor(Math.max(0, scrollTop) / size) - overscan))
  const end = Math.min(total, Math.max(start, Math.ceil((Math.max(0, scrollTop) + Math.max(0, height)) / size) + overscan))
  return { start, end, before: start * size, after: (total - end) * size }
}
