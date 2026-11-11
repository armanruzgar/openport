import { QueryClient } from "@tanstack/react-query"

/** How long a market query is kept once nothing reads it. Market queries key on their
 * underlying's data version, which moves with the once-a-second tick, so a superseded
 * version is never read again; TanStack's default of five minutes kept about 300 copies
 * of each mounted chain, summary or surface. A few seconds still covers a quick return
 * to a page, and a placeholder reads its observer's previous result, not the cache. */
export const marketGcTime = 5_000
/** The first key element of every query keyed on an underlying's data version. */
export const marketQueryPrefixes = ["summary", "chain", "surface", "volatility", "candles", "watch-close", "probability", "exposure", "flow", "template"] as const

export function createQueryClient() {
  const client = new QueryClient({
    defaultOptions: {
      queries: {
        // Freshness comes from the WebSocket tick (queries key on the data version),
        // so there is no need to refetch on focus or on a timer.
        staleTime: Infinity,
        refetchOnWindowFocus: false,
        retry: 1,
      },
    },
  })
  for (const prefix of marketQueryPrefixes) client.setQueryDefaults([prefix], { gcTime: marketGcTime })
  return client
}
