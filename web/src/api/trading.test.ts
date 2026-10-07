import { captureDestination, withDestination } from "./destination"
import { api } from "../test/action-api"
import { QueryClient, QueryObserver } from "@tanstack/react-query"
import { afterEach, describe, expect, it, vi } from "vitest"
import { api as rawApi, ApiError, mapApiError } from "./client"
import { liveState } from "./live"
import { sameAccountPlaceholder, tradingQueries } from "./trading"
import { activeAccount } from "../lib/active-account"
import { dataSource } from "../lib/data-source"
import { writeToken } from "../lib/write-token"
import { fill, limits, order, portfolio, risk, status, trading } from "../test/trading-fixtures"

const cleanups: (() => void)[] = []
afterEach(() => { cleanups.splice(0).reverse().forEach((fn) => fn()); writeToken.set(""); dataSource.set("live"); activeAccount.set("main"); vi.unstubAllGlobals(); vi.restoreAllMocks() })

describe("trading API", () => {
  it("maps structured and legacy errors without losing zero or null", () => {
    const error = mapApiError(422, { error: { code: "DELTA_LIMIT", message: "Too much delta", actual: 12, limit: 0, scope: "SPX" } }, "fallback")
    expect(error).toBeInstanceOf(ApiError)
    expect(error).toMatchObject({ status: 422, code: "DELTA_LIMIT", message: "Too much delta", actual: 12, limit: 0, scope: "SPX" })
    expect(mapApiError(409, { error: { code: "LIMITS_REVISION", message: "Changed", actual: null, limit: null, scope: null } }, "fallback")).toMatchObject({ actual: null, limit: null, scope: null })
    expect(mapApiError(404, { error: "Unknown symbol" }, "fallback").message).toBe("Unknown symbol")
    expect(mapApiError(503, null, "fallback").message).toBe("fallback")
  })
  it("sends exact JSON money and a bearer token only in token write mode, including bodyless DELETE", async () => {
    const symbol = order.symbol!
    const fetcher = vi.fn(async () => new Response(JSON.stringify({ account_version: "18", order, fills: [fill] }), { status: 201 }))
    vi.stubGlobal("fetch", fetcher)
    writeToken.set("fixture-token")
    const request = { client_order_id: "client-1", symbol, side: "buy", type: "limit", quantity: 3, limit_price: "4.60", time_in_force: "day" } as const
    await api.submitOrder(request, "token")
    expect(fetcher).toHaveBeenLastCalledWith("/api/orders", expect.objectContaining({ method: "POST", headers: { Accept: "application/json", "Content-Type": "application/json", Authorization: "Bearer fixture-token" }, body: JSON.stringify(request) }))
    await api.cancelOrder("a/b", "open")
    expect(fetcher).toHaveBeenLastCalledWith("/api/orders/a%2Fb", { method: "DELETE", headers: { Accept: "application/json", "Content-Type": "application/json" } })
    await api.updateLimits("3", limits, "token")
    expect(fetcher).toHaveBeenLastCalledWith("/api/risk/limits", expect.objectContaining({ method: "PUT", body: JSON.stringify({ expected_revision: "3", limits }) }))
    await api.setKill("trip", "Risk review", "token")
    expect(fetcher).toHaveBeenLastCalledWith("/api/risk/kill", expect.objectContaining({ method: "POST", body: JSON.stringify({ action: "trip", reason: "Risk review" }) }))
    await api.settle(symbol, "7812.34", "token")
    expect(fetcher).toHaveBeenLastCalledWith("/api/settlements", expect.objectContaining({ body: JSON.stringify({ symbol, value: "7812.34" }) }))
    await api.submitOrder({ client_order_id: "market-id", symbol, side: "sell", type: "market", quantity: 1, time_in_force: "ioc" }, "open")
    expect(fetcher).toHaveBeenLastCalledWith("/api/orders", expect.objectContaining({ body: JSON.stringify({ client_order_id: "market-id", symbol, side: "sell", type: "market", quantity: 1, time_in_force: "ioc" }) }))
    const legs = [{ symbol, side: "sell", ratio: 1 }, { symbol: "SPXW  261016C07010000", side: "buy", ratio: 1 }] as const
    await api.submitOrder({ client_order_id: "spread", legs: [...legs], quantity: 2, type: "limit", limit_price: "-0.80", time_in_force: "day" }, "open")
    expect(fetcher).toHaveBeenLastCalledWith("/api/orders", expect.objectContaining({ body: JSON.stringify({ client_order_id: "spread", legs, quantity: 2, type: "limit", limit_price: "-0.80", time_in_force: "day" }) }))
  })
  it("blocks disabled writes and missing tokens before fetch; turns rejected HTTP responses into typed errors", async () => {
    const fetcher = vi.fn(async () => new Response(JSON.stringify({ error: { code: "LIMITS_REVISION", message: "Reload limits", actual: null, limit: null, scope: null } }), { status: 409 }))
    vi.stubGlobal("fetch", fetcher)
    await expect(api.setKill("trip", "review", "disabled")).rejects.toMatchObject({ code: "WRITE_DISABLED" })
    await expect(api.setKill("trip", "review", "token")).rejects.toMatchObject({ code: "WRITE_TOKEN_REQUIRED" })
    expect(fetcher).not.toHaveBeenCalled()
    await expect(api.updateLimits("2", limits, "open")).rejects.toMatchObject({ status: 409, code: "LIMITS_REVISION", message: "Reload limits" })
  })
  it("reads all endpoints without a write token and passes abort signals", async () => {
    const fetcher = vi.fn(async () => new Response("{}"))
    vi.stubGlobal("fetch", fetcher)
    const signal = new AbortController().signal
    await Promise.all([api.portfolio(signal), api.orders("open", signal), api.orders(), api.fills(signal), api.risk(signal), api.settlements(signal)])
    expect(fetcher.mock.calls.map((call) => (call as unknown[])[0])).toEqual(["/api/portfolio", "/api/orders?status=open", "/api/orders?status=all", "/api/fills", "/api/risk", "/api/settlements"])
    expect(fetcher).toHaveBeenCalledWith("/api/portfolio", { signal, headers: { Accept: "application/json" } })
  })
})
describe("account live updates", () => {
  it("requires the status capability and uses connected tick versions, preserving string versions", () => {
    const tick = { type: "tick" as const, feed: status.feed, underlyings: status.underlyings, engine: status.engine, trading: { ...trading, account_version: "9007199254740993", kill_latched: true } }
    expect(liveState({ ...status, trading: undefined }, tick, "open").trading).toBeUndefined()
    expect(liveState(status, tick, "open").trading?.account_version).toBe("9007199254740993")
    expect(liveState(status, tick, "closed").trading?.account_version).toBe("17")
    expect(liveState(status, { ...tick, trading: null }, "open").trading).toBeNull()
  })
  it("refetches all four resources when the account version changes", async () => {
    let version = "17"
    const fetcher = vi.fn(async (url: string) => {
      const payload = url === "/api/portfolio" ? portfolio : url === "/api/risk" ? risk : url === "/api/fills" ? { fills: [fill] } : { orders: [order] }
      return new Response(JSON.stringify({ ...payload, account_version: version }))
    })
    vi.stubGlobal("fetch", fetcher)
    const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
    cleanups.push(() => client.clear())
    const first = tradingQueries(1, version, true)
    const observer = new QueryObserver(client, first.portfolio)
    cleanups.push(observer.subscribe(() => {}))
    await Promise.all([client.fetchQuery(first.portfolio), client.fetchQuery(first.orders), client.fetchQuery(first.fills), client.fetchQuery(first.risk)])
    expect(fetcher).toHaveBeenCalledTimes(4)
    version = "18"
    const next = tradingQueries(1, version, true)
    observer.setOptions(next.portfolio)
    await Promise.all([client.fetchQuery(next.portfolio), client.fetchQuery(next.orders), client.fetchQuery(next.fills), client.fetchQuery(next.risk)])
    expect(fetcher).toHaveBeenCalledTimes(8)
    expect(observer.getCurrentResult().data?.account_version).toBe("18")
  })
  it("allows placeholders only in the same account connection", () => {
    expect(sameAccountPlaceholder(portfolio, ["trading", 1, "portfolio", "17"], 1)).toBe(portfolio)
    expect(sameAccountPlaceholder(portfolio, ["trading", 1, "portfolio", "17"], 2)).toBeUndefined()
    expect(sameAccountPlaceholder(portfolio, ["summary", 1], 1)).toBeUndefined()
  })
})

