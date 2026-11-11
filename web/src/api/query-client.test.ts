import { QueryObserver } from "@tanstack/react-query"
import { describe, expect, it } from "vitest"
import { createQueryClient, marketGcTime } from "./query-client"

describe("the terminal's query client", () => {
  it("lets a superseded market version go seconds after its observer moves on", () => {
    const client = createQueryClient()
    const key = (version: number) => ["chain", "SPX", "2026-10-22", 0, version]
    const observer = new QueryObserver(client, { queryKey: key(1), queryFn: () => 1 })
    const unsubscribe = observer.subscribe(() => {})
    // The next tick's version replaces the observer's key.
    observer.setOptions({ queryKey: key(2), queryFn: () => 2 })
    const superseded = client.getQueryCache().find({ queryKey: key(1), exact: true })
    expect(superseded?.getObserversCount()).toBe(0)
    expect(superseded?.gcTime).toBe(marketGcTime)
    expect(client.getQueryCache().find({ queryKey: key(2), exact: true })?.gcTime).toBe(marketGcTime)
    unsubscribe()
  })
  it("shortens only market data, whose keys carry the underlying's data version", () => {
    const client = createQueryClient()
    for (const prefix of ["summary", "chain", "surface", "volatility", "candles", "watch-close", "probability", "exposure", "flow", "template"])
      expect(client.defaultQueryOptions({ queryKey: [prefix, "SPX", 7] }).gcTime).toBe(marketGcTime)
    expect(client.defaultQueryOptions({ queryKey: ["plans"] }).gcTime).toBeUndefined()
    // Queries that choose their own keep it.
    expect(client.defaultQueryOptions({ queryKey: ["trading", 0, "portfolio", "7"], gcTime: 60_000 }).gcTime).toBe(60_000)
    expect(client.defaultQueryOptions({ queryKey: ["plans"] }).staleTime).toBe(Infinity)
  })
})
