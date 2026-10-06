// Shapes of openportd's JSON API. Numbers the engine could not compute are null.
import type { Money, TradingStatus } from "./trading-types"

export type Num = number | null

export type FeedState = "connecting" | "live" | "delayed" | "stale" | "error" | "stopped"

export interface ProviderInfo {
  name: string
  /** Generated prices, not market data: the demo market. Absent on older servers. */
  simulated?: boolean
  realtime: boolean
  realtime_plan_dependent?: boolean | null
  delay_seconds: number
  poll_interval_seconds?: Num
  trades: boolean
  open_interest: boolean
  vendor_greeks: boolean
}

export interface EngineMetrics {
  events_per_second: Num
  analytics_ms: Num
  contracts: number
  nonstandard_contracts?: Num
  queue_depth?: Num
  coalesced_events?: Num
  dropped_events?: Num
  overloaded?: boolean | null
}

export interface EngineInfo extends EngineMetrics {
  events: number
  uptime_seconds: number
}

export interface UnderlyingSnapshot {
  symbol: string
  spot: Num
  as_of: string | null
  version: number
  session?: TradingSession | null
  paper?: PaperAcceptance | null
  has_tradable_contracts?: boolean
  state?: FeedState | null
  message?: string | null
  last_success?: string | null
  last_error?: string | null
  last_error_time?: string | null
}

export interface UnderlyingStatus extends UnderlyingSnapshot {
  expiries: number
  options: number
}

export interface TradingSession {
  name: "regular" | "curb" | "global" | "closed"
  open: boolean
  note: string
}

export interface PaperAcceptance {
  accepting: boolean
  reason: string | null
  message: string | null
  /** The session new orders enter, by the market-data clock; null before any data. */
  session?: TradingSession["name"] | null
}

export interface MarketSession {
  open: boolean
  note: string
  next_open: string | null
}

/** One paper account, as status and ticks list them. */
export interface AccountBrief { damaged?: import("./trading-types").AccountDamage | null; journal_size?: import("./trading-types").JournalSize; id: string; name: string; trading: TradingStatus; sandbox_idle_seconds?: number }

export interface MarketHalt {
  level: number
  start: string
  end: string
  reference: number
  price: number
  active: boolean
}

export interface CircuitBreaker {
  inactive_reason?: "MISSING_PREVIOUS_CLOSE" | null
  symbol: string
  day: string | null
  previous_close: { date: string; price: number } | null
  level: number
  halts: MarketHalt[]
  market_time: string | null
  active: boolean
  error: string | null
}

export interface TokenStatus {
  loaded_at: string
  count: number
}
export interface TokenReloadResponse extends TokenStatus {
  names: string[]
}
export interface Status {
  tokens?: TokenStatus
  sandboxes?: { enabled: boolean; idle_seconds: number }
  /** Running openportd version; absent on older servers. */
  version?: string
  notifications?: NotificationStatus
  series?: { enabled: boolean; directory: string | null; rows_today: number; last_write: string | null; last_error: string | null }
  trading?: TradingStatus | null
  /** Every paper account, the main one first; absent on older servers. */
  accounts?: AccountBrief[] | null
  provider: ProviderInfo
  feed: { state: FeedState; message: string; updated: string | null }
  underlyings: UnderlyingStatus[]
  engine: EngineInfo
  market?: MarketSession | null
  circuit_breaker?: CircuitBreaker | null
}

export type NotificationEvent = "fill" | "order_rejected" | "floor" | "rule_trip" | "assignment" | "exercise" | "playbook_ready" | "feed_stalled" | "alert"
export interface NotificationChannel {
  id: string
  type: "webhook" | "discord" | "telegram" | "ntfy"
  enabled: boolean
  events: NotificationEvent[]
  floor_distance: Money
  delivered: number
  failures: number
  dropped: number
  last_attempt: string | null
  last_delivery: string | null
  last_error: string | null
}
export interface NotificationStatus {
  /** Forwarding and management are available on this source; channels may still be configured when false. */
  enabled: boolean
  /** Operator's server-config opt-in for demo/replay forwarding; absent on older servers. */
  include_simulated?: boolean
  queue_depth: number
  queue_capacity: number
  dropped: number
  /** Public channel settings and shared counters, independent of this source's forwarding availability. */
  channels: NotificationChannel[]
}

export type SpotSource = "quote" | "parity" | null

export interface Coverage {
  options: Num
  quoted: Num
  priced: Num
  open_interest: Num
  /** Contracts with current-session volume; absent on older servers. */
  volume?: Num
}