it("sends the saved bearer token on reads when the server requires authentication", async () => {
  const fetcher = vi.fn(async () => new Response("{}"))
  vi.stubGlobal("fetch", fetcher)
  writeToken.set("reader-token")
  await api.status()
  expect(fetcher).toHaveBeenLastCalledWith("/api/status", expect.objectContaining({ headers: { Accept: "application/json", Authorization: "Bearer reader-token" } }))
})

it("authenticates CSV downloads and preserves structured permission errors", async () => {
  const { downloadCsv } = await import("./client")
  const fetcher = vi.fn(async () => new Response(JSON.stringify({ error: { code: "SCOPE_REQUIRED", message: "Read scope required" } }), { status: 403 }))
  vi.stubGlobal("fetch", fetcher)
  writeToken.set("agent-token")
  await expect(downloadCsv("/api/fills.csv?account=practice", "fills.csv")).rejects.toMatchObject({ code: "SCOPE_REQUIRED" })
  expect(fetcher).toHaveBeenCalledWith("/api/fills.csv?account=practice", { headers: { Accept: "application/json", Authorization: "Bearer agent-token" } })
})

it("sends extended TIF deadlines and limit flatten pricing unchanged", async () => {
  const fetcher = vi.fn<typeof fetch>(async () => new Response("{}", { status: 200 }))
  vi.stubGlobal("fetch", fetcher)
  const request = { client_order_id: "gtd", symbol: order.symbol!, side: "buy", type: "limit", quantity: 1, limit_price: "4.10", time_in_force: "gtd", good_till: "2026-09-23T14:15:00Z" } as const
  await api.submitOrder(request, "open")
  expect(fetcher).toHaveBeenLastCalledWith("/api/orders", expect.objectContaining({ body: JSON.stringify(request) }))
  for (const send of [api.closePositions, api.previewFlatten]) {
    await send("SPX", "open", { type: "limit", limit_ticks: 2 })
    expect(JSON.parse(fetcher.mock.calls.at(-1)![1]!.body as string)).toEqual({ underlying: "SPX", type: "limit", limit_ticks: 2 })
  }
})


