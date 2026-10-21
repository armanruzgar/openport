import { useSyncExternalStore } from "react"
import type { ChainRow, StrategyCandidate } from "../api/types"
import type { StrategyLeg } from "./strategy"

/**
 * A strategy drawn on the Analyze tab: legs as the ticket holds them, units of
 * them, and shares bought with each unit (a covered call's hundred).
 */
export interface AnalyzeCandidate { id: string; name: string; legs: StrategyLeg[]; units: number; shares: number; visible: boolean }
export interface AnalyzeState { candidates: AnalyzeCandidate[]; /** The candidate whose strikes the chart can drag. */ selected: string | null }
export const maxAnalyzeCandidates = 4

// One list per underlying, kept in this browser only: a convenience, not account state.
const empty: AnalyzeState = { candidates: [], selected: null }
const states = new Map<string, AnalyzeState>()
const listeners = new Set<() => void>()
const storageKey = (symbol: string) => `openport.analyze.${symbol}`
function load(symbol: string): AnalyzeState {
  const saved = states.get(symbol)
  if (saved) return saved
  let state = empty
  try {
    const text = window.localStorage.getItem(storageKey(symbol))
    const parsed = text ? JSON.parse(text) as AnalyzeState : null
    if (parsed && Array.isArray(parsed.candidates)) state = { candidates: parsed.candidates.slice(0, maxAnalyzeCandidates), selected: parsed.selected ?? null }
  } catch { /* storage unavailable: start empty */ }
  states.set(symbol, state)
  return state
}
function save(symbol: string, state: AnalyzeState) {
  states.set(symbol, state)
  try { window.localStorage.setItem(storageKey(symbol), JSON.stringify(state)) } catch { /* kept in memory only */ }
  listeners.forEach((listener) => listener())
}
function subscribe(listener: () => void) {
  listeners.add(listener)
  return () => { listeners.delete(listener) }
}
let counter = 0
const nextId = () => `${Date.now().toString(36)}-${(counter++).toString(36)}`

/** Quotes go stale, so a saved leg keeps its contract and drops its quote. */
const unquoted = (legs: StrategyLeg[]) => legs.map((leg) => ({ ...leg, quote: null }))
export function updateAnalyze(symbol: string, change: (state: AnalyzeState) => AnalyzeState) {
  const next = change(load(symbol))
  const candidates = next.candidates.slice(0, maxAnalyzeCandidates).map((c) => ({ ...c, legs: unquoted(c.legs) }))
  save(symbol, { candidates, selected: candidates.some((c) => c.id === next.selected) ? next.selected : candidates[0]?.id ?? null })
}
/** Adds a candidate and selects it; false when the tab already draws four. */
export function addAnalyzeCandidate(symbol: string, candidate: Omit<AnalyzeCandidate, "id" | "visible">): boolean {
  const state = load(symbol)
  if (state.candidates.length >= maxAnalyzeCandidates) return false
  const id = nextId()
  updateAnalyze(symbol, (s) => ({ candidates: [...s.candidates, { ...candidate, id, visible: true }], selected: id }))
  return true
}
export function useAnalyze(symbol: string): AnalyzeState {
  return useSyncExternalStore(subscribe, () => load(symbol), () => empty)
}

/** The legs as GET /api/risk/profile's legs: signed contracts by padded OSI symbol. */
export function profileLegs(legs: readonly StrategyLeg[], units: number): Record<string, number> {
  const result: Record<string, number> = {}
  for (const leg of legs) result[leg.symbol] = (result[leg.symbol] ?? 0) + (leg.side === "buy" ? 1 : -1) * leg.ratio * units
  return result
}

/**
 * What entering costs beyond the legs' mids, in dollars for all units: zero at
 * mid, buying at asks and selling at bids at the natural price. The profile's
 * curves start from the contracts' values now, so this is the offset to the
 * trade's own P&L. Null while a leg has no two-sided quote.
 */
export function entryCost(legs: readonly StrategyLeg[], units: number, basis: "mid" | "natural"): number | null {
  if (basis === "mid") return 0
  let total = 0
  for (const leg of legs) {
    const q = leg.quote
    if (!q || q.bid == null || q.ask == null || q.mid == null) return null
    total += leg.ratio * (leg.side === "buy" ? q.ask - q.mid : q.mid - q.bid)
  }
  return total * 100 * units
}