export interface Expiry {
  id: string
  expiry: string
  settlement: "AM" | "PM"
  expiry_time: string
  last_trade?: string
  auto_close?: string
  days: Num
  forward: Num
  discount: Num
  rate: Num
  rate_fitted: boolean
  rate_source?: "parity" | "term" | "curve" | "assumed" | null
  rate_curve_symbol?: string | null
  deamericanized?: boolean | null
  dividends?: { ex_date: string; amount: number }[]
  atm_iv: Num
  gex: Num
  vex: Num
  strikes: number
  style?: "european" | "american" | null
  coverage?: Coverage | null
}

export interface ExposureSummary {
  gex: Num
  vex: Num
  gamma_flip: Num
  call_wall: Num
  put_wall: Num
  oi_coverage?: Num
}

export interface Summary {
  symbol: string
  spot: Num
  spot_source?: SpotSource
  as_of: string | null
  version: number
  compute_ms: Num
  exposure: ExposureSummary
  expiries: Expiry[]
  american_approximation?: boolean | null
  coverage?: Coverage | null
}

export type QuoteIssue = "no_quote" | "no_bid" | "no_ask" | "crossed" | "zero_size"
export interface OptionQuote {
  // Absent on servers predating paper trading.
  symbol?: string
  bid_size?: Num
  ask_size?: Num
  tradable?: boolean
  untradable_reason?: string | null
  /** Whether paper orders can fill on this displayed quote; absent on older servers. */
  executable?: boolean
  /** Why they cannot, when executable is false. */
  quote_issue?: QuoteIssue | null
  eep?: Num
  bid: Num
  ask: Num
  mid: Num
  iv: Num
  bid_iv: Num
  ask_iv: Num
  delta: Num
  gamma: Num
  vega: Num
  theta: Num
  vanna: Num
  oi: Num
  /** Cumulative traded contracts for this trading date; null if unknown, absent on older servers. */
  volume?: Num
  vendor_iv: Num
}

export interface ChainRow {
  strike: number
  iv: Num
  gex: Num
  vex: Num
  call: OptionQuote | null
  put: OptionQuote | null
}

export interface Chain {
  symbol: string
  spot: Num
  spot_source?: SpotSource
  as_of: string | null
  version: number
  expiry: Expiry
  strikes: ChainRow[]
}

export interface ExposureMatrix {
  symbol: string
  spot: Num
  spot_source?: SpotSource
  as_of: string | null
  version: number
  strikes: number[]
  expiries: { id: string; expiry: string; days: Num; gex: Num[]; vex: Num[] }[]
  total_gex: Num[]
  exposure: ExposureSummary
}

export interface SviFit {
  a: number
  b: number
  rho: number
  m: number
  sigma: number
  rmse_vol_points: number
  points: number
  status: "ok"
  reason: string | null
  fit_ms: number
  butterfly_min_g: Num
  butterfly_k: Num
  butterfly_ok: boolean
}

export interface SsviFit {
  rho: Num
  eta: Num
  gamma: Num
  rmse_vol_points: Num
  status: "ok" | "too_few_points" | "failed"
  reason: string | null
  monotone_adjusted: boolean
  fit_ms: number
}

export interface SurfaceExpiry {
  id: string
  expiry: string
  days: Num
  forward: Num
  atm_iv: Num
  points: SmilePoint[]
  ssvi_theta?: Num
  ssvi_rmse_vol_points?: Num
  ssvi_reason?: string | null
  ssvi_min_k?: Num
  ssvi_max_k?: Num
  svi?: SviFit | null
  svi_status?: "ok" | "too_few_points" | "failed"
  svi_reason?: string | null
  svi_points?: number
  svi_fit_ms?: number
  svi_years?: Num
  svi_min_k?: Num
  svi_max_k?: Num
}

export interface SmilePoint {
  strike: number
  k: Num
  iv: Num
  bid_iv: Num
  ask_iv: Num
  svi_iv?: Num
  ssvi_iv?: Num
}

export interface Surface {
  symbol: string
  spot: Num
  spot_source?: SpotSource
  as_of: string | null
  version: number
  expiries: SurfaceExpiry[]
  ssvi?: SsviFit
  calendar_violations?: { earlier: string; later: string; k: number; vol_points?: number; tolerance_vol_points?: number }[]
}

