import type { DestinationChangeReason } from "../lib/destination-change"
import { watchReplay } from "./replay-watch"
import { announceDestination, subscribeDestinationNotices, captureDestination, destinationLabel, rememberAccounts, rememberReplay } from "./destination"
import { DestinationConfirmation } from "./action-client"
import { useQuery, useQueryClient } from "@tanstack/react-query"
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from "react"
import { api } from "./client"
import { connectLive, type Connection } from "./connection"
import { activeAccount, MAIN_ACCOUNT, useActiveAccount } from "../lib/active-account"
import { isSandboxToken, useWriteToken, writeToken } from "../lib/write-token"
import { dataSource, useDataSource, type DataSource } from "../lib/data-source"
import type { AccountBrief, ReplayState, Status, Tick, UnderlyingSnapshot, UnderlyingStatus } from "./types"

export type { Connection } from "./connection"

interface Live {
  /** The active account's trading status. */
  trading: Status["trading"]
  accountScope: number
  /** Every paper account, and the one the terminal acts on. */
  accounts: AccountBrief[]
  account: string
  switchAccount: (id: string, reason: DestinationChangeReason) => void
  /** Whether the terminal shows the live feed or the replay running beside it, and that replay. */
  source: DataSource
  replay: ReplayState | null
  switchSource: (source: DataSource, reason: DestinationChangeReason) => void
  status: Status | undefined
  tick: Tick | null
  connection: Connection
  market: Status["market"]
  circuitBreaker: Status["circuit_breaker"]
  underlyings: (UnderlyingSnapshot & Partial<Pick<UnderlyingStatus, "expiries" | "options">>)[]
  /** Version of an underlying's data: queries include it in their key and refetch when it moves. */
  version: (symbol: string) => number
}

const LiveContext = createContext<Live | null>(null)

export function liveState(status: Status | undefined, tick: Tick | null, connection: Connection, accountScope = 0,
  account = MAIN_ACCOUNT, switchAccount: (id: string, reason: DestinationChangeReason) => void = () => {},
  source: DataSource = "live", replay: ReplayState | null = null, switchSource: (source: DataSource, reason: DestinationChangeReason) => void = () => {}): Live {
  const connectedTick = connection === "open" ? tick : null
  const accounts = connectedTick?.accounts ?? status?.accounts ?? []
  const main = connectedTick?.trading === undefined ? status?.trading : connectedTick.trading
  // Another account's status comes from the account list; older servers have only the main one.
  const trading = account === MAIN_ACCOUNT ? main : accounts.find((a) => a.id === account)?.trading
  const replayReason = source === "replay" && (replay?.fast_forwarding ? "REPLAY_FAST_FORWARD" : replay?.stepping ? "REPLAY_STEPPING"
    : replay?.finished ? "REPLAY_READ_ONLY" : null)
  const availableTrading = replayReason && trading ? { ...trading, write: "disabled" as const, reason: replayReason } : trading
  return {
    // REST capability is required: old servers must never expose trading UI.
    trading: status?.trading ? availableTrading : undefined,
    accountScope,
    accounts,
    account,
    switchAccount,
    source,
    replay,
    switchSource,
    status,
    tick: connectedTick,
    connection,
    market: connectedTick?.market === undefined ? status?.market : connectedTick.market,
    circuitBreaker: connectedTick?.circuit_breaker === undefined ? status?.circuit_breaker : connectedTick.circuit_breaker,
    underlyings: connectedTick
      ? connectedTick.underlyings.map((u) => ({ ...status?.underlyings.find((previous) => previous.symbol === u.symbol), ...u }))
      : status?.underlyings ?? [],
    version: (symbol) => connectedTick?.underlyings.find((u) => u.symbol === symbol)?.version
      ?? status?.underlyings.find((u) => u.symbol === symbol)?.version ?? 0,
  }
}

