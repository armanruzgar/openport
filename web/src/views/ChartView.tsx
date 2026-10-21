import { useQuery } from "@tanstack/react-query"
import { api } from "../api/client"
import { marketTime, useLive } from "../api/live"
import { PriceChart } from "../components/PriceChart"
import { defaultExpiry } from "../lib/expiry"
import { matchingPayload } from "../lib/payload"

export function ChartView({ symbol, expiry }: { symbol: string; expiry?: string | null }) {
  const live = useLive()
  const summary = useQuery({ queryKey: ["summary", symbol, live.version(symbol)], queryFn: ({ signal }) => api.summary(symbol, signal) }).data
  const data = matchingPayload(summary, symbol)
  const id = expiry && data?.expiries.some((e) => e.id === expiry) ? expiry
    : defaultExpiry(data?.expiries ?? [], marketTime(live, symbol, data?.as_of))
  return <PriceChart key={symbol} symbol={symbol} spot={data?.spot ?? null} expiry={data?.expiries.find((e) => e.id === id) ?? null} />
}
