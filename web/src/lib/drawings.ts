export interface DrawingPoint { time: number; price: number }
export type Drawing = { id: string; kind: "horizontal"; price: number } | { id: string; kind: "trend"; start: DrawingPoint; end: DrawingPoint }
const key = (symbol: string) => `openport.drawings.${symbol.toUpperCase()}`
const point = (p: DrawingPoint | null | undefined) => p && Number.isFinite(p.time) && p.time > 0 && Number.isFinite(p.price) && p.price > 0
export function loadDrawings(symbol: string): Drawing[] {
  try {
    const saved: unknown = JSON.parse(localStorage.getItem(key(symbol)) ?? "[]")
    return Array.isArray(saved) ? saved.filter((d): d is Drawing => d && typeof d.id === "string" &&
      (d.kind === "horizontal" ? Number.isFinite(d.price) && d.price > 0 : d.kind === "trend" && point(d.start) && point(d.end) && d.start.time !== d.end.time)).slice(-100) : []
  } catch { return [] }
}
export function saveDrawings(symbol: string, drawings: Drawing[]) {
  try { localStorage.setItem(key(symbol), JSON.stringify(drawings.slice(-100))) } catch { /* Keep the in-memory drawings. */ }
}
/** Interpolate in bar space, preserving actual market timestamps across intervals. */
export function drawingIndex(times: readonly number[], time: number): number | null {
  if (!times.length || time < times[0]! || time > times[times.length - 1]!) return null
  const right = times.findIndex((t) => t >= time)
  if (times[right] === time || right === 0) return right
  return right - 1 + (time - times[right - 1]!) / (times[right]! - times[right - 1]!)
}
