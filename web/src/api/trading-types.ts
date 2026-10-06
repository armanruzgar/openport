import type { RunJournal, RunVerification } from "./types"
import type { Num, PriceRange } from "./types"

/** Exact decimal dollars, including request bodies. Responses keep 2–6 decimals; round only for display. */
export type Money = string
/** open: writes need no token (one sent is still checked); token: writes need one; disabled: none can write. */
export type WriteMode = "open" | "token" | "disabled"
export type EvaluationStatus = "active" | "passed" | "failed"
export interface TradingStatus {
  enabled: boolean
  reason: string | null
  account_version: string
  kill_latched: boolean
  write: WriteMode
  fee_per_contract?: Money
  initial_cash?: Money
  /** Active rules' display name; absent on older servers, null without a plan. */
  plan_id?: string | null
  plan?: string | null
  /** Null without a target or drawdown rule. */
  evaluation?: EvaluationStatus | null
}
/** Funded-account withdrawals: a payout needs `qualifying_days` days of at least
 * `qualifying_profit` net realised profit since the last one. */
export interface PayoutRules {
  qualifying_profit: Money
  qualifying_days: number
  /** Share of profit one payout may take. */
  withdrawal_percent: number
  /** Trader's share of each payout. */
  split_percent: number
  minimum: Money
  /** Per payout number; the last repeats; empty is uncapped. */
  caps: Money[]
  /** Best-day limit by payout number; last repeats; empty disables. */
  consistency_percents?: number[]
  /** Equity to retain above start; zero disables. */
  buffer?: Money
  /** First N payouts; zero means every payout. */
  buffer_payouts?: number
}
export type AccountType = "margin" | "cash" | "ira"
export interface SizeScaling {
  profit_percent: number
  payouts: number
  days: number
  increase_percent: number
  max_balance: Money
}
export interface SizeScale {
  day: string
  old: Money
  size: Money
}
export interface SizeScalingStatus {
  size: Money
  original: Money
  original_max_drawdown: Money
  original_daily_loss_limit: Money
  max_balance: Money
  period_started: string
  period_days: number
  days_required: number
  period_profit: Money
  profit_required: Money
  period_payouts: number
  payouts_required: number
  next_size: Money
  history: SizeScale[]
}
export interface ScalingStep { profit: Money; contracts: number }
export interface ScalingStatus { limit: number; held: number; profit: Money; next: ScalingStep | null }
export type HoldRestriction = "weekend" | "earnings" | "ex_dividend" | "split"
/** News times are UTC with trailing fractional zeros omitted; significant nanoseconds are retained. */
export interface PlanEvent { kind: "news" | "earnings" | "ex_dividend" | "split"; time: string; symbol?: string; session?: "before_open" | "after_close"; label?: string }
export interface EventWindow { kind: PlanEvent["kind"] | "weekend"; symbol: string | null; label: string | null; start: string; end: string; active: boolean }
export interface AccountRules {
  events?: PlanEvent[]
  news_before_minutes?: number
  news_after_minutes?: number
  news_action?: "block" | "flatten"
  hold_restrictions?: HoldRestriction[]
  hold_cutoff?: string
  plan_id?: string | null
  plan: string | null
  phase: "evaluation" | "verification" | "funded"
  evaluation_fee?: Money
  reset_fee?: Money
  activation_fee?: Money
  max_resets?: number
  profit_target: Money | null
  max_drawdown: Money | null
  /** intraday and end_of_day floors trail; a static one stays at the starting balance less max_drawdown. */
  drawdown_mode: "intraday" | "end_of_day" | "static"
  /** The trailing floor stops once it reaches this balance. */
  lock_balance: Money | null
  /** The trailing floor locks at the attempt's starting balance. Fields to day_end are absent from older servers. */
  lock_at_start?: boolean
  /** Marked equity, or the closed balance (cash plus the positions' cost), for the target, consistency and profitable days. */
  profit_basis?: ProfitBasis
  daily_loss_limit?: Money | null
  daily_loss_basis?: DailyLossBasis
  /** lock: flatten and refuse opening orders until the next trading day; fail: fail the attempt. */
  daily_loss_action?: "lock" | "fail"
  /** The best day may be at most this percent of the basis; 0 is off. */
  min_hold_seconds?: number
  microscalp_seconds?: number
  microscalp_percent?: number
  min_trades?: number
  trade_consistency_percent?: number
  consistency_percent?: number
  consistency_basis?: "total" | "positive_days"
  min_trading_days?: number
  min_profitable_days?: number
  profitable_day_profit?: Money | null
  /** "HH:MM" New York time the plan's trading day ends, 16:15 to 24:00. */
  day_end?: string
  /** Calendar days; zero disables. The time limit applies to evaluation and verification; funded requires zero. */
  time_limit_days?: number
  inactivity_days?: number
  /** By underlying, so SPX includes SPXW options; empty allows all. */
  underlyings?: string[]
  /** HH:MM New York; both null or a start-inclusive, end-exclusive window. */
  trading_start?: string | null
  trading_end?: string | null
  /** Mandatory close time in New York, before day_end. */
  flat_time?: string | null
  no_overnight?: boolean
  buy_only: boolean
  /** Every short option needs a long of its type expiring with it or later; absent from older servers. */
  defined_risk?: boolean
  /** Older servers use zero slippage and strategy margin. */
  slippage_ticks?: number
  fill_latency_ms?: number
  impact_ticks?: number
  inside_fill_percent?: number
  /** Absent under the flat fee and on older servers. */
  fees?: FeeSchedule
  margin?: "strategy" | "portfolio"
  /** What the account may hold; absent from older servers (a margin account). */
  account_type?: AccountType
  /** A broker's house margin in percent on top of Reg T, or of the portfolio scan. */
  house_margin_percent?: number
  /** Portfolio margin's implied-volatility shock, in points up and down. */
  pm_vol_shock?: number
  /** Held options plus working opening contracts, excluding shares; 0 disables. */
  /** Acceptance checks full size; each fill checks its proposed contracts against current volume. */
  max_volume_percent?: number
  no_hedging?: boolean
  /** Includes other live accounts’ held and working opening directions; rechecked at fill. */
  no_counter_positions?: boolean
  max_contracts_held?: number
  require_stop_loss?: boolean
  /** Per-order risk before fees; null/zero disables. */
  max_trade_risk?: Money | null
  /** Percent of equity minus the plan floor; 0 disables, ignored without a floor. */
  max_trade_risk_percent?: number
  buying_power: boolean
  expiry_cutoff_seconds: number
  payouts: PayoutRules | null
  /** Gross option contracts held; next session uses closed-balance profit. Empty disables. */
  scaling?: ScalingStep[]
  size_scaling?: SizeScaling | null
}
export type ProfitBasis = "equity" | "balance"
export type DailyLossBasis = "equity" | "balance" | "higher" | "peak"
/** Plan objectives, decisions and trading restrictions in evaluation and verification. */
export type PlanReason = "PROFIT_TARGET" | "DRAWDOWN_FLOOR" | "DAILY_LOSS_LIMIT" | "MIN_TRADING_DAYS" | "MIN_PROFITABLE_DAYS" | "CONSISTENCY" | "MAX_CONTRACTS_HELD" | "STOP_REQUIRED" | "MAX_TRADE_RISK" | "TIME_LIMIT" | "INACTIVITY" | "INSTRUMENT_NOT_ALLOWED" | "OUTSIDE_PLAN_HOURS" | "FLAT_TIME" | "OVERNIGHT_HOLD" | "SCALING_LIMIT" | "TRADE_CONSISTENCY" | "MIN_TRADES" | "MIN_HOLD" | "MICROSCALPING" | "HEDGING" | "COUNTER_POSITION" | "MAX_VOLUME_SHARE" | "NEWS_BLACKOUT" | "HOLD_RESTRICTED"
/** Compatibility name for the complete plan-reason set. */
export type PlanRuleReason = PlanReason
/** held_dollar_delta is the compared held book or working entry for COUNTER_POSITION. */
export type RuleEvidence = { underlying: string; order_dollar_delta: number; held_dollar_delta: number; other_account?: string }
  | { contract: string; contracts: number; volume: number | null; percent: number }
