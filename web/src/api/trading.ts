import { queryOptions, useQuery, useQueryClient } from "@tanstack/react-query"
import { useEffect, useRef } from "react"
import { api } from "./client"
import { useLive } from "./live"

export function tradingQueries(scope: number, version: string | undefined, enabled: boolean) {
  // The contract has one account per server and no account ID. A connection
  // epoch isolates potentially different accounts, even when versions repeat.
  const common = { enabled, placeholderData: <T,>(previous: T | undefined, query: { queryKey: readonly unknown[] } | undefined) => sameAccountPlaceholder(previous, query?.queryKey, scope), gcTime: 60_000 }
  return {
    portfolio: queryOptions({ ...common, queryKey: ["trading", scope, "portfolio", version], queryFn: ({ signal }) => api.portfolio(signal) }),
    orders: queryOptions({ ...common, queryKey: ["trading", scope, "orders", version], queryFn: ({ signal }) => api.orders("open", signal) }),
    settlements: queryOptions({ ...common, queryKey: ["trading", scope, "settlements", version], queryFn: ({ signal }) => api.settlements(signal) }),
    fills: queryOptions({ ...common, queryKey: ["trading", scope, "fills", version], queryFn: ({ signal }) => api.fills(signal) }),
    risk: queryOptions({ ...common, queryKey: ["trading", scope, "risk", version], queryFn: ({ signal }) => api.risk(signal) }),
    account: queryOptions({ ...common, queryKey: ["trading", scope, "account", version], queryFn: ({ signal }) => api.account(signal) }),
    allOrders: queryOptions({ ...common, queryKey: ["trading", scope, "orders-all", version], queryFn: ({ signal }) => api.orders("all", signal) }),
    trades: (attempt: "current" | "all") => queryOptions({ ...common, queryKey: ["trading", scope, "trades", attempt, version],
      queryFn: ({ signal }) => api.trades("all", attempt, signal) }),
    alerts: queryOptions({ ...common, queryKey: ["trading", scope, "alerts", version], queryFn: ({ signal }) => api.alerts(signal) }),
    fillQuality: (attempt: "current" | "all") => queryOptions({ ...common, queryKey: ["trading", scope, "fill-quality", attempt, version],
      queryFn: ({ signal }) => api.fillQuality(attempt, signal) }),
  }
}
export function sameAccountPlaceholder<T>(data: T | undefined, key: readonly unknown[] | undefined, scope: number) {
  return key?.[0] === "trading" && key[1] === scope ? data : undefined
}
function useOptions() {
  const { trading, accountScope } = useLive()
  return tradingQueries(accountScope, trading?.account_version, trading?.enabled === true || trading?.reason?.startsWith("ACCOUNT_DAMAGED:") === true)
}
export function useTradingQueries() {
  const options = useOptions()
  return { portfolio: useQuery(options.portfolio), orders: useQuery(options.orders), fills: useQuery(options.fills), risk: useQuery(options.risk) }
}
export const useAccount = () => useQuery(useOptions().account)
export const usePortfolio = () => useQuery(useOptions().portfolio)
export const useAllOrders = () => useQuery(useOptions().allOrders)
export const useOpenOrders = () => useQuery(useOptions().orders)
export const useSettlements = () => useQuery(useOptions().settlements)
export const useFills = () => useQuery(useOptions().fills)
export const useRisk = () => useQuery(useOptions().risk)
export const useTrades = (attempt: "current" | "all" = "current") => useQuery(useOptions().trades(attempt))
export const useServerAlerts = () => useQuery(useOptions().alerts)
export const useFillQuality = (attempt: "current" | "all" = "current") => useQuery(useOptions().fillQuality(attempt))
/** Presets never change while the server runs. */
export function usePlans(enabled = true) {
  return useQuery({ queryKey: ["plans"], queryFn: ({ signal }) => api.plans(signal), enabled, staleTime: Infinity })
}
export function useRefreshTrading() {
  const client = useQueryClient()
  return () => Promise.all([
    client.invalidateQueries({ queryKey: ["trading"] }),
    client.invalidateQueries({ queryKey: ["status"] }),
  ])
}
/** Prevent a late mutation response from being rendered in another connection. */
export function useTradingSession() {
  const { accountScope } = useLive()
  const current = useRef<number | null>(accountScope)
  current.current = accountScope
  useEffect(() => { current.current = accountScope; return () => { current.current = null } }, [accountScope])
  return () => current.current === accountScope
}
