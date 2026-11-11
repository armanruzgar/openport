import { ApiError } from "../api/client"

/** A failed request in words: its message, without the class name String() puts first ("ApiError: ..."). */
export function errorText(error: unknown): string {
  return error instanceof Error ? error.message : typeof error === "string" ? error : "Request failed. Please retry."
}

/**
 * A market-data request's failure in words. Until the feed's first snapshot of an
 * underlying, its summary, chain and analytics answer 503 NOT_READY (older servers: 404
 * while it has no as_of yet), which is still loading: say so, with the feed's state,
 * rather than show "No data for SPX yet".
 */
export function marketDataError(error: unknown, symbol: string, underlying: { as_of?: string | null } | undefined, feed?: string | null): string {
  if (error instanceof ApiError && (error.code === "NOT_READY" || (error.status === 404 && !underlying?.as_of)))
    return `Waiting for the first ${symbol} snapshot…${feed ? ` (feed: ${feed})` : ""}`
  return errorText(error)
}