/**
 * Generic reason codes include plan/reset refusals and PLAYBOOK_TIME_STOP,
 * PLAYBOOK_TRAILING_STOP, PLAYBOOK_DTE_STOP and PLAYBOOK_DAYS_IN_TRADE_STOP
 * cancellations (older runs use USER_CANCEL). Numeric evidence compares actual
 * against limit, with the underlying or aggregate scope; absent on older servers
 * and null when a check has none. Structured evidence is omitted when inapplicable.
 */
/** Clock rule evidence uses HH:MM New York strings; news uses UTC timestamps. */
export interface Decision { evidence?: RuleEvidence; code: string; message: string; actual?: number | string | null; limit?: number | string | null; scope?: string | null }
/** `requirement` is `short_requirement` under a name that fits portfolio margin too; absent on older servers. */
export interface BuyingPower { available: Money; reserved: Money; short_requirement: Money; requirement?: Money }
/**
 * P&L explained by the Greeks, in dollars to the micro-dollar (older servers: to the cent): each stretch a
 * position is held at one size, split by its Greeks at the start. `other` is what they leave unexplained;
 * `costs` are the spread paid against the mark at each fill, and fees.
 */
export interface Attribution {
  delta: number
  gamma: number
  vega: number
  theta: number
  other: number
  costs: number
  total: number
  /** Some of it is `other` because a stretch moved without valuations at both ends; absent from older servers. */
  fallback?: boolean
}
/** Every round trip in a holding this attempt, the open one included. */
export interface Lifetime { round_trips: number; realised: Money; fees: Money; net: Money }
/** A contract or shares traded today and no longer held, with the day's P&L by Greek. */
export interface ClosedAttribution { symbol: string; kind: "option" | "shares"; underlying: string; attribution: Attribution }
export interface EvaluationDay {
  low_equity?: Money | null
  high_equity?: Money | null
  low_at?: string | null
  high_at?: string | null
  day: string
  open_equity: Money
  close_equity: Money
  peak: Money
  floor: Money | null
  /** Net realised P&L of the day, after fees. */
  realised: Money
  /** Funded accounts: the day counted toward a payout. */
  qualifying: boolean
  /** The day's P&L by Greek; absent from older servers. */
  attribution?: Attribution
  /** The day's profit under the plan's basis. Fields to locked are absent from older servers. */
  profit?: Money
  profitable?: boolean
  /** Executions of your own orders; null unless a minimum-days rule counts them. */
  executions?: number | null
  /** The plan limit that locked the day, such as DAILY_LOSS_LIMIT. */
  locked?: string | null
}
/** One condition a pass needs; `actual` and `required` are dollars, days or percent by code. */
export interface Objective {
  code: "PROFIT_TARGET" | "MIN_TRADING_DAYS" | "MIN_PROFITABLE_DAYS" | "CONSISTENCY" | "TRADE_CONSISTENCY" | "MIN_TRADES" | "MICROSCALPING" | string
  met: boolean
  actual: number | null
  required: number
  message: string
}
export interface DailyLossStatus {
  limit: Money
  basis: DailyLossBasis
  action: "lock" | "fail"
  /** What today's limit is measured from. */
  reference: Money
  /** The equity that reaches it. */
  level: Money
  room: Money
}
export interface Payout {
  number: number
  time: string
  /** Trading day in progress when requested; it and later days count toward the next payout. */
  day: string
  amount: Money
  trader_share: Money
  /** Equity when requested, before the withdrawal. */
  balance: Money
}
export interface PayoutBestDay {
  day: string
  profit: Money
}
/** A funded account's standing for its next payout. */
export interface PayoutStatus {
  eligible: boolean
  /** The first unmet requirement. */
  blocked: Decision | null
  number: number
  active: boolean
  /** No positions and no working or armed orders. */
  flat: boolean
  qualifying_days: number
  required_days: number
  qualifying_profit: Money
  profit: Money
  withdrawable: Money
  cap: Money | null
  consistency_percent: number | null
  cycle_profit: Money
  best_day: PayoutBestDay | null
  consistency_needed: Money
  buffer_balance: Money | null
  /** Largest amount accepted now. */
  maximum: Money
  minimum: Money
  trader_share: Money
  withdrawal_percent: number
  split_percent: number
}
export interface Evaluation {
  next_event?: EventWindow | null
  active_events?: EventWindow[]
  time_limit_days?: number | null
  deadline?: string | null
  days_left?: number | null
  last_activity?: string | null
  inactive_days?: number | null
  inactivity_deadline?: string | null
  flat_time?: string | null
  flat_now?: boolean
  enabled: boolean
  attempt: number
  status: EvaluationStatus
  started: string
  starting_balance: Money
  equity: Money
  /** Every position has a mark; rules only decide on fully marked equity. */
  marked: boolean
  valuation_complete: boolean
  profit: Money
  peak: Money
  floor: Money | null
  drawdown_buffer: Money | null
  target_equity: Money | null
  target_remaining: Money | null
  decided_at: string | null
  decided_equity: Money | null
  decision: string | null
  day: string
  day_open_equity: Money
  day_close_equity: Money
  day_low_equity?: Money | null
  day_high_equity?: Money | null
  day_low_at?: string | null
  day_high_at?: string | null
  closest_floor?: Money | null
  closest_floor_at?: string | null
  days: EvaluationDay[]
  floor_locked: boolean
  /** Days closed since the last payout (or the start) that qualified. */
  qualifying_days: number
  cycle_started: string
  payouts: Payout[]
  /** PROFIT_TARGET, DRAWDOWN_FLOOR or DAILY_LOSS_LIMIT once decided. Fields to liquidation_cost are absent from older servers. */
  decision_code?: string | null
  /** Cash plus the positions' cost: the closed balance. */
  balance?: Money
  profit_basis?: ProfitBasis
  objectives?: Objective[]
  trading_days?: number | null
  profitable_days?: number
  short_profit?: Money | null
  closed_trades?: number | null
  best_trade?: { id: string; pnl: Money } | null
  best_day?: { day: string; profit: Money } | null
  consistency_target?: Money | null
  daily_loss?: DailyLossStatus | null
  scaling?: ScalingStatus | null
  size_scaling?: SizeScalingStatus | null
  day_lock?: string | null
  day_locked_at?: string | null
  /** Equity if every position closed now at the bid or ask, with slippage and fees. */
  exit_equity?: Money
  exit_cost?: Money
  /** A decided attempt's equity once flat, and what the liquidation cost against decided_equity. */
  liquidated_equity?: Money | null
  liquidation_cost?: Money | null
}
export interface AttemptSummary {
  fee_charged?: Money
  fee_kind?: string | null
  attempt: number
  plan_id?: string | null
  plan: string | null
  started: string
  ended: string
  starting_balance: Money
  final_equity: Money
  status: EvaluationStatus
  decision: string | null
  /** Absent from older servers. */
  decision_code?: string | null
  /** Rules and levels at reset; null in older journals, absent from older servers. */
  rules?: AccountRules | null
  decided_at?: string | null
  decided_equity?: Money | null
  peak?: Money | null
  floor?: Money | null
}
/**
 * Something about the held book worth acting on. `actual` and `limit` depend on the
 * code (docs/paper-trading.md, Risk warnings).
 * EXPIRY_DELIVERY warns whenever projected buying power is negative, including before the cutoff;
 * delivery applies only to contracts still held if a close cannot fill.
 */
