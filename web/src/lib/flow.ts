import type { Flow, FlowPrint } from "../api/types"

/** The tape's words for where a print traded. */
export const sideLabels: Record<FlowPrint["side"], string> = {
  ask: "at ask", bid: "at bid", mid: "mid", outside: "outside", unquoted: "no quote",
}

/** Sweep, block and multi-leg markers, in that order. */
export function flowTags(print: FlowPrint): string[] {
  const tags: string[] = []
  if (print.sweep) tags.push("sweep")
  if (print.block) tags.push("block")
  if (print.group != null) tags.push("multi-leg")
  return tags
}

/** Where the print sat against the quote: "outside" says above the ask or below the bid. */
export function sideText(print: FlowPrint): string {
  if (print.side !== "outside") return sideLabels[print.side]
  return print.aggressor === "buy" ? "above ask" : "below bid"
}

/** Net premium by strike for one expiry, the largest first, at most `limit` rows. */
export function strikeBars(flow: Flow, expiry: string | null, limit = 12) {
  return flow.strikes
    .filter((row) => expiry == null || row.expiry === expiry)
    .sort((a, b) => Math.abs(b.net) - Math.abs(a.net))
    .slice(0, limit)
    .map((row) => ({ label: `${row.strike}${expiry == null ? ` ${row.expiry.slice(5, 10)}` : ""}`, value: row.net }))
}

/** What to say when there is nothing on the tape, so an empty view never looks like a quiet market. */
export function emptyTapeText(flow: Flow): string {
  if (flow.retained > 0) return "No prints match these filters."
  if (flow.message) return flow.message
  return `No option trades for ${flow.symbol} yet.`
}
