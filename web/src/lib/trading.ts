import type { OptionQuote, TradingSession, UnderlyingSnapshot } from "../api/types"
import type { Money, Side } from "../api/trading-types"

function decimal(value: string | null | undefined) {
  if (value == null || !/^-?\d+(\.\d+)?$/.test(value)) return null
  const negative = value.startsWith("-")
  const [whole = "0", fraction = ""] = value.replace(/^-/, "").split(".")
  return { units: BigInt(whole + fraction) * (negative ? -1n : 1n), scale: fraction.length }
}
function decimalString(units: bigint, scale: number) {
  const negative = units < 0n
  const digits = (negative ? -units : units).toString().padStart(scale + 1, "0")
  return `${negative ? "-" : ""}${scale ? `${digits.slice(0, -scale)}.${digits.slice(-scale)}` : digits}`
}
/** Exact integer multiplication; no binary floating point accounting. */
export function multiplyMoney(value: Money | null | undefined, multiplier: number): Money | null {
  const parsed = decimal(value)
  return parsed && Number.isSafeInteger(multiplier) ? decimalString(parsed.units * BigInt(multiplier), parsed.scale) : null
}
export function validMoney(value: string) {
  const parsed = decimal(value)
  return parsed != null && parsed.units >= 0n
}
/** Round only for display, half away from zero, without losing large integer cents. */
export function formatMoney(value: Money | null | undefined, digits = 2): string {
  const parsed = decimal(value)
  if (!parsed) return "—"
  const absolute = parsed.units < 0n ? -parsed.units : parsed.units
  const factor = 10n ** BigInt(Math.abs(parsed.scale - digits))
  const rounded = parsed.scale > digits ? (absolute + factor / 2n) / factor : absolute * factor
  const [whole = "0", fraction] = decimalString(rounded, digits).split(".")
  return `${parsed.units < 0n && rounded !== 0n ? "−" : ""}$${whole.replace(/\B(?=(\d{3})+(?!\d))/g, ",")}${fraction ? `.${fraction}` : ""}`
}
/** Rule amounts retain cents, while whole-dollar presets stay compact. */
export function ruleMoney(value: Money | null | undefined): string {
  const parsed = decimal(value)
  return formatMoney(value, parsed && parsed.units % (10n ** BigInt(parsed.scale)) === 0n ? 0 : 2)
}
/** P&L style: "+$267.50", "−$10.65", "$0.00". */
export function signedMoney(value: Money | null | undefined, digits = 2): string {
  const text = formatMoney(value, digits)
  const parsed = decimal(value)
  return parsed && parsed.units > 0n && !/^\$0(\.0+)?$/.test(text) ? `+${text}` : text
}
/** Display-only ratio of two decimal strings, null when undefined. */
export function ratio(numerator: Money | null | undefined, denominator: Money | null | undefined): number | null {
  const a = numerator == null ? NaN : Number(numerator)
  const b = denominator == null ? NaN : Number(denominator)
  return Number.isFinite(a) && Number.isFinite(b) && b !== 0 ? a / b : null
}
/** Exact difference of two decimal strings, for display. */
export function subtractMoney(a: Money | null | undefined, b: Money | null | undefined): Money | null {
  const x = decimal(a), y = decimal(b)
  if (!x || !y) return null
  const scale = Math.max(x.scale, y.scale)
  return decimalString(x.units * 10n ** BigInt(scale - x.scale) - y.units * 10n ** BigInt(scale - y.scale), scale)
}
/** Exact sign of a - b: -1, 0 or 1; null when either is not a decimal. */
export function compareMoney(a: Money | null | undefined, b: Money | null | undefined): -1 | 0 | 1 | null {
  const difference = decimal(subtractMoney(a, b))
  return difference == null ? null : difference.units < 0n ? -1 : difference.units > 0n ? 1 : 0
}
/** Exact whole-number percentage of an amount, e.g. a payout split. */
export function percentOfMoney(value: Money | null | undefined, percent: number): Money | null {
  const parsed = decimal(value)
  return parsed && Number.isSafeInteger(percent) ? decimalString(parsed.units * BigInt(percent), parsed.scale + 2) : null
}
/** Exact sum of decimal strings; invalid values are skipped. */
export function sumMoney(values: Money[]): Money {
  let units = 0n, scale = 2
  for (const value of values) {
    const parsed = decimal(value)
    if (!parsed) continue
    if (parsed.scale > scale) { units *= 10n ** BigInt(parsed.scale - scale); scale = parsed.scale }
    units += parsed.units * 10n ** BigInt(scale - parsed.scale)
  }
  return decimalString(units, scale)
}
export const sideFromCell = (cell: "bid" | "ask"): Side => cell === "bid" ? "sell" : "buy"