export interface RiskWarning {
  code: "DELTA_LIMIT" | "VEGA_LIMIT" | "DELTA_HEADROOM" | "SOFT_FLOOR" | "SOFT_FLOOR_ROLLOVER" | "SOFT_FLOOR_UNUSED" | "FLOOR_RATCHET" | "EXPIRY_DELIVERY" | "EARLY_ASSIGNMENT" | "EX_DIVIDEND"
  severity: "warning" | "info"
  /** The underlying, or "aggregate" for the account. */
  scope: string
  symbol: string | null
  message: string
  actual: number | null
  limit: number | null
}
export interface AccountDamage {
  reason: string
  last_good_seq: number
  last_good_time: string | null
}
export interface JournalSize {
  bytes: number
  records: number
  warning: string | null
}
export interface Account {
  damaged?: AccountDamage | null
  journal_size?: JournalSize
  verification_cost?: import("./types").VerificationCost
  verification?: RunVerification
  journal?: RunJournal
  journal_found?: RunJournal
  torn?: boolean
  bytes_cut?: number
  truncated?: boolean
  mismatch?: boolean
  integrity_message?: string
  breach?: Breach
  /** Absent on older servers. */
  warnings?: RiskWarning[]
  guardrails?: Guardrails
  guardrail_state?: GuardrailState
  account_version: string
  time: string
  rules: AccountRules
  evaluation: Evaluation
  buying_power: BuyingPower
  /** Null outside the funded phase. */
  payout: PayoutStatus | null
  attempts: AttemptSummary[]
  next_plans?: string[]
  costs?: ProgramCosts
}
export interface FillContext {
  spot: Num
  spot_source: string | null
  iv: Num
  delta: Num
  years: Num
  equity: Money | null
  floor_room: Money | null
  buying_power: Money | null
}
export interface Excursion { pnl: Money; time: string; spot: Num }
export interface TradeReview {
  mae: Money | null
  mfe: Money | null
  worst: Excursion | null
  best: Excursion | null
  planned_risk: Money | null
  give_back: Money | null
  heat: Num
  r_multiple: Num
}
export interface DayNote { plan: string; review: string; time?: string | null }
export interface SettlementSource {
  kind: string
  provider?: string
  symbol?: string
  quote_time?: string
}
export interface SettlementRecord {
  symbol: string
  underlying: string
  expiry: string
  settlement: "AM" | "PM"
  value: Money
  time: string
  quantity: number
  cash: Money
  realised: Money
  fee: Money
  source: SettlementSource | null
}
export interface SettlementsResponse {
  account_version: string
  settlements: SettlementRecord[]
}
export interface Trade {
  id: string
  attempt: number
  entry_context?: FillContext | null
  exit_context?: FillContext | null
  review?: TradeReview
  strategy_id?: string | null
  strategy_review?: TradeReview | null
  /** The whole trade it is in, by its first round trip's ID (its own unless rolled, adjusted or grouped); absent from older servers. */
  group?: string
  /** The buying power its entry needed on its own, and net over it once closed; absent from older servers. */
  buying_power?: Money
  return_on_buying_power?: Num
  /** The same for its strategy's legs together; null outside a strategy. */
  strategy_buying_power?: Money | null
  strategy_return_on_buying_power?: Num
  symbol: string
  underlying: string
  expiry: string
  settlement: "AM" | "PM"
  strike: number
  type: "call" | "put"
  direction: "long" | "short"
  status: "open" | "closed"
  opened: string
  closed: string | null
  /** The engine's trading date of the close, market holidays known; null while open, absent from older servers. */
  trading_day?: string | null
  duration_seconds: number | null
  quantity: number
  max_quantity: number
  opened_contracts: number
  closed_contracts: number
  average_open: Money | null
  average_close: Money | null
  cost: Money
  gross: Money
  fees: Money
  net: Money
  /** Net over entry cost, closed trades only. */
  return: Num
  mark: Money | null
  unrealised: Money | null
  settlement_value?: Money | null
  settlement_source?: string | null
  closure: "settlement" | "reset" | "exercise" | "assignment" | "abandon" | null
  /** What closed it: the closure, or its last reducing fill's order. Null while open; absent from older servers. */
  closed_by?: TradeExit | null
  /** Why the reducer liquidated it when closed_by is "system": "target", "drawdown", "soft_floor" or "expiry". */
  system_reason?: string | null
  /** Its P&L by Greek over its life; null for one open from before they were kept, absent from older servers. */
  attribution?: Attribution | null
  fills: string[]
  /** The trader's note ("" for none) and tags; absent from older servers. */
  note?: string
  tags?: string[]
}
/** How a round trip ended: the trader's own order, a bracket exit, a flatten, a playbook's close, a reducer liquidation, or a closure. */
export type TradeExit = "order" | "stop_loss" | "take_profit" | "flatten" | "playbook" | "system" | "settlement" | "exercise" | "assignment" | "abandon" | "reset"
export interface TradeNote {
  note: string
  tags: string[]
}
export interface TradeNoteResponse extends TradeNote {
  account_version: string
  trade: string
}
/** How shares changed hands: an option's exercise or assignment, a trade that reduced them, the account closing them when an evaluation is decided, or a reset. */
export type ShareSource = "expiry_exercise" | "assignment" | "early_exercise" | "trade" | "rule" | "reset"
/** One round trip in an underlying's shares from exercise and assignment, flat to flat or still open. */
export interface ShareTrade {
  kind: "shares"
  id: string
  attempt: number
  symbol: string
  direction: "long" | "short"
  status: "open" | "closed"
  opened: string
  closed: string | null
  trading_day?: string | null
  duration_seconds: number | null
  /** Signed shares held now; zero once closed. */
  shares: number
  max_shares: number
  opened_shares: number
  closed_shares: number
  average_open: Money | null
  average_close: Money | null
  cost: Money
  gross: Money
  /** Dividends received (negative: paid while short) during the round trip; in net. Absent on older servers. */
  dividends?: Money
  fees: Money
  net: Money
  return: Num
  mark: Money | null
  unrealised: Money | null
  opened_by: ShareSource
  /** The option whose exercise or assignment opened them, or null. */
  option: string | null
  closed_by: ShareSource | null
  closing_option: string | null
  /** Its P&L by Greek over its life (all delta, dividends in other); absent from older servers. */
  attribution?: Attribution | null
  /** Stock fill IDs, "s"-prefixed ("s3"); older servers sent bare numbers. */
  fills: string[]
  /** The trader's note ("" for none) and tags, as on option trades; absent from older servers. */
  note?: string
  tags?: string[]
}
/** One change in shares held, and how it came about. */
/** buying_power_after records automatic delivery with buying-power checks; null/absent for older records and other fills. */
export interface StockFill { id: string; symbol: string; shares: number; price: Money; time: string; source: ShareSource; option: string | null; buying_power_after?: Money | null }
/** A dividend paid on (negative: charged to short) shares held into its ex-date. */
export interface DividendPaid { symbol: string; ex_date: string; per_share: Money; shares: number; amount: Money; time: string }
/** A trade with more than one entry: a roll, an adjustment or round trips the account grouped. */
export interface WholeTrade {
  id: string
  attempt: number
  underlying: string
  status: "open" | "closed"
  opened: string
  closed: string | null
  trading_day: string | null
  /** Its round trips' trade IDs, in the order they opened. */
  round_trips: string[]
  /** The orders that opened them. */
  entries: number
  gross: Money
  fees: Money
  net: Money
  /** The open round trips' unrealized P&L; null when closed or a mark is missing. */
  unrealised: Money | null
  /** The most buying power its round trips needed at once, and net over it once closed; absent from older servers. */
  buying_power?: Money
  return_on_buying_power?: Num
  /** Its whole-trade review: from the first entry, or from `review_since` when the account grouped it later. */
  review: TradeReview | null
  review_since: string | null
}
export interface TradesResponse {
  account_version: string
  attempt: number
  trades: Trade[]
  /** Whole trades with more than one entry, newest first; absent from older servers. */
  groups?: WholeTrade[]
  share_trades?: ShareTrade[]
  day_notes?: Record<string, DayNote>
  /** Every change in shares and every dividend, oldest first; absent from older servers. */
  stock_fills?: StockFill[]
  dividends?: DividendPaid[]
  /** The replay run these trades come from; null for a live account, absent from older servers. */
  run?: RunIdentity | null
}
/** A replay run: its id and what it replayed (a scenario and seed, or a recording). */
export interface RunIdentity { id: string | null; scenario: string | null; seed: string | null; recording: string | null; date: string | null }
export interface ProgramCosts {
  evaluation: Money
  reset: Money
  activation: Money
  total: Money
  resets_used: number
  resets_left: number | null
  payouts_received: Money
  net: Money
  fee_charged: Money
  fee_kind: string | null
}
export interface ProgramCostSettings {
  evaluation_fee?: Money
  reset_fee?: Money
  activation_fee?: Money
  max_resets?: number
}
/** Reset refusals use HTTP 409, with attempted reset count and configured limit. */
export type ResetReason = "RESET_LIMIT" | "PLAN_LOCKED"
export interface Plan {
  phase?: AccountRules["phase"]
  unlocks?: string[]
  id: string
  name: string
  summary: string
  initial_cash: Money
  rules: AccountRules
  /** Funded plans: the evaluation plan ID whose pass unlocks them. */
  unlocked_by: string | null
}
export interface PlansResponse { plans: Plan[] }
export type FillModel = "as_displayed" | "conservative" | "midpoint"
export type FeeModel = "flat" | "itemized"
export type AccountRulesInput = Pick<AccountRules, "profit_target" | "max_drawdown" | "drawdown_mode" | "buy_only" | "buying_power" | "expiry_cutoff_seconds">
  & Partial<Omit<AccountRules, "fees">> & { fees?: Partial<FeeSchedule> | null }