/// Keeps one WebSocket to openportd open (reconnecting with backoff) and exposes the
/// latest connected tick. REST status polls more often while the socket is down.
export function LiveProvider({ children }: { children: ReactNode }) {
  const queryClient = useQueryClient()
  const token = useWriteToken()
  const sandbox = isSandboxToken(token) ? writeToken.sandboxAccount() : ""
  const [tick, setTick] = useState<Tick | null>(null)
  const [replayTick, setReplayTick] = useState<Tick | null>(null)
  const [reconnecting, setReconnecting] = useState(false)
  const [returnReplay, setReturnReplay] = useState(false)
  const [notice, setNotice] = useState<string | null>(null)
  const replaySeen = useRef(Date.now())
  const replayRun = useRef<string | undefined>(undefined)
  const [connection, setConnection] = useState<Connection>("connecting")
  const [accountScope, setAccountScope] = useState(0)
  const account = useActiveAccount()
  const source = useDataSource()

  useEffect(() => {
    const scheme = window.location.protocol === "https:" ? "wss" : "ws"
    setTick(null)
    setReplayTick(null)
    return connectLive(`${scheme}://${window.location.host}/ws`, queryClient, setTick, (next) => {
      setConnection(next)
      setAccountScope((scope) => scope + 1)
    }, (next) => {
      if (next) { replaySeen.current = Date.now(); setReconnecting(false); rememberReplay(next.replay?.id) }
      setReplayTick(next)
    })
  }, [queryClient, token])

  useEffect(() => subscribeDestinationNotices(setNotice), [])

  useEffect(() => { void queryClient.invalidateQueries() }, [queryClient, token])

  const history = useQuery({ enabled: !isSandboxToken(token), queryKey: ["replay-listing"], queryFn: ({ signal }) => api.replay(signal), refetchInterval: 10_000 })
  const archived = source.startsWith("history:") ? history.data?.history?.find((run) => run.id === source.slice(8)) : undefined
  const status = useQuery({
    queryKey: ["status", source, sandbox || (token ? "authenticated" : "public")],
    queryFn: ({ signal }) => api.status(signal),
    refetchInterval: connection === "open" ? 10_000 : 2_000,
  })

  // A new account is a new scope: account-bound views remount and refetch.
  const switchAccount = useCallback((id: string, reason: DestinationChangeReason) => {
    activeAccount.set(id, reason)
    setAccountScope((scope) => scope + 1)
  }, [])
  // Everything cached came from the other source: drop it, and remount account views.
  const switchSource = useCallback((next: DataSource, reason: DestinationChangeReason) => {
    if (dataSource.get() === next) return
    dataSource.set(next, reason)
    queryClient.removeQueries()
    setAccountScope((scope) => scope + 1)
  }, [queryClient])
  // A new run reuses /api/replay but has a different account, including when
  // another browser started it. Do not reuse the previous run's cached trades.
  useEffect(() => {
    const id = replayTick?.replay?.id
    if (id && replayRun.current && id !== replayRun.current && source === "replay") {
      queryClient.removeQueries({ queryKey: ["trading"] })
      void queryClient.invalidateQueries({ queryKey: ["status", "replay"] })
      setAccountScope((scope) => scope + 1)
    }
    replayRun.current = id
  }, [replayTick?.replay?.id, source, queryClient])
  // A replay that stopped, here or in another window, returns the terminal to the live feed.
  useEffect(() => {
    if (source !== "replay") return
    replaySeen.current = Date.now()
    return watchReplay({ now: Date.now, seen: () => replaySeen.current, probe: async signal => {
        const listing = await api.replay(signal)
        setReturnReplay(!!listing.replay)
        queryClient.setQueryData(["replay-listing"], listing)
        return listing
      },
      reconnecting: setReconnecting, leave: reason => {
        switchSource("live", "automatic")
        setNotice(`${reason} Orders now go to ${destinationLabel(captureDestination())}.`)
      } })
  }, [source, switchSource, queryClient])
  // Fall back to the main account when the active one is gone (or the server keeps only one).
  const known = tick?.accounts ?? (source === "live" ? status.data?.accounts ?? (status.data ? [] : undefined) : undefined)
  useEffect(() => {
    if (!isSandboxToken(token) && source === "live" && known && account !== MAIN_ACCOUNT && !known.some((a) => a.id === account)) {
      switchAccount(MAIN_ACCOUNT, "automatic")
      announceDestination("The active account is no longer available.")
    }
  }, [source, known, account, switchAccount, token])
  useEffect(() => {
    if (!sandbox) return
    if (source !== "live") switchSource("live", "automatic")
    if (sandbox !== account) switchAccount(sandbox, "automatic")
    if (source === "live" && status.data && !status.isFetching && !status.data.sandboxes?.enabled) {
      activeAccount.set(MAIN_ACCOUNT, "automatic")
      writeToken.set("")
      announceDestination("Sandbox access ended.")
    }
  }, [sandbox, status.data, status.isFetching, account, switchAccount, source, switchSource])
  useEffect(() => {
    rememberAccounts(known ?? [])
    if (history.data) setReturnReplay(!!history.data.replay)
  }, [known, history.data])
  useEffect(() => {
    let previous = captureDestination()
    const changed = (reason: DestinationChangeReason) => {
      const next = captureDestination()
      if (next.source !== previous.source || next.account !== previous.account) {
        if (reason === "automatic") setNotice(`Destination changed from ${destinationLabel(previous)}. Orders now go to ${destinationLabel(next)}.`)
        previous = next
      }
    }
    const accountSubscription = activeAccount.subscribe(changed)
    const sourceSubscription = dataSource.subscribe(changed)
    return () => { accountSubscription(); sourceSubscription() }
  }, [])
  const value = useMemo<Live>(
    () => liveState(status.data, source === "replay" ? replayTick : source === "live" ? tick : null, connection, accountScope,
      source !== "live" ? MAIN_ACCOUNT : account, switchAccount, source, source.startsWith("history:") ? archived ?? null : replayTick?.replay ?? null, switchSource),
    [status.data, tick, replayTick, connection, accountScope, account, switchAccount, source, switchSource, archived],
  )
  return <LiveContext value={value}>
    {reconnecting && source === "replay" && <div role="status" className="border-b border-warn/40 bg-warn/10 px-4 py-2 text-xs">Reconnecting to the replay…</div>}
    {notice && <div role="alert" className="border-b border-warn/40 bg-warn/10 px-4 py-2 text-xs">
      {notice}
      {source !== "replay" && (history.data ? !!history.data.replay : returnReplay) && <button className="trade-button ml-3" onClick={() => switchSource("replay", "user")}>Back to replay</button>}
      <button className="trade-button ml-3" onClick={() => setNotice(null)}>OK</button>
    </div>}
    {children}<DestinationConfirmation />
  </LiveContext>
}

/** Replay and simulated feeds use market time; live providers use the wall clock. */
export function marketNow(live: Pick<Live, "source" | "replay" | "status" | "underlyings">): number {
  const replay = live.source !== "live" && live.replay?.time ? Date.parse(live.replay.time) : Number.NaN
  if (Number.isFinite(replay)) return replay
  if (live.status?.provider.simulated) {
    const times = live.underlyings.map(underlying => Date.parse(underlying.as_of ?? "")).filter(Number.isFinite)
    if (times.length) return Math.max(...times)
  }
  return Date.now()
}

/** Latest effective market time for a chain, including delayed feeds and paused replays. */
export function marketTime(live: Pick<Live, "underlyings">, symbol: string, asOf?: string | null): number {
  const time = live.underlyings.find((u) => u.symbol === symbol)?.as_of ?? asOf
  return time ? Date.parse(time) : Number.NaN
}

export function useLive(): Live {
  const live = useContext(LiveContext)
  if (!live) throw new Error("useLive outside LiveProvider")
  return live
}
