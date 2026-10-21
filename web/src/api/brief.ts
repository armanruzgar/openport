import { useQuery } from "@tanstack/react-query"
import { api } from "./client"
import { marketTime, useLive } from "./live"
import { briefDay, briefLevels, overnightRange, priorSeries, priorSession, todayMove, marketDate } from "../lib/brief"
import { barClock, barDay } from "../lib/candles"

/** Shared by Brief and its optional Trade overlay. No hidden-symbol fan-out. */
export function useBriefMarket(symbol: string, enabled = true, history = true) {
  const live = useLive()
  const version = live.version(symbol)
  const scope = `${live.source}/${live.source === "live" ? "" : live.replay?.id ?? ""}`
  const active = enabled && live.underlyings.some(row => row.symbol === symbol) && !live.source.startsWith("history:")
  const marketKey = (kind: string) => live.source === "live" ? [kind, symbol, version] : [kind, symbol, version, scope]
  const placeholder = <T extends { symbol: string }>(previous: T | undefined, query: { queryKey: readonly unknown[] } | undefined) =>
    previous?.symbol === symbol && (query?.queryKey.length === 3 ? live.source === "live" : query?.queryKey.includes(scope)) ? previous : undefined
  const summary = useQuery({
    queryKey: marketKey("summary"), queryFn: ({ signal }) => api.summary(symbol, signal), enabled: active, placeholderData: (previous, query) => placeholder(previous, query), gcTime: 60_000,
  })
  const volatility = useQuery({
    queryKey: [...marketKey("volatility"), live.account], queryFn: ({ signal }) => api.volatility(symbol, signal), enabled: active,
    placeholderData: (previous, query) => query?.queryKey.at(-1) === live.account ? placeholder(previous, query ? { queryKey: query.queryKey.slice(0, -1) } : undefined) : undefined, refetchInterval: 60_000, gcTime: 60_000,
  })
  const now = marketTime(live, symbol, summary.data?.as_of)
  const minute = Number.isFinite(now) ? Math.floor(now / 60_000) : null
  const series = useQuery({
    queryKey: ["brief-series", symbol, scope, minute], queryFn: ({ signal }) => api.briefSeries(symbol, now, signal),
    placeholderData: (previous, query) => placeholder(previous, query), enabled: active && history && minute != null, staleTime: 60_000, refetchInterval: 60_000, gcTime: 60_000,
  })
  const daily = useQuery({
    queryKey: ["brief-candles", symbol, scope, "1d", minute], queryFn: ({ signal }) => api.candles(symbol, "1d", 15, signal),
    placeholderData: (previous, query) => placeholder(previous, query), enabled: active && minute != null, staleTime: 60_000, refetchInterval: 60_000, gcTime: 60_000,
  })
  const intraday = useQuery({
    queryKey: ["brief-candles", symbol, scope, "1m", minute], queryFn: ({ signal }) => api.candles(symbol, "1m", 1800, signal),
    placeholderData: (previous, query) => placeholder(previous, query), enabled: active && minute != null, staleTime: 60_000, refetchInterval: 60_000, gcTime: 60_000,
  })
  // Another underlying can advance the shared breaker while this symbol is stalled.
  const breakerTime = Date.parse(live.circuitBreaker?.market_time ?? "")
  const breaker = Number.isFinite(breakerTime) && Number.isFinite(now) && marketDate(breakerTime) === marketDate(now) &&
    (barClock(breakerTime / 1000) >= "17:00") === (barClock(now / 1000) >= "17:00") ? live.circuitBreaker : undefined
  const day = briefDay(now, breaker?.day, volatility.data)
  const expectedDay = breaker?.day === day ? breaker.previous_close?.date : null
  const prior = priorSession(daily.data?.bars ?? [], day, now, expectedDay)
  const priorDay = expectedDay ?? (prior ? barDay(prior.t) : null)
  const closeRow = priorSeries(series.data?.rows ?? [], priorDay, now)
  const overnight = overnightRange(intraday.data?.bars ?? [], day, now)
  const move = todayMove(volatility.data, day)
  return { summary, volatility, series, daily, intraday, now, day, prior, priorDay, closeRow, overnight, move,
    levels: briefLevels(prior, overnight, summary.data?.exposure, summary.data?.spot, move),
    currentDate: marketDate(now), active }
}