/** The account's margin as its broker sets it, beside a plan or custom rules. */
export interface MarginModel {
  margin?: "strategy" | "portfolio"
  account_type?: AccountType
  house_margin_percent?: number
  pm_vol_shock?: number
}
export type ResetRequest = { reason: string; fill_model?: FillModel; fee_model?: FeeModel } & MarginModel & ProgramCostSettings & ({ plan: string } | { initial_cash: Money; rules: AccountRulesInput })
export type Side = "buy" | "sell"
/**
 * Option triggers compare the order's executable side, or its mid or mark; underlying ones
 * compare spot, or another underlying's price when they name `symbol`; study ones an
 * underlying's iv30 or iv7 (vol points) or term_ratio; time ones the New York time of day
 * `at` (level zero).
 */
export interface Trigger {
  source: "option" | "underlying" | "combo" | "study" | "time"
  direction: "at_or_below" | "at_or_above"
  /** Where it activates; a trailing trigger's moves as it trails. */
  level: Money
  symbol?: string
  study?: TriggerStudy
  at?: string
  /** Default bid_ask; absent from older servers. */
  reference?: TriggerReference
  /** A trailing stop's distance; null or absent when it does not trail. */
  trail?: Trail | null
}
export type TriggerStudy = "iv30" | "iv7" | "term_ratio"
/** What an option or combo trigger reads: the executable side, the mid or the mark. Underlying triggers read spot. */
export type TriggerReference = "bid_ask" | "mid" | "mark"
/** A trailing distance: dollars or a percentage (decimal strings), or whole ticks. */
export type Trail = { unit: "amount" | "percent"; value: Money } | { unit: "ticks"; value: number }
/** A time trigger is sent without a level: `{source: "time", at: "15:30"}`. */
export type TriggerRequest = Trigger | { source: "time"; at: string; direction?: "at_or_below" | "at_or_above" }
/** A trigger (a stop: market when reached), a limit (a resting take-profit), or both (a stop-limit: a GTC limit once reached). */
export type ExitSpec = { trigger: Trigger; limit_price?: Money } | { limit_price: Money; trigger?: Trigger }
export interface Bracket { stop_loss?: ExitSpec; take_profit?: ExitSpec }
/** One leg of a multi-leg order: `ratio` contracts per unit. */
export interface OrderLeg { symbol: string; side: Side; ratio: number }
/** What an account alert watches: a contract, a spread's net mark, an underlying or the account. */
export type AlertScope = "contract" | "spread" | "underlying" | "account"
export interface AlertRequest {
  label?: string
  scope: AlertScope
  metric: string
  symbol?: string | null
  legs?: OrderLeg[] | null
  direction: "at_or_below" | "at_or_above"
  level: Money
  repeat?: boolean
}
/** An alert the server keeps with the account, checks after each market batch and forwards to its channels. */
export interface Alert {
  id: string
  label: string
  scope: "contract" | "spread" | "underlying" | "account"
  metric: string
  symbol: string | null
  legs: OrderLeg[] | null
  direction: "at_or_below" | "at_or_above"
  level: Money
  repeat: boolean
  created_at: string
  actor: string
  armed: boolean
  fired: number
  fired_at: string | null
  value: Money | null
}
export interface AlertsResponse { account_version: string; alerts: Alert[] }
export interface AlertResponse { account_version: string; alert: Alert }
export interface AlertDeleted { account_version: string; deleted: string }
export type TimeInForce = "day" | "gtc" | "ioc" | "exto" | "gtc_exto" | "gtd"
type Pricing = { type: "limit"; limit_price: Money; time_in_force: TimeInForce }
  | { type: "market"; time_in_force: "ioc" | "exto" | "gtc_exto" | "gtd"; limit_price?: never }