/** Preserve typed off-tick precision for server validation; pad cents for display. */
export function limitPriceText(value: Money): Money {
  const parsed = decimal(value)
  if (!parsed || parsed.units < 0n) return value
  while (parsed.scale > 2 && parsed.units % 10n === 0n) { parsed.units /= 10n; parsed.scale-- }
  return decimalString(parsed.units * 10n ** BigInt(Math.max(0, 2 - parsed.scale)), Math.max(2, parsed.scale))
}
const indexRoots = ["SPX", "SPXW", "NDX", "NDXP", "RUT", "RUTW", "OEX", "XEO", "XSP", "MRUT", "XND", "DJX", "VIX", "VIXW"]
/** Mirrors the server's tick policy v2. */
function tickCents(root: string, below: boolean): bigint {
  if (["SPX", "SPXW", "NDX", "NDXP", "RUT", "RUTW", "OEX"].includes(root)) return below ? 5n : 10n
  if (["XSP", "MRUT"].includes(root)) return below ? 1n : 5n
  // Equity and ETF classes: SPY, QQQ and IWM in pennies, the rest on penny-pilot tiers.
  if (!indexRoots.includes(root) && !["SPY", "QQQ", "IWM"].includes(root)) return below ? 1n : 5n
  return 1n
}
/** A multi-leg net price's tick in cents: the smallest leg's lower-tier tick. */
export function comboTickCents(roots: string[]): number {
  return roots.length ? Math.min(...roots.map((root) => Number(tickCents(root, true)))) : 1
}
/** Nearest valid limit on the root's tier tick, for suggested prices. */
export function roundToTick(root: string, value: number): Money | null {
  if (!Number.isFinite(value) || value <= 0) return null
  const cents = Number(tickCents(root, value < 3))
  const rounded = Math.max(cents, Math.round((value * 100) / cents) * cents)
  return (rounded / 100).toFixed(2)
}
export function limitPriceTick(root: string, value: Money): Money {
  const parsed = decimal(value)
  return decimalString(tickCents(root, !!parsed && parsed.units < 3n * 10n ** BigInt(parsed.scale)), 2)
}
/** Next valid tick in either direction, including the tier boundary and off-tick input. */
export function stepLimitPrice(root: string, value: Money, direction: 1 | -1): Money {
  const parsed = decimal(value)
  if (!parsed || parsed.units < 0n) return decimalString(tickCents(root, true), 2)
  const scale = Math.max(2, parsed.scale)
  const units = parsed.units * 10n ** BigInt(scale - parsed.scale)
  const threshold = 3n * 10n ** BigInt(scale)
  const below = units < threshold || (units === threshold && direction === -1)
  const cents = tickCents(root, below)
  const tick = cents * 10n ** BigInt(scale - 2)
  const steps = direction === 1 ? units / tick + 1n : (units + tick - 1n) / tick - 1n
  return decimalString((steps > 0n ? steps : 1n) * cents, 2)
}

/** Index options settle in cash; equity and ETF options deliver shares. The server's index roots. */
const indexUnderlyings = new Set(["SPX", "XSP", "NDX", "XND", "RUT", "MRUT", "VIX", "DJX", "OEX"])
export function deliversShares(underlying: string): boolean {
  return !indexUnderlyings.has(underlying)
}

/** For servers without `paper`: orders wait while the product's sessions are closed. */
export function paperSessionNotice(symbol: string, session: TradingSession | null | undefined) {
  if (!session || session.open) return null
  return `${symbol} options are not trading now (${session.note}); paper orders wait for the next session.`
}

/**
 * The overnight (global) and curb sessions take limit orders only: no market
 * orders, and a trigger or bracket only on a GTC limit, which waits for the
 * regular session. Uses the session of the market-data clock, which a delayed
 * feed keeps behind the wall clock, when the server gives it.
 */
export function extendedSession(underlying: UnderlyingSnapshot | undefined): "overnight" | "curb" | null {
  const name = underlying?.paper?.session ?? (underlying?.session?.open ? underlying.session.name : null)
  return name === "global" ? "overnight" : name === "curb" ? "curb" : null
}
export function limitOnlyNotice(symbol: string, underlying: UnderlyingSnapshot | undefined) {
  const session = extendedSession(underlying)
  if (!session) return null
  return session === "overnight"
    ? `${symbol} is in its overnight session (8:15 pm to 9:25 am ET): limit orders only. EXTO and GTC + EXTO conditions and exits work here; GTC and GTD wait for regular hours. A Day order lasts until 9:25 am.`
    : `${symbol} is in its curb session (4:15 to 5:00 pm ET): limit orders only. EXTO and GTC + EXTO conditions and exits work here; GTC and GTD wait for regular hours. A Day order lasts until 5:00 pm.`
}

export function paperNotice(symbol: string, underlying: UnderlyingSnapshot | undefined) {
  const paper = underlying?.paper
  if (paper) return paper.accepting ? null : paper.message || `${symbol} paper orders are unavailable${paper.reason ? ` (${paper.reason})` : ""}.`
  return paperSessionNotice(symbol, underlying?.session)
}

export function ticketEstimate(quote: OptionQuote | null, side: Side, quantity: number, price: Money | null, fee: Money | null) {
  const valid = Number.isSafeInteger(quantity) && quantity > 0 && Number.isSafeInteger(quantity * 100)
  const multiplier = quantity * 100 * (side === "buy" ? 1 : -1)
  const greek = (key: "delta" | "gamma" | "vega" | "theta") => {
    const value = quote?.[key]
    return valid && value != null && Number.isFinite(value) ? value * multiplier : null
  }
  return {
    premium: valid ? multiplyMoney(price, quantity * 100) : null,
    fees: valid ? multiplyMoney(fee, quantity) : null,
    delta: greek("delta"), gamma: greek("gamma"), vega: greek("vega"), theta: greek("theta"),
  }
}

export function scenarioScale(values: (number | null)[][]) {
  return values.flat().reduce<number>((max, value) => value != null && Number.isFinite(value) ? Math.max(max, Math.abs(value)) : max, 0) || 1
}
export function scenarioColour(value: number | null | undefined, scale: number) {
  if (value == null || !Number.isFinite(value) || value === 0) return "var(--panel)"
  const intensity = Math.min(1, Math.abs(value) / Math.max(scale, Number.EPSILON)) * 30
  return `color-mix(in srgb, var(${value > 0 ? "--bullish" : "--bearish"}) ${intensity}%, var(--panel))`
}
