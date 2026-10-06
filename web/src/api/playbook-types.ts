import type { NewOrder } from "./trading-types"
import type { StrategyTemplate } from "../lib/strategy"
export type TechnicalCondition = { interval: "minute" | "day"; period: number } & (
  | { indicator: "sma" | "ema"; direction: "above" | "below" }
  | { indicator: "rsi"; min: number; max: number }
  | { indicator: "bollinger"; direction: "above" | "below"; k: number }
)
export interface Playbook {
  id: string; version?: number; name: string; description: string; underlyings: string[]
  window: { start: string; end: string; weekdays: number[] }
  conditions: {
    price?: { reference: "level" | "prior_close" | "day_open"; direction: "above" | "below"; value: number }
    technical?: TechnicalCondition[]; vix?: { min: number; max: number }; gap?: { min_percent: number; max_percent: number }
    iv_rank?: { min: number; max: number }; vrp_min?: number; term_inverted?: boolean; dte?: { min: number; max: number }
  }
  structure: { template: StrategyTemplate; expiry: { min: number; max: number } }
  sizing: { units: number } | { floor_share: number }
  management: { take_profit_percent?: number; stop_credit_multiple?: number; stop_underlying?: { direction: "above" | "below"; level: number }; close_by: string; max_hold_days?: number; max_days_in_trade?: number; close_at_dte?: number; stop_loss_percent?: number; trailing_stop?: { percent: number } }
  guardrails: { max_entries_per_day: number; cooldown_minutes: number }
}
export interface PlaybookStats {
  trades: number; win_rate: number | null; average_win: string | null; average_loss: string | null
  expectancy: string | null; profit_factor: number | null; no_losses: boolean; average_r: number | null; adherence: number | null
  /** Mean return on buying power of the closed trades; absent from older servers. */
  average_return_on_buying_power?: number | null
}
export interface PlaybookTrade {
  order: string; version: number; tag: string; opened: string; closed: string | null; net: string
  rules: Record<string, boolean | null>; followed: boolean; r: number | null
  /** The buying power its entry needed, and net over it once closed; absent from older servers. */
  buying_power?: string; return_on_buying_power?: number | null
}
export interface StagedOrder {
  /** Preview advice; absent from older servers. */
  warnings?: { code: string; message: string }[]
  /** HTTP order body; absent from older servers. */
  order?: NewOrder
  /** Legacy internal journal encoding, unsuitable for HTTP submission. */
  request?: Record<string, unknown>
  id: string; playbook: string; version: number; name: string; underlying: string; units: number; net: string
  management?: Playbook["management"]; max_loss: string | null; max_loss_basis: string; close_by: string; simulated: true
  legs: { symbol: string; side: string; ratio: number; strike: number; type: string; expiry: string }[]
}
export interface PlaybookReport {
  all: PlaybookStats; followed: PlaybookStats; deviated: PlaybookStats; trades: PlaybookTrade[]
}
export interface ForwardTestWindow {
  account: string; playbook: string; version: number; started: string; ended: string | null; actor: string
  first_order: string; end_order: string | null
}
export interface ForwardTest {
  windows: ForwardTestWindow[]
  /** Statistics are included on GET; older servers omit forward_tests entirely. */
  running?: boolean; days_running?: number; entries?: number; time_stops?: number; rejected_entries?: number
  report?: PlaybookReport; versions?: Record<string, PlaybookReport>
}
export interface PlaybooksResponse {
  definitions: Record<string, { versions: Playbook[]; deleted: boolean }>
  modes: Record<string, "off" | "stage" | "auto">; auto_allowed: boolean; staged: StagedOrder[]; reasons: Record<string, string>
  reports?: Record<string, PlaybookReport>
  forward_tests?: Record<string, ForwardTest>
}
export interface PassOdds {
  pass: number; fail: number; neither: number; median_days_to_pass: number | null; historical_days: number; seed: string
  days: number; samples: number; label: string; history_basis: string; path_assumption: string; simulated: true
}