export type FlattenPricing = { type?: "market" | "limit"; limit_ticks?: number }
export interface Walk { step: Money; seconds: number; limit: Money }
export interface WalkStep { time: string; limit_price: Money }
/** Orders chained to an order: placed once it fills completely (then), or accepted with it, the first fill of either cancelling the other (oco). */
export interface OrderChain { then?: ChainedOrder; oco?: ChainedOrder }
/** An order body chained to another: it takes the other's client order ID with a suffix, and is never a held exit. */
export type ChainedOrder = NewOrder extends infer O ? O extends unknown ? Omit<O, "client_order_id" | "exits_only"> : never : never
export type NewOrder = { tags?: string[]; note?: string; good_till?: string; walk?: Walk | null } & OrderChain & ({
  client_order_id: string
  symbol: string
  side: Side
  quantity: number
  trigger?: TriggerRequest
  bracket?: Bracket
  /** A whole trade to join, by its ID or one of its open round trips' IDs (an adjustment). */
  group?: string
  /** Attach the bracket to a held contract instead of opening one: the order closes it. */
  exits_only?: boolean
  legs?: never
} | {
  client_order_id: string
  /** Two to four legs on one underlying, filled together. */
  legs: OrderLeg[]
  trigger?: TriggerRequest
  bracket?: Bracket
  exits_only?: boolean
  /** A whole trade to join, by its ID or one of its open round trips' IDs (an adjustment). */
  group?: string
  /** Units of the strategy. */
  quantity: number
  symbol?: never
  side?: never
}) & Pricing
/** A chained order not yet placed, in the order body's shape. */
export interface PendingOrder {
  symbol: string | null; side: Side | null; legs: OrderLeg[] | null
  type: "limit" | "market"; time_in_force: TimeInForce; quantity: number; limit_price: Money | null
  good_till: string | null; walk: Walk | null; group: string | null
  trigger: Trigger | null; bracket: unknown; then: PendingOrder | null; oco: PendingOrder | null
}
/** What keeps an open order from filling now. */
export interface OrderWait {
  code: "SESSION_CLOSED" | "TRIGGER" | "REGULAR_SESSION" | "INVALID_QUOTE" | "STALE_QUOTE" | "FILL_LATENCY" | "NEWER_QUOTE" | "LIMIT" | "DISPLAYED_SIZE" | "STALE_DATA"
  message: string
}
/** A change asked of a resting order: the terms requested (null where kept), those before it, and its outcome. */
export interface OrderChangeRecord {
  time: string
  actor: string
  quantity: number | null
  limit_price: Money | null
  trigger_level: Money | null
  /** A change of time in force; absent from older servers. */
  time_in_force?: "day" | "gtc" | "ioc" | null
  walk?: Walk | null
  previous: { quantity: number; limit_price: Money | null; trigger_level: Money | null; time_in_force?: "day" | "gtc" | "ioc" | null; walk?: Walk | null }
  applied: boolean
  reason: Decision | null
}
export interface Order {
  actor?: string
  tags?: string[]
  note?: string
  exits_only?: boolean
  /** The whole trade it joins; null for none, absent from older servers. */
  group?: string | null
  id: string
  client_order_id: string
  /** Null for a multi-leg order; see `legs`. */
  symbol: string | null
  underlying: string
  side: Side | null
  /** A multi-leg order's legs; its prices are net per unit, negative for a credit. */
  legs?: OrderLeg[] | null
  type: "limit" | "market"
  time_in_force: TimeInForce
  good_till?: string | null
  limit_ticks?: number | null
  walk?: Walk | null
  next_walk?: WalkStep | null
  quantity: number
  filled_quantity: number
  remaining_quantity: number
  limit_price: Money | null
  average_fill_price: Money | null
  status: "working" | "partially_filled" | "filled" | "cancelled" | "rejected" | "armed"
  reason: Decision | null
  accepted_at: string
  day_end: string | null
  /** When it stopped working; null while open. Absent on older servers, as are the changes. */
  ended_at?: string | null
  /** The evaluation attempt it belongs to; absent on older servers. */
  attempt?: number
  /** An open order: what keeps it from filling now, or null. */
  waiting?: OrderWait | null
  modified_at?: string | null
  changes?: OrderChangeRecord[]
  /** System orders liquidate or auto-close; absent on older servers. */
  origin?: "user" | "system"
  /**
   * A close the account works for the trader, as a flatten's are: it only reduces, shrinks
   * with its position and works on later quotes until it fills or its session ends.
   */
  reduce_only?: boolean
  trigger?: Trigger | null
  triggered_at?: string | null
  bracket?: { stop_loss: { trigger: Trigger | null; limit_price: Money | null } | null; take_profit: { trigger: Trigger | null; limit_price: Money | null } | null } | null
  role?: "stop_loss" | "take_profit" | null
  parent?: string | null
  oco?: string | null
  stop_loss_order?: string | null
  take_profit_order?: string | null
  /** The order its complete fill will place, as a body; null when none. Absent from older servers. */
  then?: PendingOrder | null
  /** The order its complete fill placed, and the order whose fill placed this one. */
  chained_order?: string | null
  chained_from?: string | null
}
/** The book a fill traded against: size_left is the taken side's displayed size still free for paper orders before it, quoted_at when the quote was first given. */
export interface FillQuote {
  observation: string
  bid: Money | null
  ask: Money | null
  bid_size: number
  ask_size: number
  size_left: number
  quoted_at: string
  age_seconds: number
}
/** Commission is capped per leg per order across partial fills; zero is uncapped.
 * Other charges apply per contract; index fees use the option root. */
