import { ApiError } from "../api/client"

/** A failed request in words: its message, without the class name String() puts first ("ApiError: ..."). */
export function errorText(error: unknown): string {
  return error instanceof Error ? error.message : typeof error === "string" ? error : "Request failed. Please retry."
}

/**
 * A market-data request's failure in words. Until the feed's first snapshot of an
 * underlying (it has no as_of yet), its summary, chain and analytics answer 404, which is
 * still loading: say so, with the feed's state, rather than show "no data for SPX yet".
 */
export function marketDataError(error: unknown, symbol: string, underlying: { as_of?: string | null } | undefined, feed?: string | null): string {
  if (error instanceof ApiError && error.status === 404 && !underlying?.as_of)
    return `Waiting for the first ${symbol} snapshot…${feed ? ` (feed: ${feed})` : ""}`
  return errorText(error)
}