it("routes share trades and previews to the named live account or replay", async () => {
  const fetcher = vi.fn(async () => new Response("{}"))
  vi.stubGlobal("fetch", fetcher)
  try {
    activeAccount.set("hedges")
    await api.tradeStock("SPY", "sell", 125, "open")
    expect(fetcher).toHaveBeenLastCalledWith("/api/stocks/trade?account=hedges", expect.objectContaining({
      method: "POST", body: JSON.stringify({ symbol: "SPY", side: "sell", shares: 125 }),
    }))
    dataSource.set("replay")
    await api.previewStock("SPY", "buy", 100, "open")
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay/stocks/trade/preview", expect.objectContaining({
      method: "POST", body: JSON.stringify({ symbol: "SPY", side: "buy", shares: 100 }),
    }))
  } finally { dataSource.set("live"); activeAccount.set("main") }
})

it("routes verification outside the selected account and authenticates JSON receipts", async () => {
  const fetcher = vi.fn(async () => new Response(JSON.stringify({ status: "running", message: "Verifying" })))
  vi.stubGlobal("fetch", fetcher)
  writeToken.set("replay-token")
  dataSource.set("history:other-run")
  await api.verifyReplay("saved run", "token")
  expect(fetcher).toHaveBeenLastCalledWith("/api/replay/history/saved%20run/verify", expect.objectContaining({ method: "POST", body: "{}",
    headers: expect.objectContaining({ Authorization: "Bearer replay-token" }) }))
  await api.replayVerification("saved run")
  expect(fetcher).toHaveBeenLastCalledWith("/api/replay/history/saved%20run/verify", expect.objectContaining({ headers: expect.objectContaining({ Authorization: "Bearer replay-token" }) }))
  fetcher.mockImplementation(async () => new Response(JSON.stringify({ error: { code: "VERIFICATION_UNAVAILABLE", message: "Verify again" } }), { status: 409 }))
  await expect(api.downloadVerificationReceipt("saved run")).rejects.toMatchObject({ code: "VERIFICATION_UNAVAILABLE" })
  expect(fetcher).toHaveBeenLastCalledWith("/api/replay/history/saved%20run/verify?format=receipt", { headers: { Accept: "application/json", Authorization: "Bearer replay-token" } })
})

 it.each(["live", "replay", "history:other-run"] as const)("reloads named tokens on the live admin route from %s", async (source) => {
   const fetcher = vi.fn(async () => new Response(JSON.stringify({ names: ["operator"], count: 1, loaded_at: "2026-09-22T14:00:00Z" }), { status: 200 }))
   vi.stubGlobal("fetch", fetcher)
   dataSource.set(source)
   activeAccount.set("evaluation")
   writeToken.set("admin-token")
   await api.reloadTokens("token")
   expect(fetcher).toHaveBeenLastCalledWith("/api/tokens/reload", expect.objectContaining({ method: "POST", body: "{}",
     headers: expect.objectContaining({ Authorization: "Bearer admin-token" }) }))
 })