export interface FeeSchedule {
  open: Money
  close: Money
  leg_cap: Money
  clearing: Money
  regulatory: Money
  index: Record<string, Money>
  exercise: Money
}
export interface FillFees { commission: Money; clearing: Money; regulatory: Money; index: Money }
export interface Fill {
  actor?: string
  context?: FillContext | null
  /** Null for fills recorded before the book was kept; absent on older servers. */
  quote?: FillQuote | null
  /** Null under flat fees; absent on older servers. Adds up to fee. */
  fees?: FillFees | null
  id: string
  order_id: string
  /** The attempt it belongs to; absent from older servers. */
  attempt?: number
  symbol: string
  underlying: string
  side: Side
  quantity: number
  price: Money
  fee: Money
  quote_time: string
  time: string
}
export interface Position {
  symbol: string
  underlying: string
  expiry: string
  settlement: "AM" | "PM"
  /** When it expires and awaits settlement, and when it last trades (the business day before, for AM settlement); absent on older servers. */
  expiry_time?: string
  last_trade_time?: string
  strike: number
  type: "call" | "put"
  quantity: number
  average_price: Money
  basis: Money
  mark: Money | null
  mark_age_seconds: Num
  market_value: Money | null
  unrealised: Money | null
  realised: Money
  fees: Money
  fresh: boolean
  awaiting_settlement: boolean
  /** While awaiting settlement: on the recorded closing print, or by a value entered by hand (AM series, or no print arrived). */
  settle_by?: "closing_print" | "opening_print" | "manual" | null
  /** It cannot be sold now (its quote shows only an ask, or it has expired), so a long can be abandoned; absent on older servers. */
  no_bid?: boolean
  /** A long option instructed not to be exercised at expiry; absent on older servers. */
  do_not_exercise?: boolean
  greeks: { delta: Num; gamma: Num; vega: Num; theta: Num; dollar_delta: Num; dollar_gamma_1pct: Num; vega_dollars: Num; theta_dollars: Num }
  /** Today's P&L by Greek for this contract; null until its first fill or rollover on this server. */
  attribution?: Attribution | null
  /** The round trip in progress (its realised and fees are this row's) and every round trip in the contract this attempt; absent from older servers. */
  trade?: string | null
  lifetime?: Lifetime | null
}
/** Shares of an underlying, delivered by exercise and assignment. */
export interface StockHolding {
  symbol: string
  shares: number
  average_price: Money
  basis: Money
  mark: Money | null
  mark_time: string | null
  market_value: Money | null
  unrealised: Money | null
  realised: Money
  fees: Money
  fresh: boolean
  attribution: Attribution | null
  trade?: string | null
  lifetime?: Lifetime | null
}
export interface Portfolio {
  account_version: string
  time: string
  cash: Money
  equity: Money
  start_of_day_equity: Money
  day_pnl: Money
  realised: Money
  unrealised: Money
  fees: Money
  valuation_complete: boolean
  quality_flags: string[]
  positions: Position[]
  /** Absent from older servers. */
  stocks?: StockHolding[]
  buying_power?: BuyingPower
  /** The requirement by underlying with what holds it; absent from older servers. */
  margin?: MarginUnderlying[]
  /** Today's P&L by Greek; absent from older servers. */
  attribution?: Attribution
  /** Current quotes whose displayed size this account's orders have taken some of; absent from older servers. */
  liquidity_used?: LiquidityUsed[]
  /** Contracts and shares traded today and no longer held; absent from older servers. */
  closed?: ClosedAttribution[]
  /** Held positions by the whole trade each is in; absent from older servers. */
  strategies?: HeldStrategy[]
}
/** An OSI symbol, or the underlying for shares, with the signed contracts or shares a part takes. */
export interface MarginLeg { symbol: string; quantity: number }
/** Positions allocated together; worst_loss uses same-expiry shorts and same-or-later longs. */
export interface MarginPart {
  kind: "naked" | "vertical" | "covered" | "straddle" | "short_shares" | "protected_shares" | "worst_loss" | "long" | "cash_secured"
  legs: MarginLeg[]
  requirement: Money
}
/** Where a portfolio-margin scan loses most, and the contract minimum under it. */
export interface MarginScan { loss: Money; spot_percent: number; vol_points: number; minimum: Money }
/** Strategy margin's parts add up to the requirement; portfolio margin's scan sets it. */
export interface MarginUnderlying {
  underlying: string; requirement: Money; parts: MarginPart[]; scan: MarginScan | null
  /** Exact optimum, conservative work-cap fallback, or an older recorded allocator. */
  allocation?: "exact" | "bounded" | "legacy" | null
}
export interface LiquidityUsed { symbol: string; bid_size: number; ask_size: number; bid_left: number; ask_left: number }
/** A whole trade still held: its open legs, and every round trip of it in its realized P&L and fees. */
export interface HeldStrategy {
  id: string
  underlying: string
  opened: string
  legs: { symbol: string; quantity: number; trade: string }[]
  round_trips: number
  entries: number
  realised: Money
  fees: Money
  unrealised: Money | null
  net: Money | null
}
/** The trade each named round trip is in after grouping or ungrouping. */
export interface GroupResponse { account_version: string; groups: Record<string, string> }
export interface RiskCaps { dollar_delta: number; vega: number }
export interface Limits {
  max_order_contracts: number
  price_band_absolute: Money
  price_band_relative: number
  aggregate: RiskCaps
  per_underlying: RiskCaps
  underlying_overrides?: Record<string, RiskCaps>
  max_daily_loss: Money
  max_quote_age_seconds: number
  max_valuation_age_seconds: number
}
export interface Bucket {
  dollar_delta: Num
  dollar_gamma_1pct: Num
  vega: Num
  theta: Num
  reachable: { delta_low: Num; delta_high: Num; vega_low: Num; vega_high: Num }
  limits: RiskCaps
  delta_utilisation: Num
  vega_utilisation: Num
}
/** One change of the kill latch; previous is the reason it replaced, cleared or released. */
export interface KillChange { time: string; action: "trip" | "reset" | "release"; reason: string | null; previous: string | null; actor: string }
export interface KillState {
  latched: boolean
  reason: string | null
  /** Why POST /api/risk/kill action=reset cannot clear it; account reset separately preserves daily discipline and re-checks the soft floor. Absent on older servers, as is history. */
  reset_blocked?: Decision | null
  /** Actual latch changes; retaining the same latch on account reset adds no reset entry. */
  history?: KillChange[]
}
export interface Scenarios {
  spot_percent: number[]
  vol_points: number[]
  pnl: Num[][]
  clamped: boolean[][]
  complete: boolean
}
export interface Risk {
  time?: string
  breach?: Breach
  /** Absent on older servers. */
  warnings?: RiskWarning[]
  guardrails?: Guardrails
  guardrail_state?: GuardrailState
  pending_limits?: Limits | null
  pending_guardrails?: Guardrails | null
  pending_applied_day?: string | null
  pending_applied_at?: string | null
  pending_requires_reset?: boolean
  pending_effective?: "next_trading_day" | null
  account_version: string
  limits_revision: string
  limits: Limits
  complete: boolean
  daily_loss: Money
  kill: KillState
  aggregate: Bucket
  underlyings: Record<string, Bucket>
  scenarios: Scenarios
}
export interface OrdersResponse { account_version: string; orders: Order[] }
export interface FillsResponse { account_version: string; fills: Fill[] }
export interface OrderResponse { account_version: string; order: Order }
export interface SubmitOrderResponse extends OrderResponse { fills: Fill[] }
/** New terms for a resting order; omitted fields keep their value. */
/** `time_in_force` switches a resting limit order between DAY and GTC; absent from older servers' accepted fields. */
export interface OrderChange { quantity?: number; limit_price?: Money; trigger_level?: Money; time_in_force?: "day" | "gtc"; walk?: Walk | null }
export interface CancelAllResponse { account_version: string; cancelled_orders: string[] }
/** POST /api/orders/cancel: every open order, one underlying's, or the listed orders together. */
export type CancelRequest = { underlying?: string; orders?: never } | { orders: string[]; underlying?: never }
export interface AccountListItem { damaged?: AccountDamage | null; journal_size?: JournalSize; id: string; name: string; archived?: boolean; sandbox_idle_seconds?: number; trading: TradingStatus; equity: Money | null }
export interface AccountsResponse { accounts: AccountListItem[] }
export type CreateAccountRequest = { name: string; copy_settings_from?: string; fill_model?: FillModel; fee_model?: FeeModel } & MarginModel & ProgramCostSettings & ({ plan: string } | { initial_cash: Money; rules: AccountRulesInput })
export interface CreateAccountResponse { account: { id: string; name: string; account_version: string; plan: string | null; plan_id?: string | null; equity: Money } }
/** Delivered shares a flatten could not close, as held after it, and why. */
export interface KeptStock { symbol: string; shares: number; reason: Decision }
/** A position a flatten left open: contracts still held, those still being worked, and why the rest are not. */
export interface FlattenResidual {
  symbol: string
  underlying: string
  /** Signed contracts still held. */
  quantity: number
  /** Contracts its reduce-only closes are still working on later quotes. */
  working: number
  reason: Decision | null
}
/**
 * Each closing order with its outcome; a rejected one carries its reason. The
 * share fields are absent from older servers.
 */
