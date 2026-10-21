# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Core wire types from docs/openapi.yaml. Results remain ordinary dictionaries.

Unknown fields are preserved. Money and identifiers stay strings, not floats.
"""
from typing import Any, Literal, TypedDict

JSON = dict[str, Any]


class RuleEvidence(TypedDict, total=False):
    underlying: str
    order_dollar_delta: float
    held_dollar_delta: float
    other_account: str
    contract: str
    contracts: int
    volume: int | None
    percent: int


class Decision(TypedDict, total=False):
    """Numeric checks, HH:MM New York clock rules, or UTC news timestamps."""

    code: str
    message: str
    actual: float | str | None
    limit: float | str | None
    scope: str | None
    evidence: RuleEvidence


class EvaluationObjective(TypedDict):
    code: Literal["PROFIT_TARGET", "MIN_TRADING_DAYS", "MIN_PROFITABLE_DAYS", "CONSISTENCY", "TRADE_CONSISTENCY", "MIN_TRADES", "MICROSCALPING"]
    met: bool
    actual: float | None
    required: float
    message: str


class BestTrade(TypedDict):
    id: str
    pnl: str


# Known plan/reset reasons; generic wire codes remain open to other reasons.
PlanReason = Literal[
    "PROFIT_TARGET", "DRAWDOWN_FLOOR", "DAILY_LOSS_LIMIT",
    "MIN_TRADING_DAYS", "MIN_PROFITABLE_DAYS", "CONSISTENCY",
    "MAX_CONTRACTS_HELD", "STOP_REQUIRED", "MAX_TRADE_RISK",
    "TIME_LIMIT", "INACTIVITY", "INSTRUMENT_NOT_ALLOWED",
    "OUTSIDE_PLAN_HOURS", "FLAT_TIME", "OVERNIGHT_HOLD", "SCALING_LIMIT",
    "TRADE_CONSISTENCY", "MIN_TRADES", "MIN_HOLD", "MICROSCALPING", "HEDGING", "COUNTER_POSITION", "MAX_VOLUME_SHARE",
    "NEWS_BLACKOUT", "HOLD_RESTRICTED",
]
# Compatibility name for the complete plan-reason set.
PlanRuleReason = PlanReason
ResetReason = Literal["PLAN_LOCKED", "RESET_LIMIT"]


class Provider(TypedDict):
    name: str
    realtime: bool
    delay_seconds: int
    simulated: bool


class Status(TypedDict):
    provider: Provider
    underlyings: list[JSON]
    feed: JSON
    engine: JSON


class Summary(TypedDict):
    symbol: str
    spot: float | None
    as_of: str | None
    version: int
    expiries: list[JSON]
    exposure: JSON


class Chain(TypedDict):
    symbol: str
    spot: float | None
    as_of: str | None
    version: int
    expiry: JSON
    strikes: list[JSON]


class Surface(TypedDict):
    symbol: str
    as_of: str | None
    expiries: list[JSON]


class Walk(TypedDict):
    step: str
    seconds: int
    limit: str


class WalkStep(TypedDict):
    time: str
    limit_price: str


class OrderWalk(TypedDict, total=False):
    walk: Walk | None
    next_walk: WalkStep | None


class Order(OrderWalk):
    id: str
    client_order_id: str
    actor: str
    symbol: str | None
    quantity: int
    filled_quantity: int
    limit_price: str | None
    time_in_force: str
    good_till: str | None
    limit_ticks: int | None
    status: str


class FeeSchedule(TypedDict):
    open: str
    close: str
    leg_cap: str
    clearing: str
    regulatory: str
    index: dict[str, str]
    exercise: str


class FillFees(TypedDict):
    commission: str
    clearing: str
    regulatory: str
    index: str


class Fill(TypedDict):
    id: str
    order_id: str
    actor: str
    symbol: str
    quantity: int
    price: str
    fee: str
    fees: FillFees | None
    time: str
    quote: JSON | None


class Orders(TypedDict):
    account_version: str
    orders: list[Order]


class Fills(TypedDict):
    account_version: str
    fills: list[Fill]


class OrderResult(TypedDict):
    account_version: str
    order: Order


class Settlements(TypedDict):
    account_version: str
    settlements: list[JSON]


class Trades(TypedDict):
    account_version: str
    attempt: int
    trades: list[JSON]


class Portfolio(TypedDict):
    account_version: str
    time: str
    cash: str
    equity: str
    positions: list[JSON]


class PayoutOptions(TypedDict, total=False):
    consistency_percents: list[int]
    buffer: str
    buffer_payouts: int


class PayoutRules(PayoutOptions):
    qualifying_profit: str
    qualifying_days: int
    withdrawal_percent: int
    split_percent: int
    minimum: str
    caps: list[str]


class PayoutBestDay(TypedDict):
    day: str
    profit: str


class PayoutStatus(TypedDict):
    eligible: bool
    blocked: JSON | None
    number: int
    active: bool
    flat: bool
    qualifying_days: int
    required_days: int
    qualifying_profit: str
    profit: str
    withdrawable: str
    cap: str | None
    maximum: str
    minimum: str
    trader_share: str
    withdrawal_percent: int
    split_percent: int
    consistency_percent: int | None
    cycle_profit: str
    best_day: PayoutBestDay | None
    consistency_needed: str
    buffer_balance: str | None


class SizeScaling(TypedDict):
    profit_percent: int
    payouts: int
    days: int
    increase_percent: int
    max_balance: str


class SizeScale(TypedDict):
    day: str
    old: str
    size: str


class SizeScalingStatus(TypedDict):
    size: str
    original: str
    original_max_drawdown: str
    original_daily_loss_limit: str
    max_balance: str
    period_started: str
    period_days: int
    days_required: int
    period_profit: str
    profit_required: str
    period_payouts: int
    payouts_required: int
    next_size: str
    history: list[SizeScale]


class ScalingStep(TypedDict):
    profit: str
    contracts: int


class ScalingStatus(TypedDict):
    limit: int
    held: int
    profit: str
    next: ScalingStep | None


class _PlanEvent(TypedDict):
    kind: Literal["news", "earnings", "ex_dividend", "split"]
    time: str


class PlanEvent(_PlanEvent, total=False):
    symbol: str
    session: Literal["before_open", "after_close"]
    label: str


class EventWindow(TypedDict):
    kind: Literal["news", "weekend", "earnings", "ex_dividend", "split"]
    symbol: str | None
    label: str | None
    start: str
    end: str
    active: bool


class AccountRules(TypedDict, total=False):
    events: list[PlanEvent]
    news_before_minutes: int
    news_after_minutes: int
    news_action: Literal["block", "flatten"]
    hold_restrictions: list[Literal["weekend", "earnings", "ex_dividend", "split"]]
    hold_cutoff: str
    time_limit_days: int  # Evaluation and verification only; funded requires zero.
    inactivity_days: int
    underlyings: list[str]
    trading_start: str | None
    trading_end: str | None
    flat_time: str | None
    no_overnight: bool
    plan_id: str | None
    plan: str | None
    phase: str
    evaluation_fee: str
    reset_fee: str
    activation_fee: str
    max_resets: int
    profit_target: str | None
    max_drawdown: str | None
    drawdown_mode: str
    lock_balance: str | None
    lock_at_start: bool
    profit_basis: str
    daily_loss_limit: str | None
    daily_loss_basis: str
    daily_loss_action: str
    consistency_percent: int
    consistency_basis: str
    min_trading_days: int
    min_profitable_days: int
    profitable_day_profit: str | None
    day_end: str
    buy_only: bool
    defined_risk: bool
    min_hold_seconds: int
    microscalp_seconds: int
    microscalp_percent: int
    min_trades: int
    trade_consistency_percent: int
    max_volume_percent: int
    no_hedging: bool
    no_counter_positions: bool
    max_contracts_held: int
    require_stop_loss: bool
    max_trade_risk: str | None
    max_trade_risk_percent: int
    slippage_ticks: int
    fill_latency_ms: int
    impact_ticks: int
    inside_fill_percent: int
    margin: str
    account_type: str
    house_margin_percent: int
    pm_vol_shock: int
    pin_risk: bool
    pin_risk_seed: int
    cash_credit_rate_bps: int
    margin_debit_rate_bps: int
    stock_borrow_rate_bps: int
    buying_power: bool
    expiry_cutoff_seconds: int
    payouts: PayoutRules | None
    scaling: list[ScalingStep]
    size_scaling: SizeScaling | None
    fees: FeeSchedule


class AccountDamage(TypedDict):
    reason: str
    last_good_seq: int
    last_good_time: str | None


class JournalSize(TypedDict):
    bytes: int
    records: int
    warning: str | None


class AccountStorage(TypedDict, total=False):
    damaged: AccountDamage | None
    journal_size: JournalSize


class EvaluationProgress(TypedDict, total=False):
    next_event: EventWindow | None
    active_events: list[EventWindow]
    day_low_equity: str | None
    day_high_equity: str | None
    day_low_at: str | None
    day_high_at: str | None
    closest_floor: str | None
    closest_floor_at: str | None
    decision_code: str | None
    balance: str
    profit_basis: str
    objectives: list[EvaluationObjective]
    trading_days: int | None
    profitable_days: int
    best_day: JSON | None
    best_trade: BestTrade | None
    closed_trades: int | None
    short_profit: str | None
    consistency_target: str | None
    daily_loss: JSON | None
    day_lock: str | None
    day_locked_at: str | None
    exit_equity: str
    exit_cost: str
    liquidated_equity: str | None
    liquidation_cost: str | None
    time_limit_days: int | None
    deadline: str | None
    days_left: int | None
    last_activity: str | None
    inactive_days: int | None
    inactivity_deadline: str | None
    flat_time: str | None
    flat_now: bool
    scaling: ScalingStatus | None
    size_scaling: SizeScalingStatus | None


class Evaluation(EvaluationProgress):
    enabled: bool
    attempt: int
    status: str
    started: str
    starting_balance: str
    equity: str
    marked: bool
    valuation_complete: bool
    profit: str
    peak: str
    floor: str | None
    drawdown_buffer: str | None
    target_equity: str | None
    target_remaining: str | None
    decided_at: str | None
    decided_equity: str | None
    decision: str | None
    day: str
    day_open_equity: str
    day_close_equity: str
    days: list[JSON]
    floor_locked: bool
    qualifying_days: int
    cycle_started: str
    payouts: list[JSON]


class ProgramCosts(TypedDict):
    evaluation: str
    reset: str
    activation: str
    total: str
    resets_used: int
    resets_left: int | None
    payouts_received: str
    net: str
    fee_charged: str
    fee_kind: str | None


class CashFlow(TypedDict):
    id: str
    attempt: int
    time: str
    day: str
    kind: Literal["deposit", "withdrawal", "credit_interest", "debit_interest", "borrow_fee"]
    symbol: str | None
    amount: str
    basis: str
    rate_bps: int
    days: int
    client_id: str | None


class CashFlowHistory(TypedDict, total=False):
    cash_flows: list[CashFlow]
    cash_transfers_allowed: bool


class Account(AccountStorage, CashFlowHistory):
    account_version: str
    time: str
    rules: AccountRules
    evaluation: Evaluation
    costs: ProgramCosts
    next_plans: list[str]
    payout: PayoutStatus | None


class ReplayListing(TypedDict):
    directory: str
    recordings: list[JSON]
    replay: JSON | None


class RunVerification(TypedDict, total=False):
    status: str
    message: str
    progress: float
    transactions: int
    equity: str | None
    head: str | None
    build: str
    time: str | None
    finished_at: str
    run: JSON


class ReplayFeedback(TypedDict, total=False):
    settled_through: str
    aborted: bool
    message: str


class ReplayResult(ReplayFeedback):
    replay: JSON | None


# The server also accepts "open_interest", the deprecated spelling of long_calls_short_puts.
ExposureConvention = Literal["long_calls_short_puts", "short_all", "customer_flow"]
ExposureSlice = Literal["all", "0dte", "ex_0dte"]


class ExposureLevels(TypedDict):
    flip: float | None
    positive_wall: float | None
    negative_wall: float | None


class ExposureMetricLevels(TypedDict):
    gex: ExposureLevels
    vex: ExposureLevels
    dex: ExposureLevels
    cex: ExposureLevels
    vomex: ExposureLevels


class ExposureSummary(TypedDict):
    gex: float | None
    vex: float | None
    dex: float | None
    cex: float | None
    vomex: float | None
    gamma_flip: float | None
    call_wall: float | None
    put_wall: float | None
    oi_coverage: float | None
    available: bool
    unavailable_reason: str | None
    levels: ExposureMetricLevels


class ExposureExpiry(TypedDict):
    id: str
    expiry: str
    days: float | None
    gex: list[float | None]
    vex: list[float | None]
    dex: list[float | None]
    cex: list[float | None]
    vomex: list[float | None]


class Exposure(TypedDict):
    symbol: str
    as_of: str | None
    spot: float | None
    spot_source: Literal["quote", "parity"] | None
    version: int
    convention: ExposureConvention
    slice: ExposureSlice
    strikes: list[float]
    expiries: list[ExposureExpiry]
    total_gex: list[float | None]
    total_vex: list[float | None]
    total_dex: list[float | None]
    total_cex: list[float | None]
    total_vomex: list[float | None]
    exposure: ExposureSummary


class HistoryRank(TypedDict):
    current: float | None
    rank: float | None
    percentile: float | None
    z_score: float | None
    sessions: int
    window: int


class VolatilityHistoryFields(TypedDict, total=False):
    iv_rank: float | None
    iv_percentile: float | None
    iv_z_score: float | None
    history_ranks: dict[str, HistoryRank]
    history_sessions: int
    history_basis: JSON
    proxy: JSON
    history_values: list[JSON]
    ex_post_vrp: JSON | None
    event_volatility: list[JSON]


class Volatility(VolatilityHistoryFields):
    symbol: str
    as_of: str
    units: str
    mfiv: JSON
    atm: JSON
    skew: JSON
    realized: JSON
    vrp: JSON
    implied_moves: JSON


class Candles(TypedDict):
    symbol: str
    interval: str
    bars: list[JSON]


class RiskScopes(TypedDict, total=False):
    scopes: dict[str, JSON]


class Risk(RiskScopes):
    account_version: str
    limits_revision: str
    limits: JSON
    complete: bool
    daily_loss: str
    kill: JSON
    aggregate: JSON
    underlyings: dict[str, JSON]
    scenarios: JSON


class RiskProfileOptions(TypedDict, total=False):
    term_scaled: bool


class RiskProfile(RiskProfileOptions):
    account_version: str
    time: str
    underlying: str | None
    benchmark: str | None
    reference: str
    spot: float | None
    vol_points: float
    equity: str
    room: str | None
    soft_room: str | None
    percent: list[float]
    prices: list[float | None]
    curves: list[JSON]
    betas: list[JSON]
    weighted_dollar_delta: float | None
    weighted_delta: float | None
    complete: bool
    model: str


class Probability(TypedDict):
    symbol: str
    as_of: str
    spot: float | None
    horizons: list[JSON]
    model: str


class OrderPreview(TypedDict):
    trade_risk: str | None
    trade_risk_limit: str | None
    trade_risk_basis: str | None
    next_walk: WalkStep | None
    account_version: str
    decision: str
    reason: Decision | None
    buying_power: JSON
    max_loss: str | None
    max_units: int | None
    max_units_buying_power: int | None
    max_units_floor: int | None
    breach: JSON
    execution: JSON
    liquidity: list[JSON]
    warnings: list[JSON]
    fee: str | None
    fees: FillFees | None
    simulated: bool


class WhatIfResult(TypedDict):
    account_version: str
    current: JSON
    candidates: list[JSON]
    simulated: bool


class StockPreview(TypedDict):
    account_version: str
    simulated: bool
    decision: str
    reason: Decision | None
    price: str | None
    cost: str | None
    current: JSON
    after: JSON


class FlattenPreview(TypedDict):
    account_version: str
    decision: str
    reason: Decision | None
    cancelled_orders: list[str]
    orders: list[JSON]
    fills: list[JSON]
    stock_fills: list[JSON]
    kept_stocks: list[JSON]
    remaining: list[JSON]
    remaining_shares: list[JSON]
    current: JSON | None
    after: JSON | None
    simulated: bool


class SubmitResult(OrderResult):
    fills: list[Fill]


class Plans(TypedDict):
    plans: list[JSON]


class Accounts(TypedDict):
    accounts: list[JSON]


class EquityPaging(TypedDict, total=False):
    next: str | None
    error_time: str | None
    error_market_time: str | None
    error_recovered: bool


class EquityHistory(EquityPaging):
    samples: list[JSON]
    error: str | None


class CancelAllResult(TypedDict):
    account_version: str
    cancelled_orders: list[str]


class FlattenResult(CancelAllResult):
    orders: list[Order]
    fills: list[Fill]
    stock_fills: list[JSON]
    kept_stocks: list[JSON]
    residuals: list[JSON]


class HistoricalRisk(TypedDict):
    account_version: str
    time: str
    stress: list[JSON]
    sticky_strike: JSON
    atm_vol_change: JSON
    model: str


class ScenarioAnalysis(TypedDict):
    account_version: str
    time: str
    mode: str
    benchmark: str
    betas: list[JSON]
    term_scaled: bool
    slices: list[JSON]
    complete: bool
    model: str
