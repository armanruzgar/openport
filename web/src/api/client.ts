import { announceDestination, checkWriteDestination } from "./destination"
import type { BacktestComparison, BacktestListing, BacktestState, BacktestStart } from "./backtest-types"
import type { Playbook, PlaybooksResponse, PassOdds } from "./playbook-types"
import type { StrategyTemplate, TemplateResult } from "../lib/strategy"
import type { Volatility, VolatilitySeries } from "./types"
import type { NotificationChannel, NotificationStatus, TokenReloadResponse } from "./types"
import type { CandleInterval, Candles, Chain, ExposureMatrix, Probability, ReplayListing, ReplayState, Status, Summary, Surface, RunVerification } from "./types"
import type { Account, AccountsResponse, UpdateAccountRequest, UpdateAccountResponse, DeleteAccountResponse, AlertDeleted, AlertRequest, AlertResponse, AlertsResponse, CancelAllResponse, CancelRequest, ClosePositionsResponse, CreateAccountRequest, CreateAccountResponse, DayNote, EquityHistory, FillsResponse, FlattenPreview, FlattenPricing, GroupResponse, Guardrails, KillResponse, Limits, Money, NewOrder, OrderChange, OrderPreview, OrderResponse, OrdersResponse, PlansResponse, Portfolio, ResetRequest, Risk, RiskProfile, RiskProfileQuery, SettlementsResponse, SettlementResponse, Side, StockPreview, SubmitOrderResponse, TradeNote, TradeNoteResponse, TradesResponse, WhatIfResponse, WriteMode } from "./trading-types"
import { activeAccount, MAIN_ACCOUNT } from "../lib/active-account"
import { dataSource } from "../lib/data-source"
import { isSandboxToken, writeToken } from "../lib/write-token"

/** What a replay plays: a recording in the recordings directory, or the demo market. */
export type ReplaySource = { file: string } | { demo: true | string }
export interface ReplayControl { speed?: number; paused?: boolean; skip?: boolean; until?: string; abort?: boolean; play_until?: string }
export interface ReplayResult { replay: ReplayState | null; settled_through?: string; aborted?: boolean; message?: string }
export interface ReplayStart { copy_settings_from?: string; plan?: string; start_at?: string; paused?: boolean; seed?: string; date?: string }

export class ApiError extends Error {
  constructor(
    readonly status: number,
    message: string,
    readonly code: string = "HTTP_ERROR",
    readonly actual: number | string | null = null,
    readonly limit: number | string | null = null,
    readonly scope: string | null = null,
  ) {
    super(message)
    this.name = "ApiError"
  }
}

export function mapApiError(status: number, body: unknown, fallback: string): ApiError {
  const error = body && typeof body === "object" && "error" in body ? body.error : null
  if (typeof error === "string") return new ApiError(status, error)
  if (error && typeof error === "object") {
    const e = error as Record<string, unknown>
    return new ApiError(status, typeof e.message === "string" ? e.message : fallback,
      typeof e.code === "string" ? e.code : "HTTP_ERROR",
      typeof e.actual === "string" || (typeof e.actual === "number" && Number.isFinite(e.actual)) ? e.actual : null,
      typeof e.limit === "string" || (typeof e.limit === "number" && Number.isFinite(e.limit)) ? e.limit : null,
      typeof e.scope === "string" ? e.scope : null)
  }
  return new ApiError(status, fallback)
}

async function request<T>(path: string, init: RequestInit): Promise<T> {
  const response = await fetch(path, init)
  if (!response.ok) {
    const body: unknown = await response.json().catch(() => null)
    const error = mapApiError(response.status, body, `${response.status} ${response.statusText}`)
    const sent = new Headers(init.headers).get("Authorization")
    const token = writeToken.get()
    if (isSandboxToken(token) && sent === `Bearer ${token}` && response.status === 403 &&
        (error.code === "SANDBOX_EXPIRED" || error.code === "WRITE_TOKEN_REQUIRED")) {
      activeAccount.set(MAIN_ACCOUNT, "automatic")
      dataSource.set("live", "automatic")
      writeToken.set("")
      announceDestination("The sandbox expired or its write access ended.")
    }
    throw error
  }
  return (await response.json()) as T
}