export interface ClosePositionsResponse extends CancelAllResponse {
  orders: Order[]
  fills: Fill[]
  /** Shares it closed. */
  stock_fills?: StockFill[]
  /** Shares it could not close. */
  kept_stocks?: KeptStock[]
  /** Positions still open after it; absent on older servers. */
  residuals?: FlattenResidual[]
}
export interface KillResponse { account_version: string; kill: KillState; cancelled_orders: string[] }
export interface SettlementResponse { account_version: string; position_closed: boolean }

export interface Guardrails {
  soft_floor: Money
  /** New requests accept 0–99; stored settings may still contain 100. */
  soft_floor_percent: number
  /** An order counts at its first opening fill only, even when later partial fills cross rollover. */
  max_opening_trades: number
  cooldown_loss: Money
  /** Market minutes after an adverse-direction closing trigger, trailing stop or bracket stop-loss fills. */
  cooldown_minutes: number
  profit_lock: Money
}
export interface GuardrailState {
  opening_trades: number
  latched: string[]
  cooldown_until: string | null
  cooldown_seconds: number
  soft_floor: Money | null
}
export interface BreachLevel { points: number; percent: number; touch_probability: number | null }
export interface Breach {
  room: Money | null
  soft_room: Money | null
  complete: boolean
  model: string
  underlyings: {
    underlying: string; spot: number; complete: boolean; close_sigma: number | null; down: BreachLevel | null; up: BreachLevel | null
    /** Levels at the personal soft floor; the same as down/up without a plan floor. Absent on older servers. */
    soft_down?: BreachLevel | null; soft_up?: BreachLevel | null
  }[]
}
/** Where a risk-profile curve reaches a floor: the reference's move and price, and the odds of touching it by the curve's horizon. */
export interface ProfileLevel { percent: number; points: number | null; price: number | null; touch_probability: number | null }
export interface ProfileHorizon {
  /** When touch odds and the cone are measured to: today's close for today's curve, else the curve's date. */
  until: string
  sigma: number | null
  one_sd: PriceRange | null
  two_sd: PriceRange | null
}
export interface ProfileCurve {
  days: number
  /** "expiry" for the first held expiry's curve. */
  label: string | null
  time: string
  /** By move; analytical dollars from the book's value now. Null while incomplete. */
  pnl: (number | null)[]
  clamped: boolean
  horizon: ProfileHorizon
  down: ProfileLevel | null
  up: ProfileLevel | null
  soft_down: ProfileLevel | null
  soft_up: ProfileLevel | null
}
export type BetaSource = "reference" | "given" | "index" | "daily" | "intraday"
export interface ProfileBeta {
  underlying: string
  beta: number | null
  source: BetaSource | null
  observations: number
  correlation: number | null
  dollar_delta: number | null
  weighted_dollar_delta: number | null
}
export interface RiskProfile {
  account_version: string
  time: string
  underlying: string | null
  benchmark: string | null
  /** The symbol whose moves the curves are drawn against. */
  reference: string
  spot: number | null
  vol_points: number
  equity: Money
  room: Money | null
  soft_room: Money | null
  percent: number[]
  prices: (number | null)[]
  curves: ProfileCurve[]
  betas: ProfileBeta[]
  weighted_dollar_delta: number | null
  weighted_delta: number | null
  complete: boolean
  model: string
}
export interface RiskProfileQuery { underlying?: string; benchmark?: "SPY" | "SPX"; days?: (number | "expiry")[]; iv?: number; range?: number; steps?: number; betas?: Record<string, number> }
/** A block of contracts at one price, as a fill would trade it. */
export interface PreviewFill { symbol: string; side: Side; quantity: number; price: Money }
/** What submitting the order now would execute (a private dry run), and its full size block by block. */
export interface PreviewExecution {
  status: Order["status"]
  filled_quantity: number
  remaining_quantity: number
  reason: { code: string; message: string } | null
  fills: PreviewFill[]
  average_fill_price: Money | null
  schedule: PreviewFill[]
  average_price: Money | null
}
export interface OrderPreview {
  trade_risk?: Money | null
  trade_risk_limit?: Money | null
  trade_risk_basis?: "stop_loss" | "expiry_payoff" | "unbounded_or_unknown" | null
  next_walk?: WalkStep | null
  account_version: string
  decision: string
  reason: Decision | null
  /** working: available while the order works, before it fills; absent on older servers. */
  buying_power: { required: Money; before: Money; working?: Money | null; after: Money | null }
  exposure_change: { dollar_delta: number; dollar_gamma_1pct: number; vega: number; theta: number } | null
  max_loss: Money | null
  max_loss_basis: "expiry_payoff" | "scenario_grid" | null
  equity_at_max_loss: Money | null
  breaches_floor: boolean | null
  breaches_soft_floor: boolean | null
  /** Units that fit buying power, the limits and the floor share; null when sizing is unavailable. */
  max_units: number | null
  /** Constraint binding max_units; null when unavailable, absent on older servers. */
  max_units_basis?: "floor" | "buying_power" | "limits" | null
  /** Units that fit buying power and the limits, floor room aside. Absent on older servers. */
  max_units_buying_power?: number | null
  /** Units whose loss fits the floor share; null without a plan or soft floor. Absent on older servers. */
  max_units_floor?: number | null
  breach: Breach
  /** Full remaining size's fees; null without a projection, absent on older servers. */
  fee?: Money | null
  fees?: FillFees | null
  /** Absent on older servers. */
  execution?: PreviewExecution
  /** Each leg's quote; absent on older servers. */
  liquidity?: PreviewLiquidity[]
  /** Advice on accepted terms that are rarely meant; absent on older servers. */
  warnings?: OrderWarning[]
  simulated: true
}
/** An account as POST /api/orders/what-if projects it, measured from today's equity. */
export interface WhatIfAccount {
  equity: Money
  buying_power: Money
  exposure: { dollar_delta: number | null; dollar_gamma_1pct: number | null; vega: number | null; theta: number | null } | null
  /** The worst loss on the spot × volatility grid from today's equity, trading costs included. */
  max_loss: Money | null
  equity_at_max_loss: Money | null
  breaches_floor: boolean | null
  breaches_soft_floor: boolean | null
  /** Each cell's P&L from today's equity, spot-major. */
  scenarios: { spot_percent: number[]; vol_points: number[]; pnl: (number | null)[][]; complete: boolean }
  breach: Breach
}
export interface WhatIfCandidate {
  name: string
  decision: string
  reason: Decision | null
  orders: { decision: string; reason: Decision | null }[]
  /** Null when an order could not be projected (no contract or quote). */
  after: WhatIfAccount | null
}
/** POST /api/positions/close/preview: a flatten's dry run on a private copy of the account. */
export interface StockPreview {
  account_version: string
  simulated: true
  decision: string
  reason: Decision | null
  price: Money | null
  cost: Money | null
  current: WhatIfAccount
  after: WhatIfAccount
}
export interface FlattenPreview {
  account_version: string
  decision: string
  reason: Decision | null
  cancelled_orders: string[]
  /** Its closing orders as the current quotes would leave them, without IDs. */
  orders: { symbol: string | null; underlying: string; side: Side | null; legs: OrderLeg[] | null; quantity: number; filled_quantity: number; average_fill_price: Money | null
    status: Order["status"]; reason: Decision | null }[]
  fills: { symbol: string; side: Side; quantity: number; price: Money; fee: Money }[]
  stock_fills: { symbol: string; shares: number; price: Money }[]
  kept_stocks: KeptStock[]
  /** What would still be held in scope, signed. */
  remaining: { symbol: string; underlying: string; quantity: number }[]
  remaining_shares: { symbol: string; shares: number }[]
  current: WhatIfAccount | null
  after: WhatIfAccount | null
  simulated: true
}
export interface WhatIfResponse { account_version: string; current: WhatIfAccount; candidates: WhatIfCandidate[]; simulated: true }
/** The quote a previewed leg takes: size_left is what this account's orders have left of its displayed size. */
export interface PreviewLiquidity {
  symbol: string
  side: Side
  contracts: number
  executable: boolean
  reason: { code: string; message: string } | null
  displayed: number
  size_left: number
}
export interface OrderWarning { code: "STOP_AS_LIMIT" | "STOP_REACHED" | "TARGET_REACHED" | "TRIGGER_REACHED" | "SLIPPAGE_BAND" | (string & {}); message: string }
export interface EquitySample {
  time: string
  day: string
  attempt: number
  equity: Money
  floor: Money | null
  peak: Money
  target: Money | null
  tomorrow_floor: Money | null
  fill: string | null
}
export interface EquityHistory {
  samples: EquitySample[]
  error: string | null
  error_time?: string | null
  error_market_time?: string | null
  error_recovered?: boolean
  next?: string | null
}

export interface UpdateAccountRequest { name?: string; archived?: boolean }
export interface UpdateAccountResponse { account: { id: string; name: string; archived: boolean } }
export interface DeleteAccountResponse { deleted: string }