it("collects equity pages without dropping samples at equal market times", async () => {
  const fetcher = vi.fn().mockResolvedValueOnce(new Response(JSON.stringify({ samples: [{ time: "same", fill: "1" }], error: null, next: "123:1" })))
    .mockResolvedValueOnce(new Response(JSON.stringify({ samples: [{ time: "same", fill: "2" }], error: "equity: failed", error_time: "2026-09-22T14:00:00Z", error_recovered: true, next: null })))
  vi.stubGlobal("fetch", fetcher)
  const result = await api.equity()
  expect(result.samples.map((sample) => sample.fill)).toEqual(["1", "2"])
  expect(result.error_recovered).toBe(true)
  expect(String(fetcher.mock.calls[0]![0])).toContain("limit=2000")
  expect(String(fetcher.mock.calls[1]![0])).toContain("cursor=123%3A1")
})

 it("refuses unbound writes so a new UI caller cannot silently use send-time routing", async () => {
  const fetcher = vi.fn()
  vi.stubGlobal("fetch", fetcher)
  await expect(rawApi.cancelAllOrders(null, "open")).rejects.toThrow("no opening destination")
  expect(fetcher).not.toHaveBeenCalled()
})


it("server and explicitly targeted writes ignore missing and stale destinations", async () => {
  const send = vi.fn(async () => new Response("{}"))
  vi.stubGlobal("fetch", send)
  const destination = captureDestination()
  dataSource.set("replay"); activeAccount.set("other")
  for (const invoke of [
    () => rawApi.controlReplay({ paused: false }, "open"),
    () => rawApi.configureNotification("hook", { enabled: true, events: [], floor_distance: "0.00" }, "open"),
    () => rawApi.testNotification("hook", "open"),
    () => rawApi.reloadTokens("open"),
    () => rawApi.updateAccount("named", { name: "Renamed" }, "open"),
    () => rawApi.deleteAccount("named", "open"),
    () => rawApi.stopReplay("open"),
    () => rawApi.deleteReplay("saved", "open"),
    () => rawApi.cancelBacktest("test", "open"),
  ]) {
    await invoke()
    await withDestination<Promise<unknown>>(destination, invoke)
  }
  expect(send).toHaveBeenCalledTimes(18)
  expect(send).toHaveBeenCalledWith("/api/notifications/channels/hook", expect.objectContaining({ method: "PUT" }))
  expect(send).toHaveBeenCalledWith("/api/accounts/named", expect.objectContaining({ method: "PATCH" }))
})