/** The replay running beside the live feed: its recording, speed and clock. */
export interface ReplayState {
  imported?: boolean
  id?: string
  scenario?: string | null
  seed?: string | null
  generator?: number | null
  date?: string
  start_at?: string
  plan?: string
  durable?: boolean
  /** Preparing the start state: playing unpaced up to start_at. */
  fast_forwarding?: boolean
  /** A lockstep step (PUT until) is playing; writes wait for it. Absent from older servers. */
  stepping?: boolean
  skip_pending?: boolean
  pause_at?: string | null
  restarted_from?: { id: string; at: string }
  settled_through?: string | null
  progress?: number
  file: string
  /** The demo market: a simulated day rather than a recording of real quotes. */
  demo?: boolean
  provider: string
  symbols: string[]
  started: string | null
  delay_seconds: number
  speed: number
  paused: boolean
  finished: boolean
  /** The replay's clock: when the latest event it played was first received. */
  time: string | null
  /** A scenario run's sessions in order; empty for a recording. Absent from older servers. */
  sessions?: ReplaySession[]
  /** A scenario run's last snapshot; null for a recording. Absent from older servers. */
  end?: string | null
}
/** One session of a scenario run: its kind, trading date, and first and last snapshot. */
export interface ReplaySession { session: "regular" | "curb" | "overnight"; date: string; open: string; end: string }
export interface ReplayRecording {
  imported?: boolean
  simulated?: boolean
  file: string
  bytes: number
  provider?: string
  symbols?: string[]
  started?: string | null
  delay_seconds?: number
  error?: string
}
/** A simulated day the demo market plays, in these symbols. */
export interface ReplayDemo { goal?: string; session?: "regular" | "overnight"; date?: string; seed?: string; generator?: number; id?: string; title?: string; description?: string; provider: string; symbols: string[]; started: string; sessions?: ReplaySession[]; end?: string }
export interface RunInput {
  kind: "scenario" | "recording"
  version: number
  sha256: string
  date: string
  started: number
  id?: string
  name?: string
  size?: number
  builtin?: boolean
  generator?: number
  /** Scenario output revision: 8 freezes ETF prices at the stock close while options trade on; older runs keep their saved revision. */
  revision?: number
  seed?: string
}
export interface VerificationRun {
  id: string
  file: string
  inputs: RunInput[]
  plan: string
  recorded_journal?: RunJournal
}
export interface VerificationCost {
  estimated_seconds: number
  warning: string | null
}
export interface RunVerification {
  cost?: VerificationCost
  status: "idle" | "running" | "passed" | "failed"
  message: string
  progress?: number
  transactions?: number
  equity?: string | null
  head?: string | null
  build?: string
  time?: string | null
  finished_at?: string
  run?: VerificationRun
}
export interface RunJournal {
  transactions: number
  head: string
  bytes: number
}
export interface ReplayHistory extends ReplayState {
  journal_size?: import("./trading-types").JournalSize
  verification_cost?: VerificationCost
  verification?: RunVerification
  journal?: RunJournal
  journal_found?: RunJournal
  torn?: boolean
  bytes_cut?: number
  truncated?: boolean
  mismatch?: boolean
  integrity_message?: string
  id: string
  /** The plan's display name; plan is its id. Absent from older servers. */
  plan_id?: string | null
  plan_name?: string
  result: "pass" | "fail" | "open"
  pnl: string | null
  valuation_complete?: boolean
  error?: string
  /** A crash interrupted the run: POST /api/replay {resume} continues it. Absent from older servers. */
  interrupted?: boolean
  read_only: true
}
export interface ReplayListing {
  write?: "open" | "token" | "disabled"
  history?: ReplayHistory[]
  directory: string
  recordings: ReplayRecording[]
  /** The default demo day, and every one; absent from older servers. */
  demo?: ReplayDemo | null
  demos?: ReplayDemo[]
  replay: ReplayState | null
}

export interface Tick {
  trading?: TradingStatus | null
  accounts?: AccountBrief[] | null
  /** Replay ticks carry the replay's state. */
  replay?: ReplayState
  type: "tick" | "replay_tick"
  feed: { state: FeedState; message: string }
  underlyings: UnderlyingSnapshot[]
  engine: EngineMetrics
  market?: MarketSession | null
  circuit_breaker?: CircuitBreaker | null
}

export type CandleInterval = "1m" | "5m" | "15m" | "30m" | "1h" | "1d"
/** One OHLC bar; t is its start in Unix seconds of market-data time. */
export interface Candle { t: number; o: number; h: number; l: number; c: number }
export interface Candles { symbol: string; interval: CandleInterval; bars: Candle[] }

/** A price band, such as one or two standard deviations of a probability cone. */
export interface PriceRange { low: number; high: number }
/** Odds of finishing above or below a price by a horizon, or touching it on the way. */
export interface PriceOdds { price: number; above: number | null; below: number | null; touch: number | null }
export interface ProbabilityHorizon {
  days: number
  /** When the horizon ends: today's regular close for 0 days. */
  until: string
  /** Standard deviation of the log price to `until`, from the implied term structure. */
  sigma: number | null
  one_sd: PriceRange | null
  two_sd: PriceRange | null
  prices: PriceOdds[]
}
export interface Probability { symbol: string; as_of: string; spot: number | null; horizons: ProbabilityHorizon[]; model: string }

