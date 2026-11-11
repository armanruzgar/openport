import { QueryClient, QueryObserver } from "@tanstack/react-query"
import { afterEach, describe, expect, it, vi } from "vitest"
import { writeToken } from "../lib/write-token"
import { connectLive } from "./connection"
import { liveState, marketNow } from "./live"
import type { Status, Tick } from "./types"

const status: Status = {
  provider: { name: "test", realtime: true, delay_seconds: 0, trades: false, open_interest: true, vendor_greeks: false },
  feed: { state: "live", message: "REST", updated: null },
  underlyings: [{ symbol: "SPX", spot: 7002, as_of: "2026-09-22T20:00:00Z", version: 12, expiries: 2, options: 100 }],
  engine: { events: 100, events_per_second: 2, analytics_ms: 3, contracts: 100, uptime_seconds: 60 },
}
const tick: Tick = {
  type: "tick",
  feed: { state: "delayed", message: "old socket data" },
  underlyings: [{ symbol: "SPX", spot: 7000, as_of: "2026-09-22T19:59:00Z", version: 10 }],
  engine: { events_per_second: 1, analytics_ms: 4, contracts: 90 },
}

describe("live state", () => {
  it.each(["closed", "connecting"] as const)("uses REST for versions, prices, feed and engine when %s", (connection) => {
    const state = liveState(status, tick, connection)
    expect(state.tick).toBeNull()
    expect(state.status).toBe(status)
    expect(state.version("SPX")).toBe(12)
    expect(state.version("missing")).toBe(0)
  })

  it("uses connected ticks, including a reset version of zero, and REST before the first tick", () => {
    expect(liveState(status, tick, "open").version("SPX")).toBe(10)
    expect(liveState(status, null, "open").version("SPX")).toBe(12)
    const restarted = { ...tick, underlyings: [{ ...tick.underlyings[0]!, version: 0 }] }
    expect(liveState(status, restarted, "open").version("SPX")).toBe(0)
    expect(liveState(undefined, null, "connecting").version("SPX")).toBe(0)
  })

  it("merges tick health with REST counts and preserves explicit nulls and newly subscribed symbols", () => {
    const rest: Status = { ...status, underlyings: [{ ...status.underlyings[0]!, last_error: "old error", last_error_time: "2026-09-22T19:00:00Z" }] }
    const pending = { symbol: "SPX", version: 0, spot: null, as_of: null, state: "connecting" as const, message: null, last_success: null, last_error: null, last_error_time: null }
    const update: Tick = { ...tick, underlyings: [pending, { ...pending, symbol: "SPY" }] }
    const live = liveState(rest, update, "open")
    expect(live.underlyings[0]).toEqual({ ...rest.underlyings[0], ...pending })
    expect(live.underlyings[1]).toEqual({ ...pending, symbol: "SPY" })
    expect(liveState(rest, update, "closed").underlyings).toBe(rest.underlyings)
    expect(liveState(undefined, null, "connecting").underlyings).toEqual([])
  })

  it("prefers connected breaker snapshots and uses REST after a disconnect or with older ticks", () => {
    const breaker = { symbol: "SPX", day: "2026-09-22", previous_close: null, level: 1,
      halts: [], market_time: "2026-09-22T14:05:00Z", active: true, error: null }
    const rest = { ...status, circuit_breaker: breaker }
    const resumed = { ...breaker, active: false }
    const update = { ...tick, circuit_breaker: resumed }
    expect(liveState(rest, update, "open").circuitBreaker).toBe(resumed)
    expect(liveState(rest, update, "closed").circuitBreaker).toBe(breaker)
    expect(liveState(rest, tick, "open").circuitBreaker).toBe(breaker)
    expect(liveState(rest, null, "connecting").circuitBreaker).toBe(breaker)
    expect(liveState(rest, { ...tick, circuit_breaker: null }, "open").circuitBreaker).toBeNull()
    expect(liveState(status, tick, "open").circuitBreaker).toBeUndefined()
  })

  it("prefers connected market updates, falls back for older ticks, and respects explicit null", () => {
    const market = { open: false, note: "Closed", next_open: null }
    const rest = { ...status, market }
    expect(liveState(rest, tick, "open").market).toBe(market)
    expect(liveState(rest, { ...tick, market: null }, "open").market).toBeNull()
    const update = { ...tick, market: { ...market, open: true } }
    expect(liveState(rest, update, "open").market?.open).toBe(true)
    expect(liveState(rest, update, "closed").market?.open).toBe(false)
  })
})