/** A curve's value at a price, linear between its points; null outside them or next to a missing one. */
export function valueAt(prices: readonly number[], pnl: readonly (number | null)[], price: number): number | null {
  if (!prices.length || !(price >= prices[0]!) || !(price <= prices.at(-1)!)) return null
  for (let i = 1; i < prices.length; i++) {
    const [x0, x1] = [prices[i - 1]!, prices[i]!]
    if (price > x1) continue
    const [y0, y1] = [pnl[i - 1], pnl[i]]
    if (y0 == null || y1 == null) return null
    return x1 === x0 ? y0 : y0 + (y1 - y0) * (price - x0) / (x1 - x0)
  }
  return pnl[0] ?? null
}

/** Where a curve crosses zero, linearly between its points. */
export function zeroCrossings(prices: readonly number[], pnl: readonly (number | null)[]): number[] {
  const result: number[] = []
  for (let i = 1; i < prices.length; i++) {
    const [a, b] = [pnl[i - 1], pnl[i]]
    if (a == null || b == null) continue
    if (a === 0 && i === 1) result.push(prices[0]!)
    if (b === 0) result.push(prices[i]!)
    else if ((a < 0 && b > 0) || (a > 0 && b < 0)) result.push(prices[i - 1]! + (prices[i]! - prices[i - 1]!) * (-a / (b - a)))
  }
  return [...new Set(result.map((x) => Math.round(x * 100) / 100))]
}

const tradable = (row: ChainRow, type: StrategyLeg["type"]) => row[type]?.tradable && row[type]?.symbol ? row[type] : null
/** The leg on another row of its chain: that strike's contract and quote. */
export function moveLeg(leg: StrategyLeg, row: ChainRow): StrategyLeg {
  const quote = tradable(row, leg.type)!
  return { ...leg, symbol: quote.symbol!, strike: row.strike, quote }
}
/** The next listed strike with a tradable contract of the leg's type, up (+1) or down (-1); null at the edge. */
export function stepStrike(rows: readonly ChainRow[], leg: StrategyLeg, step: 1 | -1): ChainRow | null {
  const listed = rows.filter((row) => tradable(row, leg.type)).sort((a, b) => a.strike - b.strike)
  if (step > 0) return listed.find((row) => row.strike > leg.strike) ?? null
  return listed.filter((row) => row.strike < leg.strike).at(-1) ?? null
}
/** The tradable strike of `type` nearest `price`, as a dragged strike snaps to. */
export function nearestRow(rows: readonly ChainRow[], type: StrategyLeg["type"], price: number): ChainRow | null {
  let best: ChainRow | null = null
  for (const row of rows) if (tradable(row, type) && (!best || Math.abs(row.strike - price) < Math.abs(best.strike - price))) best = row
  return best
}

/** One row of the price slices: a percent move, an entered price, a standard deviation or a breakeven. */
export interface SliceRow { key: string; label: string; kind: "percent" | "price" | "sigma" | "breakeven"; price: number }
export function sliceRows(spot: number, { percents = [], prices = [], sigma = null, breakevens = [] }: {
  percents?: readonly number[]; prices?: readonly number[]; sigma?: number | null; breakevens?: readonly { label: string; price: number }[]
}): SliceRow[] {
  const rows: SliceRow[] = [
    ...percents.map((p) => ({ key: `p${p}`, label: `${p > 0 ? "+" : ""}${p}%`, kind: "percent" as const, price: spot * (1 + p / 100) })),
    ...prices.map((p) => ({ key: `$${p}`, label: `$${p}`, kind: "price" as const, price: p })),
    ...(sigma != null && sigma > 0 ? [-2, -1, 1, 2].map((k) => ({ key: `s${k}`, label: `${k > 0 ? "+" : "−"}${Math.abs(k)}σ`, kind: "sigma" as const, price: spot * Math.exp(k * sigma) })) : []),
    ...breakevens.map((b, i) => ({ key: `b${i}`, label: b.label, kind: "breakeven" as const, price: b.price })),
  ]
  return rows.filter((row) => Number.isFinite(row.price) && row.price > 0).sort((a, b) => a.price - b.price)
}

/** An optimizer candidate's legs as the ticket holds them; quotes follow from the chains. */
export function candidateLegs(candidate: StrategyCandidate, underlying: string): StrategyLeg[] {
  return candidate.legs.map((leg) => ({ symbol: leg.symbol, underlying, side: leg.side, ratio: leg.ratio, type: leg.type,
    strike: leg.strike, expiry: leg.expiry, quote: null }))
}

/** Calendar days from `now` to an instant, never negative. */
export function daysUntil(time: string, now: number): number {
  return Math.max(0, (Date.parse(time) - now) / 86_400_000)
}