/** Current snapshot metrics. Every vol, RR/BF and slope is in vol points. */
export interface ConstantVol {
  days: number
  variance: number | null
  vol: number | null
  near: string | null
  next: string | null
  truncated: boolean
  proxy: boolean
  reason: string | null
}
export interface DeltaSkew {
  call: { strike: number | null; vol: number | null; reason: string | null }
  put: { strike: number | null; vol: number | null; reason: string | null }
  rr: number | null
  bf: number | null
}
export interface RealizedEstimate {
  vol: number | null
  fallback: boolean
  reason: string | null
}
export interface VolCone {
  sessions: number
  days_used: number
  observations: number
  min: number | null
  p10: number | null
  p25: number | null
  p50: number | null
  p75: number | null
  p90: number | null
  max: number | null
  current: number | null
  current_percentile: number | null
  implied_days: number
  implied_vol: number | null
  implied_proxy: boolean
  implied_reason: string | null
}
export interface Volatility {
  iv_rank?: Num
  iv_percentile?: Num
  history_sessions?: number
  history_basis?: { current: "own_mfiv" | "own_atm" | "missing"; own_sessions: number; proxy_sessions: number; window: number }
  proxy?: { name: string | null; used: boolean; start: string | null }
  history_values?: { date: string; value: number; source: string }[]
  ex_post_vrp?: ExPostVrp | null
  symbol: string
  as_of: string
  version: number
  spot: number | null
  forward: number | null
  units: "vol_points"
  mfiv: {
    expiries: { id: string; minutes: number; variance: number | null; vol: number | null;
      k0: number | null; low: number | null; high: number | null; strikes: number;
      lower_stop: "window" | "zero_bids"; upper_stop: "window" | "zero_bids";
      truncated: boolean; proxy: boolean; eep: string; reason: string | null }[]
    constant: ConstantVol[]
  }
  atm: {
    expiries: { id: string; minutes: number; vol: number | null; source: string;
      slope: number | null; curvature: number | null; reason: string | null }[]
    constant: ConstantVol[]
  }
  skew: {
    delta_convention: string
    expiries: { id: string; source: string; delta25: DeltaSkew; delta10: DeltaSkew;
      slope: number | null; curvature: number | null }[]
    days: number
    delta25: DeltaSkew
    delta10: DeltaSkew
    proxy: boolean
    reason: string | null
  }
  term: { mfiv9_30: number | null; mfiv30_93: number | null; atm30_7: number | null; truncated: boolean; proxy: boolean }
  realized: {
    daily_as_of: string | null
    windows: { sessions: number; close_to_close: RealizedEstimate; parkinson: RealizedEstimate;
      garman_klass: RealizedEstimate; yang_zhang: RealizedEstimate }[]
    cones: VolCone[]
    intraday: { date: string; vol: number | null; returns: number; observed_minutes: number;
      session_minutes: number; partial: boolean; reason: string | null }[]
    today: number | null
  }
  vrp: { spread: number | null; ratio: number | null; truncated: boolean; proxy: boolean; reason: string | null }
  implied_moves: {
    intervals: { from: string; to: string; forward_variance: number | null; sessions: number;
      calendar_arbitrage: boolean; proxy: boolean; truncated: boolean; reason: string | null }[]
    sessions: { date: string; points: number | null; percent: number | null; forward: number | null;
      shared: boolean; calendar_arbitrage: boolean; proxy: boolean; truncated: boolean;
      label: string | null; reason: string | null }[]
    today_points: number | null
    today_percent: number | null
    today_reason: string | null
    today_proxy: boolean
    today_truncated: boolean
    today_calendar_arbitrage: boolean
  }
  sources: Record<"mfiv" | "atm" | "skew" | "term" | "realized" | "vrp" | "implied_moves", string>
}

export interface VolatilitySeriesRow {
  t: number
  sample_time?: number
  mfiv30?: Num
  atm30?: Num
  rr25?: Num
  rv21?: Num
  proxy_iv30?: Num
  sources: Record<string, string | null>
}
export interface VolatilitySeries {
  symbol: string
  interval: "1m" | "1d"
  fields: string[]
  rows: VolatilitySeriesRow[]
}
export interface ExPostVrp {
  proxy: boolean
  implied_source: string
  realized_source: string
  convention: string
  points: { date: string; end: string; implied: Num; realized: Num; variance: Num; vol_points: Num; source: string }[]
  summaries: { years: number; observations: number; proxy_observations: number; from: string; to: string;
    mean_variance: Num; median_variance: Num; mean_vol_points: Num; median_vol_points: Num; positive_share: Num }[]
}