class Socket {
  static instances: Socket[] = []
  onopen: (() => void) | null = null
  onclose: (() => void) | null = null
  onmessage: ((event: { data: string }) => void) | null = null
  constructor(readonly url: string, readonly protocols?: string[]) { Socket.instances.push(this) }
  open() { this.onopen?.() }
  close() { this.onclose?.() }
  message(value: Tick) { this.onmessage?.({ data: JSON.stringify(value) }) }
}

const cleanups: (() => void)[] = []
afterEach(() => {
  for (const cleanup of cleanups.splice(0).reverse()) cleanup()
  Socket.instances = []
  writeToken.set("")
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

describe("socket recovery", () => {
  it("refetches reused versions on reconnect and marks inactive cache entries stale", async () => {
    vi.useFakeTimers()
    vi.stubGlobal("WebSocket", Socket)
    const client = new QueryClient({ defaultOptions: { queries: { staleTime: Infinity, gcTime: Infinity, retry: false } } })
    cleanups.push(() => client.clear())
    const key = ["summary", "SPX", 1]
    const inactiveKey = ["chain", "SPX", "2026-10-16AM", 1]
    client.setQueryData(key, "old server")
    client.setQueryData(inactiveKey, "inactive old server")
    let server = "first connection"
    const queryFn = vi.fn(async () => server)
    const observer = new QueryObserver<string>(client, { queryKey: key, queryFn })
    cleanups.push(observer.subscribe(() => {}))
    const onTick = vi.fn()
    const onConnection = vi.fn()
    cleanups.push(connectLive("ws://localhost/ws", client, onTick, onConnection))
    const first = Socket.instances[0]!
    first.open()
    // The first open follows the page's own first requests: nothing is cancelled or asked again.
    await vi.advanceTimersByTimeAsync(0)
    expect(queryFn).not.toHaveBeenCalled()
    expect(client.getQueryState(inactiveKey)?.isInvalidated).toBe(false)
    first.message(tick)
    expect(onTick).toHaveBeenLastCalledWith(tick)
    first.close()
    expect(onTick).toHaveBeenLastCalledWith(null)
    expect(onConnection).toHaveBeenLastCalledWith("closed")

    server = "restarted server, same version"
    await vi.advanceTimersByTimeAsync(500)
    const second = Socket.instances[1]!
    second.open()
    await vi.waitFor(() => expect(observer.getCurrentResult().data).toBe(server))
    expect(client.getQueryState(inactiveKey)?.isInvalidated).toBe(true)
    expect(queryFn).toHaveBeenCalledTimes(1)
    expect(onConnection).toHaveBeenLastCalledWith("open")
    first.message(tick)
    first.close()
    expect(onTick).toHaveBeenLastCalledWith(null)
    expect(onConnection).toHaveBeenLastCalledWith("open")
  })

  it("cancels old in-flight requests before invalidating and stops reconnecting after cleanup", async () => {
    vi.useFakeTimers()
    vi.stubGlobal("WebSocket", Socket)
    const client = new QueryClient({ defaultOptions: { queries: { gcTime: Infinity, retry: false } } })
    cleanups.push(() => client.clear())
    let signal: AbortSignal | undefined
    const request = client.fetchQuery({
      queryKey: ["chain", "SPX", 1],
      queryFn: (context) => { signal = context.signal; return new Promise(() => {}) },
    }).catch(() => undefined)
    const onTick = vi.fn()
    const onConnection = vi.fn()
    const cleanup = connectLive("ws://localhost/ws", client, onTick, onConnection)
    cleanups.push(cleanup)
    const first = Socket.instances[0]!
    first.open()
    await vi.advanceTimersByTimeAsync(0)
    expect(signal?.aborted).toBe(false)
    first.close()
    await vi.advanceTimersByTimeAsync(500)
    const socket = Socket.instances[1]!
    socket.open()
    await request
    expect(signal?.aborted).toBe(true)
    socket.close()
    cleanup()
    onTick.mockClear()
    onConnection.mockClear()
    socket.message(tick)
    socket.open()
    await vi.advanceTimersByTimeAsync(10_000)
    expect(Socket.instances).toHaveLength(2)
    expect(onTick).not.toHaveBeenCalled()
    expect(onConnection).not.toHaveBeenCalled()
  })
})


it("refetches on the first open after a failed attempt, when the server may have restarted", async () => {
  vi.useFakeTimers()
  vi.stubGlobal("WebSocket", Socket)
  const client = new QueryClient({ defaultOptions: { queries: { staleTime: Infinity, gcTime: Infinity, retry: false } } })
  cleanups.push(() => client.clear())
  const key = ["summary", "SPX", 1]
  client.setQueryData(key, "before the restart")
  const queryFn = vi.fn(async () => "after the restart")
  const observer = new QueryObserver<string>(client, { queryKey: key, queryFn })
  cleanups.push(observer.subscribe(() => {}))
  cleanups.push(connectLive("ws://localhost/ws", client, () => {}, () => {}))
  // The server is down at first: the attempt closes without opening.
  Socket.instances[0]!.close()
  await vi.advanceTimersByTimeAsync(500)
  Socket.instances[1]!.open()
  await vi.waitFor(() => expect(observer.getCurrentResult().data).toBe("after the restart"))
  expect(queryFn).toHaveBeenCalledOnce()
})

it("authenticates browser sockets through subprotocols without putting tokens in the URL", () => {
  vi.stubGlobal("WebSocket", Socket)
  writeToken.set("abc")
  const client = new QueryClient()
  cleanups.push(() => client.clear())
  cleanups.push(connectLive("ws://localhost/ws", client, () => {}, () => {}))
  expect(Socket.instances[0]?.url).toBe("ws://localhost/ws")
  expect(Socket.instances[0]?.protocols).toEqual(["openport", "openport.token.616263"])
})

it("uses simulated feed timestamps for ages and journal dates across the overnight jump", () => {
  vi.useFakeTimers()
  vi.setSystemTime(new Date("2026-09-27T12:00:00Z"))
  const simulated = { ...status, provider: { ...status.provider, name: "demo", simulated: true } }
  expect(marketNow(liveState(simulated, null, "open"))).toBe(Date.parse(status.underlyings[0]!.as_of!))
  const next = { ...tick, underlyings: [{ ...tick.underlyings[0]!, as_of: "2026-09-23T13:30:00Z" }] }
  expect(marketNow(liveState(simulated, next, "open"))).toBe(Date.parse("2026-09-23T13:30:00Z"))
  expect(marketNow(liveState(status, next, "open"))).toBeGreaterThan(Date.parse("2026-09-23T13:30:00Z"))
})

it("refreshes the switcher's shared finished-run cache on successive replay completions", async () => {
  vi.stubGlobal("WebSocket", Socket)
  const client = new QueryClient({ defaultOptions: { queries: { staleTime: Infinity, gcTime: Infinity, retry: false } } })
  cleanups.push(() => client.clear())
  const key = ["replay-listing"]
  let history = ["first"]
  client.setQueryData(key, { history })
  const fetch = vi.fn(async () => ({ history }))
  const switcher = new QueryObserver(client, { queryKey: key, queryFn: fetch })
  cleanups.push(switcher.subscribe(() => {}))
  cleanups.push(connectLive("ws://localhost/ws", client, () => {}, () => {}))
  const socket = Socket.instances[0]!
  const replay = { id: "second", file: "demo", provider: "demo", symbols: [], started: null, delay_seconds: 0, speed: 0, paused: false, finished: false, time: null }
  socket.message({ ...tick, type: "replay_tick", replay })
  await vi.waitFor(() => expect(fetch).toHaveBeenCalledTimes(1))
  history = ["first", "second"]
  socket.message({ ...tick, type: "replay_tick", replay: { ...replay, finished: true } })
  await vi.waitFor(() => expect(switcher.getCurrentResult().data?.history).toEqual(history))
  expect(fetch).toHaveBeenCalledTimes(2)
  socket.message({ ...tick, type: "replay_tick", replay: { ...replay, finished: true } })
  expect(fetch).toHaveBeenCalledTimes(2)
  // A run can finish between ticks; its ID distinguishes it from the last finish.
  history = [...history, "third"]
  socket.message({ ...tick, type: "replay_tick", replay: { ...replay, id: "third", finished: true } })
  await vi.waitFor(() => expect(switcher.getCurrentResult().data?.history).toEqual(history))
  expect(fetch).toHaveBeenCalledTimes(3)
})
