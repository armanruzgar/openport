import type { ReplayDemo, ReplayHistory } from "../api/types"
import { newYorkDate, type Bucket, type ScenarioMark } from "./journal"

export function scenarioMarkers(events: ScenarioMark[], from: number, to: number, unit = 1, symbol?: string) {
  return events.filter(e => !symbol || !e.symbols?.length || e.symbols.includes(symbol)).flatMap(e => [
    { x: Date.parse(e.time), label: `Scenario ${e.type}`, color: "var(--warn)" },
    ...(e.end ? [{ x: Date.parse(e.end), label: `${e.type} ends`, color: "var(--warn)" }] : []),
  ]).filter(e => e.x >= from && e.x <= to).map(e => ({ ...e, x: e.x / unit }))
}
/** Sidecar scores are editable: a best is verified only once a verification recomputed it. */
export function personalBestLabel(run: Pick<ReplayHistory, "personal_best" | "personal_best_verified">) {
  if (run.personal_best == null) return ""
  return ` · personal best ${run.personal_best.toFixed(0)}/100 (${run.personal_best_verified ? "verified" : "unverified"})`
}
export function freshPracticeSeed() {
  const values = crypto.getRandomValues(new Uint32Array(2))
  return ((BigInt(values[0]!) << 32n) | BigInt(values[1]!)).toString()
}
const dayNumber = (date: string) => Math.floor(Date.parse(`${date}T12:00:00Z`) / 86_400_000)
const practiceDays = (history: ReplayHistory[]) => [...new Set(history.filter(r => r.drill?.complete_run && r.drill.score != null && r.completed_at)
  .map(r => newYorkDate(r.completed_at!)?.date).filter((d): d is string => !!d))].sort().reverse()
/** Real completion dates, never a scenario's simulated date. Yesterday keeps a streak alive. */
export function practiceStreak(history: ReplayHistory[], today: string) {
  const days = practiceDays(history).map(dayNumber)
  let expected = dayNumber(today)
  if (!days.includes(expected)) expected--
  let count = 0
  while (days.includes(expected--)) count++
  return count
}
export function practiceQueue(demos: ReplayDemo[], history: ReplayHistory[], today: string, seed = freshPracticeSeed, edges: readonly Bucket[] = []) {
  return demos.flatMap(demo => {
    if (!demo.id || !demo.objectives?.length) return []
    const prior = history.filter(r => r.scenario === demo.id && r.drill?.complete_run && r.drill.score != null && r.completed_at)
      .sort((a, b) => b.completed_at!.localeCompare(a.completed_at!))[0]
    const score = prior?.drill?.score ?? null
    const weak = edges.filter(b => b.trades >= 3 && b.expectancy != null && b.expectancy < 0 && demo.symbols?.includes(b.label)).map(b => b.label)
    const interval = weak.length || score == null || score < 60 ? 1 : score < 80 ? 3 : 7
    const elapsed = prior ? dayNumber(today) - dayNumber(newYorkDate(prior.completed_at!)!.date) : Infinity
    if (elapsed < interval) return []
    let nextSeed = seed()
    // A deterministic test generator may repeat; the neighbour is still a new seed.
    if (history.some(r => r.scenario === demo.id && r.seed === nextSeed)) {
      const used = new Set(history.filter(r => r.scenario === demo.id).map(r => r.seed))
      while (used.has(nextSeed)) nextSeed = ((BigInt(nextSeed) + 1n) & ((1n << 64n) - 1n)).toString()
    }
    return [{ scenario: demo.id, title: demo.title ?? demo.id, seed: nextSeed, plan: prior?.plan ?? "practice", score, interval, weak }]
  }).sort((a, b) => ((a.score ?? 50) - (a.weak.length ? 25 : 0)) - ((b.score ?? 50) - (b.weak.length ? 25 : 0)) || a.scenario.localeCompare(b.scenario)).slice(0, 5)
}
