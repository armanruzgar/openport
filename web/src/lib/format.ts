// Number formatting for the terminal. Missing values render as a dash, never 0.

type Value = number | null | undefined

const dash = "—"

const compact = new Intl.NumberFormat("en-US", { notation: "compact", maximumFractionDigits: 1 })
const compactMoney = new Intl.NumberFormat("en-US", {
  style: "currency",
  currency: "USD",
  notation: "compact",
  maximumFractionDigits: 2,
})

export const isNum = (x: Value): x is number => x != null && Number.isFinite(x)

export function fixed(x: Value, digits = 2): string {
  return isNum(x) ? x.toFixed(digits) : dash
}

/// Prices: two decimals, or more for sub-dollar quotes where a tick is a cent or less.
export function price(x: Value): string {
  if (!isNum(x)) return dash
  return Math.abs(x) < 1 && x !== 0 ? x.toFixed(3) : x.toFixed(2)
}

export function pct(x: Value, digits = 1): string {
  return isNum(x) ? `${(x * 100).toFixed(digits)}%` : dash
}

/// Implied volatility as vol points: 0.1575 -> "15.75".
export function vol(x: Value, digits = 2): string {
  return isNum(x) ? (x * 100).toFixed(digits) : dash
}

/// Compact dollars with the typographic minus signed money uses: -2.65e9 -> "−$2.65B".
export function money(x: Value): string {
  return isNum(x) ? compactMoney.format(x).replace("-", "−") : dash
}

export function count(x: Value): string {
  if (!isNum(x)) return dash
  return Math.abs(x) >= 10_000 ? compact.format(x) : x.toLocaleString("en-US")
}

export function signed(x: Value, formatter: (v: number) => string): string {
  if (!isNum(x)) return dash
  return `${x > 0 ? "+" : ""}${formatter(x)}`
}

export function days(x: Value): string {
  if (!isNum(x)) return dash
  if (x < 1) return `${Math.max(0, x * 24).toFixed(1)}h`
  return `${x < 10 ? x.toFixed(1) : Math.round(x)}d`
}

export function clock(iso: string | null | undefined): string {
  if (!iso) return dash
  const date = new Date(iso)
  return date.toLocaleTimeString("en-US", { hour12: false, timeZone: "America/New_York" }) + " ET"
}

/// "2026-10-05PM" -> "Oct 5" plus the settlement when it matters.
export function expiryLabel(id: string, withSettlement = false): string {
  const date = new Date(`${id.slice(0, 10)}T12:00:00Z`)
  const label = date.toLocaleDateString("en-US", { month: "short", day: "numeric", timeZone: "UTC" })
  return withSettlement ? `${label} ${id.slice(10)}` : label
}

/// Signed percentage with a typographic minus, matching money: 0.0112 -> "+1.1%", -0.011 -> "−1.1%".
export function signedPercent(x: Value, digits = 1): string {
  if (!isNum(x)) return dash
  const text = Math.abs(x * 100).toFixed(digits)
  return Number(text) === 0 ? `${text}%` : `${x > 0 ? "+" : "−"}${text}%`
}

/** ["SPX", "SPY", "QQQ"] -> "SPX, SPY and QQQ" */
export function joinList(items: readonly string[]): string {
  return items.length < 3 ? items.join(" and ") : `${items.slice(0, -1).join(", ")} and ${items.at(-1)}`
}