/** On the replay every route is the replay's: /api/X becomes /api/replay/X. Replay controls stay put. */
function routed(path: string): string {
  if (path === "/api/tokens/reload") return path
  if (path === "/api/backtests" || path.startsWith("/api/backtests/")) return path
  if (path === "/api/replay" || path.startsWith("/api/replay/")) return path
  const source = dataSource.get()
  if (source.startsWith("history:")) return path.replace(/^\/api\//, `/api/replay/history/${encodeURIComponent(source.slice(8))}/`)
  return source === "replay" ? path.replace(/^\/api\//, "/api/replay/") : path
}
export function readHeaders(): Record<string, string> {
  const token = writeToken.get()
  return { Accept: "application/json", ...(token ? { Authorization: `Bearer ${token}` } : {}) }
}
const get = <T,>(path: string, signal?: AbortSignal) => request<T>(routed(path), { signal, headers: readHeaders() })

export async function downloadFile(path: string, filename: string) {
  const response = await fetch(path, { headers: readHeaders() })
  if (!response.ok) throw mapApiError(response.status, await response.json().catch(() => null), response.statusText)
  const url = URL.createObjectURL(await response.blob())
  const link = document.createElement("a")
  link.href = url
  link.download = filename
  link.click()
  setTimeout(() => URL.revokeObjectURL(url), 1000)
}
export const downloadCsv = downloadFile

function write<T>(path: string, method: "POST" | "PUT" | "PATCH" | "DELETE", mode: WriteMode, body?: unknown) {
  const headers: Record<string, string> = { Accept: "application/json", "Content-Type": "application/json" }
  if (mode === "disabled") return Promise.reject(new ApiError(403, "Trading writes are disabled by the server.", "WRITE_DISABLED"))
  if (mode === "token") {
    const token = writeToken.get()
    if (!token) return Promise.reject(new ApiError(403, "Enter a write token to continue.", "WRITE_TOKEN_REQUIRED"))
    headers.Authorization = `Bearer ${token}`
  }
  return request<T>(path, { method, headers, ...(body === undefined ? {} : { body: JSON.stringify(body) }) })
}

/** Account mutations require the destination captured by the action's caller.
 * Server operations and requests naming their target use write directly instead.
 */
function accountWrite<T>(path: string, method: "POST" | "PUT" | "PATCH" | "DELETE", mode: WriteMode, body?: unknown) {
  try { checkWriteDestination() } catch (error) { return Promise.reject(error) }
  return write<T>(routed(path), method, mode, body)
}
/** Account dry runs do not mutate state and never request destination confirmation. */
function previewWrite<T>(path: string, method: "POST", mode: WriteMode, body?: unknown) {
  return write<T>(routed(path), method, mode, body)
}

const underlying = (symbol: string) => `/api/underlyings/${encodeURIComponent(symbol)}`
/** GET /api/risk/profile's query; defaults are left to the server. */
export function riskProfilePath(query: RiskProfileQuery): string {
  const params = new URLSearchParams()
  if (query.underlying) params.set("underlying", query.underlying)
  else if (query.benchmark) params.set("benchmark", query.benchmark)
  if (query.days?.length) params.set("days", query.days.join(","))
  if (query.iv) params.set("iv", String(query.iv))
  if (query.range != null) params.set("range", String(query.range))
  if (query.steps != null) params.set("steps", String(query.steps))
  const betas = Object.entries(query.betas ?? {}).map(([symbol, beta]) => `${symbol}:${beta}`)
  if (betas.length) params.set("betas", betas.join(","))
  return `/api/risk/profile${params.size ? `?${params}` : ""}`
}
/** Trading routes act on the active account; the main one needs no parameter. */
function scoped(path: string): string {
  const account = activeAccount.get()
  // A replay has one account of its own.
  if (account === MAIN_ACCOUNT || dataSource.get() !== "live") return path
  return `${path}${path.includes("?") ? "&" : "?"}account=${encodeURIComponent(account)}`
}

export const api = {
  createSandbox: () => request<{ account: string; token: string; idle_seconds: number; simulated: true }>("/api/sandboxes", {
    method: "POST", headers: { Accept: "application/json", "Content-Type": "application/json" }, body: "{}",
  }),
  backtests: (signal?: AbortSignal) => get<BacktestListing>("/api/backtests", signal),
  backtest: (id: string, signal?: AbortSignal) => get<BacktestState>(`/api/backtests/${encodeURIComponent(id)}`, signal),
  /** One playbook or 2–8 selectors sharing one account in lexical ID order. */
  startBacktest: (body: BacktestStart, mode: WriteMode) => write<BacktestState>("/api/backtests", "POST", mode, body),
  cancelBacktest: (id: string, mode: WriteMode) => write<BacktestState>(`/api/backtests/${encodeURIComponent(id)}`, "DELETE", mode),
  compareBacktests: (ids: string[], signal?: AbortSignal) => get<BacktestComparison>(`/api/backtests/compare?ids=${ids.map(encodeURIComponent).join(",")}`, signal),
  keepBacktest: (id: string, keep: boolean, mode: WriteMode) => write<BacktestState>(`/api/backtests/${encodeURIComponent(id)}`, "PUT", mode, { keep }),
  deleteBacktest: (id: string, mode: WriteMode) => write<{ id: string; deleted: true }>(`/api/backtests/${encodeURIComponent(id)}?purge=true`, "DELETE", mode),
  backtestPlans: (signal?: AbortSignal) => request<PlansResponse>("/api/plans", { signal, headers: readHeaders() }),
  backtestPlaybooks: (signal?: AbortSignal) => request<PlaybooksResponse>("/api/playbooks", { signal, headers: readHeaders() }),
  testNotification: (channel: string, mode: WriteMode) => write<{ queued: boolean }>("/api/notifications/test", "POST", mode, { channel }),
  configureNotification: (channel: string, settings: Pick<NotificationChannel, "enabled" | "events" | "floor_distance">, mode: WriteMode) =>
    write<NotificationStatus>(`/api/notifications/channels/${encodeURIComponent(channel)}`, "PUT", mode, settings),
  playbooks: (signal?: AbortSignal) => get<PlaybooksResponse>(scoped("/api/playbooks"), signal),
  savePlaybook: (definition: Playbook, mode: WriteMode) => accountWrite<PlaybooksResponse>(scoped(`/api/playbooks${definition.version ? `/${encodeURIComponent(definition.id)}` : ""}`), definition.version ? "PUT" : "POST", mode, definition),
  deletePlaybook: (id: string, version: number, mode: WriteMode) => accountWrite<PlaybooksResponse>(scoped(`/api/playbooks/${encodeURIComponent(id)}?version=${version}`), "DELETE", mode),
  playbookMode: (id: string, value: "off" | "stage" | "auto", mode: WriteMode) => accountWrite<PlaybooksResponse>(scoped(`/api/playbooks/${encodeURIComponent(id)}/mode`), "PUT", mode, { mode: value }),
  stagedAction: (id: string, action: "send" | "dismiss", mode: WriteMode) => accountWrite<PlaybooksResponse>(scoped(`/api/playbooks/staged/${encodeURIComponent(id)}/${action}`), "POST", mode, {}),
  passOdds: (days: number, samples: number, playbook = "", signal?: AbortSignal) => get<PassOdds>(scoped(`/api/account/pass-odds?days=${days}&samples=${samples}${playbook ? `&playbook=${encodeURIComponent(playbook)}` : ""}`), signal),
  buildTemplate: (template: StrategyTemplate, near: Chain, signal?: AbortSignal) => {
    const strikes = near.strikes.map((row) => row.strike).filter(Number.isFinite)
    const query = new URLSearchParams({ symbol: near.symbol, expiry: near.expiry.id, template: JSON.stringify(template) })
    if (strikes.length) { query.set("min_strike", String(Math.min(...strikes))); query.set("max_strike", String(Math.max(...strikes))) }
    return get<TemplateResult>(`/api/strategy-template?${query}`, signal)
  },
  previewOrder: (order: NewOrder, mode: WriteMode, floor_share = 0.5) => previewWrite<OrderPreview>(scoped("/api/orders/preview"), "POST", mode, { ...order, floor_share }),
  whatIf: (candidates: readonly { name: string; orders: readonly NewOrder[] }[], mode: WriteMode) =>
    previewWrite<WhatIfResponse>(scoped("/api/orders/what-if"), "POST", mode, { candidates: candidates.map(({ name, orders }) => ({
      name, orders: orders.map((order) => Object.fromEntries(Object.entries(order).filter(([key]) => key !== "client_order_id"))),
    })) }),
  previewChange: (id: string, change: OrderChange, mode: WriteMode, floor_share = 0.5) =>
    previewWrite<OrderPreview>(scoped(`/api/orders/${encodeURIComponent(id)}/preview`), "POST", mode, { ...change, floor_share }),
  equity: async (signal?: AbortSignal): Promise<EquityHistory> => {
    const base = scoped("/api/account/equity")
    const path = `${base}${base.includes("?") ? "&" : "?"}limit=2000`
    let page = await get<EquityHistory>(path, signal)
    const samples = [...page.samples]
    while (page.next) {
      page = await get<EquityHistory>(`${path}&cursor=${encodeURIComponent(page.next)}`, signal)
      samples.push(...page.samples)
    }
    return { ...page, samples }
  },
  updateGuardrails: (expected_revision: string, guardrails: Guardrails, mode: WriteMode) => accountWrite<Risk>(scoped("/api/risk/guardrails"), "PUT", mode, { expected_revision, guardrails }),
  portfolio: (signal?: AbortSignal) => get<Portfolio>(scoped("/api/portfolio"), signal),
  orders: (status: "open" | "all" = "all", signal?: AbortSignal) => get<OrdersResponse>(scoped(`/api/orders?status=${status}`), signal),
  settlements: (signal?: AbortSignal) => get<SettlementsResponse>(scoped("/api/settlements"), signal),
  fills: (signal?: AbortSignal) => get<FillsResponse>(scoped("/api/fills"), signal),
  risk: (signal?: AbortSignal) => get<Risk>(scoped("/api/risk"), signal),
  riskProfile: (query: RiskProfileQuery, signal?: AbortSignal) => get<RiskProfile>(scoped(riskProfilePath(query)), signal),
  order: (id: string, signal?: AbortSignal) => get<OrderResponse>(scoped(`/api/orders/${encodeURIComponent(id)}`), signal),
  submitOrder: (order: NewOrder, mode: WriteMode) => accountWrite<SubmitOrderResponse>(scoped("/api/orders"), "POST", mode, order),
  cancelOrder: (id: string, mode: WriteMode) => accountWrite<OrderResponse>(scoped(`/api/orders/${encodeURIComponent(id)}`), "DELETE", mode),
  modifyOrder: (id: string, change: OrderChange, mode: WriteMode) => accountWrite<SubmitOrderResponse>(scoped(`/api/orders/${encodeURIComponent(id)}`), "PUT", mode, change),
  cancelAllOrders: (underlying: string | null, mode: WriteMode) => accountWrite<CancelAllResponse>(scoped("/api/orders/cancel"), "POST", mode, underlying ? { underlying } : {}),
  previewFlatten: (underlying: string | null, mode: WriteMode, pricing?: FlattenPricing) => previewWrite<FlattenPreview>(scoped("/api/positions/close/preview"), "POST", mode, { ...(underlying ? { underlying } : {}), ...pricing }),
  /** Cancel the listed orders in one transaction, such as both exits of a pair. */
  cancelOrders: (ids: string[], mode: WriteMode) => accountWrite<CancelAllResponse>(scoped("/api/orders/cancel"), "POST", mode, { orders: ids } satisfies CancelRequest),
  closePositions: (underlying: string | null, mode: WriteMode, pricing?: FlattenPricing) => accountWrite<ClosePositionsResponse>(scoped("/api/positions/close"), "POST", mode, { ...(underlying ? { underlying } : {}), ...pricing }),
  journalCsvUrl: (kind: "trades" | "fills", from = "", to = "", attempt: "current" | "all" = "all") => {
    const query = new URLSearchParams()
    if (from) query.set("from", from)
    if (to) query.set("to", to)
    if (kind === "trades") query.set("attempt", attempt)
    return routed(scoped(`/api/${kind}.csv${query.size ? `?${query}` : ""}`))
  },
  alerts: (signal?: AbortSignal) => get<AlertsResponse>(scoped("/api/alerts"), signal),
  createAlert: (alert: AlertRequest, mode: WriteMode) => accountWrite<AlertResponse>(scoped("/api/alerts"), "POST", mode, alert),
  deleteAlert: (id: string, mode: WriteMode) => accountWrite<AlertDeleted>(scoped(`/api/alerts/${encodeURIComponent(id)}`), "DELETE", mode),
  annotateDay: (day: string, note: Pick<DayNote, "plan" | "review">, mode: WriteMode) =>
    accountWrite<{ account_version: string; day: string; note: DayNote }>(scoped(`/api/days/${encodeURIComponent(day)}/note`), "PUT", mode, note),
  annotateTrade: (id: string, note: TradeNote, mode: WriteMode) => accountWrite<TradeNoteResponse>(scoped(`/api/trades/${encodeURIComponent(id)}/note`), "PUT", mode, note),
  /** Joins the open round trips' trades into one whole trade, or takes each out of its trade. */
  groupTrades: (trades: string[], together: boolean, mode: WriteMode) =>
    accountWrite<GroupResponse>(scoped(together ? "/api/trades/group" : "/api/trades/ungroup"), "POST", mode, { trades }),
  exercise: (symbol: string, quantity: number, mode: WriteMode) => accountWrite<Portfolio>(scoped("/api/positions/exercise"), "POST", mode, { symbol, quantity }),
  abandon: (symbol: string, mode: WriteMode) => accountWrite<Portfolio>(scoped("/api/positions/abandon"), "POST", mode, { symbol }),
  exerciseInstruction: (symbol: string, doNotExercise: boolean, mode: WriteMode) =>
    accountWrite<Portfolio>(scoped("/api/positions/instruction"), "POST", mode, { symbol, do_not_exercise: doNotExercise }),
  tradeStock: (symbol: string, side: Side, shares: number, mode: WriteMode) =>
    accountWrite<Portfolio>(scoped("/api/stocks/trade"), "POST", mode, { symbol, side, shares }),
  previewStock: (symbol: string, side: Side, shares: number, mode: WriteMode) =>
    previewWrite<StockPreview>(scoped("/api/stocks/trade/preview"), "POST", mode, { symbol, side, shares }),
  closeStock: (symbol: string, shares: number | null, mode: WriteMode) =>
    accountWrite<Portfolio>(scoped("/api/stocks/close"), "POST", mode, shares == null ? { symbol } : { symbol, shares }),
  reloadTokens: (mode: WriteMode) => write<TokenReloadResponse>("/api/tokens/reload", "POST", mode, {}),
  updateLimits: (expected_revision: string, limits: Limits, mode: WriteMode) => accountWrite<Risk>(scoped("/api/risk/limits"), "PUT", mode, { expected_revision, limits }),
  setKill: (action: "trip" | "reset", reason: string, mode: WriteMode) => accountWrite<KillResponse>(scoped("/api/risk/kill"), "POST", mode, { action, reason }),
  settle: (symbol: string, value: Money, mode: WriteMode) => accountWrite<SettlementResponse>(scoped("/api/settlements"), "POST", mode, { symbol, value }),
  account: (signal?: AbortSignal) => get<Account>(scoped("/api/account"), signal),
  trades: (status: "open" | "closed" | "all" = "all", attempt: "current" | "all" = "current", signal?: AbortSignal) =>
    get<TradesResponse>(scoped(`/api/trades?status=${status}&attempt=${attempt}`), signal),
  plans: (signal?: AbortSignal) => get<PlansResponse>("/api/plans", signal),
  accounts: (signal?: AbortSignal, archived = false) => get<AccountsResponse>(`/api/accounts${archived ? "?archived=true" : ""}`, signal),
  liveAccounts: (signal?: AbortSignal) => request<AccountsResponse>("/api/accounts?archived=true", { signal, headers: readHeaders() }),
  updateAccount: (id: string, request: UpdateAccountRequest, mode: WriteMode) => write<UpdateAccountResponse>(`/api/accounts/${encodeURIComponent(id)}`, "PATCH", mode, request),
  deleteAccount: (id: string, mode: WriteMode) => write<DeleteAccountResponse>(`/api/accounts/${encodeURIComponent(id)}`, "DELETE", mode),
  createAccount: (request: CreateAccountRequest, mode: WriteMode) => write<CreateAccountResponse>("/api/accounts", "POST", mode, request),
  resetAccount: (request: ResetRequest, mode: WriteMode) => accountWrite<Account>(scoped("/api/account/reset"), "POST", mode, request),
  requestPayout: (amount: Money, mode: WriteMode) => accountWrite<Account>(scoped("/api/account/payout"), "POST", mode, { amount }),
  status: (signal?: AbortSignal) => get<Status>("/api/status", signal),
  series: (symbol: string, signal?: AbortSignal) => get<VolatilitySeries>(`${underlying(symbol)}/series?interval=1d&fields=mfiv30,atm30,rr25,rv21,proxy_iv30`, signal),
  briefSeries: (symbol: string, now: number, signal?: AbortSignal) => get<VolatilitySeries>(`${underlying(symbol)}/series?interval=1d&fields=mfiv30,atm30,rr25&from=${Math.floor(now / 1000) - 21 * 86400}&to=${Math.floor(now / 1000)}`, signal),
  volatility: (symbol: string, signal?: AbortSignal) => get<Volatility>(`${underlying(symbol)}/volatility`, signal),
  calendarEvents: (signal?: AbortSignal) => request<{ events: import("./trading-types").PlanEvent[] }>("/api/calendar/events", { signal, headers: readHeaders() }),
  summary: (symbol: string, signal?: AbortSignal) => get<Summary>(`${underlying(symbol)}/summary`, signal),
  chain: (symbol: string, expiry: string, window: number, signal?: AbortSignal) =>
    get<Chain>(`${underlying(symbol)}/chain?expiry=${encodeURIComponent(expiry)}&window=${window}`, signal),
  exposure: (symbol: string, expiries: number, window: number, signal?: AbortSignal) =>
    get<ExposureMatrix>(`${underlying(symbol)}/exposure?expiries=${expiries}&window=${window}`, signal),
  surface: (symbol: string, expiries: number, window: number, signal?: AbortSignal) =>
    get<Surface>(`${underlying(symbol)}/surface?expiries=${expiries}&window=${window}`, signal),
  replay: (signal?: AbortSignal) => get<ReplayListing>("/api/replay", signal),
  startReplay: (source: ReplaySource, speed: number, mode: WriteMode, options: ReplayStart = {}) =>
    write<{ replay: ReplayState }>("/api/replay", "POST", mode, { ...source, speed, ...options }),
  /** Continues a saved run a crash interrupted, paused where it stopped. */
  resumeReplay: (id: string, speed: number, mode: WriteMode) => write<{ replay: ReplayState }>("/api/replay", "POST", mode, { resume: id, speed }),
  restartReplay: (id: string, at: string | undefined, mode: WriteMode, options: { speed?: number; paused?: boolean } = {}) =>
    write<ReplayResult>("/api/replay", "POST", mode, { restart: id, ...(at ? { at } : {}), ...options }),
  verifyReplay: (id: string, mode: WriteMode) => write<RunVerification>(`/api/replay/history/${encodeURIComponent(id)}/verify`, "POST", mode, {}),
  replayVerification: (id: string, signal?: AbortSignal) => get<RunVerification>(`/api/replay/history/${encodeURIComponent(id)}/verify`, signal),
  downloadVerificationReceipt: (id: string) => downloadFile(`/api/replay/history/${encodeURIComponent(id)}/verify?format=receipt`, `${id}-verification.json`),
  deleteReplay: (id: string, mode: WriteMode) => write<{ deleted: string }>(`/api/replay/history/${encodeURIComponent(id)}`, "DELETE", mode),
  controlReplay: (change: ReplayControl, mode: WriteMode) =>
    write<ReplayResult>("/api/replay", "PUT", mode, change),
  stopReplay: (mode: WriteMode) => write<{ replay: null }>("/api/replay", "DELETE", mode),
  probability: (symbol: string, days: number[], prices: number[], signal?: AbortSignal) =>
    get<Probability>(`${underlying(symbol)}/probability?days=${days.join(",")}${prices.length ? `&prices=${prices.join(",")}` : ""}`, signal),
  candles: (symbol: string, interval: CandleInterval, limit: number, signal?: AbortSignal) =>
    get<Candles>(`${underlying(symbol)}/candles?interval=${interval}&limit=${limit}`, signal),
}
