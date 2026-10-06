import { phaseFact } from "../lib/plan-rules"
import { feeScheduleText } from "../lib/fees"
import { useState, type ReactNode } from "react"
import { useLive } from "../api/live"
import { useAccount, usePlans, useRisk } from "../api/trading"
import type { Account, Plan, TradingStatus } from "../api/trading-types"
import { PersonalRules } from "../components/PersonalRules"
import { BreachPanel } from "../components/BreachPanel"
import { ResetDialog } from "../components/ResetDialog"
import { evaluationBadge } from "../components/Sidebar"
import { TradingError } from "../components/TradingControls"
import { Empty, PageHeader, Panel } from "../components/ui"
import { lockReason, offeredPlans, payoutCap, payoutRuleFacts } from "../lib/payouts"
import { sizeScalingFact, scalingFact, clockText, dailyLossBasisText, dailyLossFact, dayEnd, floorMoves, objectiveFacts, timeRuleEntries } from "../lib/plan-rules"
import { compareMoney, formatMoney, subtractMoney } from "../lib/trading"

function Rule({ title, children }: { title: string; children: ReactNode }) {
  return <div className="border-b border-border/50 py-3 last:border-0">
    <h3 className="text-sm font-medium">{title}</h3>
    <div className="mt-1 text-sm text-muted">{children}</div>
  </div>
}

/** Plain-language rules with the account's own numbers. */
export function ruleText(account: Account, fee?: string, dailyLoss?: string) {
  const r = account.rules
  const e = account.evaluation
  const minutes = Math.round(r.expiry_cutoff_seconds / 60)
  const p = r.payouts
  const funded = r.phase === "funded"
  const balance = r.profit_basis === "balance"
  const objectives = objectiveFacts(r)
  const fixed = r.drawdown_mode === "static"
  const lockAt = r.lock_at_start ? e.starting_balance : r.lock_balance
  const end = dayEnd(r)
  return [
    { title: "Account type and margin", body: `${r.account_type === "cash" ? "Cash account" : r.account_type === "ira" ? "IRA (limited margin)" : "Margin account"}. ${r.margin === "portfolio" ? "Portfolio margin" : "Strategy margin (Reg T)"}. House margin: ${r.house_margin_percent ?? 0}%. Portfolio IV shock: ±${r.pm_vol_shock ?? 0} points${r.margin === "portfolio" ? "." : " (inactive under strategy margin)."}` },
    funded ? { title: "Funded account", body: <>There is no profit target: trade the account and withdraw from its profits under the payout rules below.
        The account stays open while its drawdown and any inactivity limit are respected.</> }
    : { title: "Profit target", body: r.profit_target
      ? <>Pass by reaching <strong className="text-foreground">{formatMoney(e.target_equity)}</strong> {balance
          ? <>on the closed balance (cash plus what your positions cost, so realised P&amp;L after fees), with every position closed: open profit counts only once you close the trade</>
          : "equity"}, {formatMoney(r.profit_target)} above your {formatMoney(e.starting_balance)} starting balance.
        {objectives.length
          ? <> The pass also waits for: {objectives.map((o) => o.charAt(0).toLowerCase() + o.slice(1)).join("; ")}. Until then, reaching the target does not end the attempt: keep trading, and protect it.</>
          : r.time_limit_days ? " There is no minimum number of trading days." : " There is no time limit and no minimum number of trading days."} Once you pass, the attempt is complete and the system tries to close positions. You can close any leftovers; opening orders require a new attempt.</>
      : "This account has no profit target." },
    { title: fixed ? "Static drawdown" : "Trailing drawdown", body: r.max_drawdown
      ? <>Equity may never touch the floor, now <strong className="text-foreground">{formatMoney(e.floor)}</strong>{fixed
          ? <>: {formatMoney(r.max_drawdown)} below your {formatMoney(e.starting_balance)} starting balance. It is static: it stays there for the whole attempt, whatever equity does.</>
          : e.floor_locked
          ? <>. It has locked at {formatMoney(lockAt)} and no longer trails.</>
          : <>: {formatMoney(r.max_drawdown)} below your highest equity ({formatMoney(e.peak)}).
            {r.drawdown_mode === "intraday" ? " The floor rises with every new equity high during the session." : " The floor rises only once a day, from each day's closing equity."} It never moves down.
            {lockAt && <> Once it reaches {r.lock_at_start ? <>your {formatMoney(lockAt)} starting balance</> : formatMoney(lockAt)} it locks there and stops trailing.</>}</>}{" "}
        Breaches are checked on every update in every mode; touching the floor {funded ? "closes the funded account" : "fails the attempt"} and starts liquidation. Closing leftover positions is still allowed.</>
      : "This account has no drawdown floor." },
    ...(r.daily_loss_limit ? [{ title: "Plan daily loss limit", body: <>
        Each trading day, equity may not touch {formatMoney(r.daily_loss_limit)} below {dailyLossBasisText[r.daily_loss_basis ?? "equity"]}
        {e.daily_loss ? <>: today <strong className="text-foreground">{formatMoney(e.daily_loss.level)}</strong>, {compareMoney(e.daily_loss.room, "0") === 1
          ? <>{formatMoney(e.daily_loss.room)} below current equity</>
          : <>which equity has reached{compareMoney(e.daily_loss.room, "0") === -1
            ? <> ({formatMoney(subtractMoney("0", e.daily_loss.room))} below it)</> : null}</>}</> : null}.
        {r.daily_loss_basis === "peak" ? " The level rises with the day's equity high, so it trails the day's gains." : null}
        {r.daily_loss_action === "fail"
          ? " Touching it fails the attempt and starts liquidation; closing orders remain allowed."
          : " Touching it closes every position, cancels your working orders and refuses opening orders until the next trading day; closing orders still work and the attempt continues."}
        {e.day_lock && <> <strong className="text-foreground">Today is locked.</strong></>}
        {" "}It is the plan's own rule, apart from your personal daily loss limit below.</> }] : []),
    ...(objectives.length ? [{ title: "Objectives to pass", body: <>
        {objectives.map((o, i) => i === 0 ? o : o.charAt(0).toLowerCase() + o.slice(1)).join("; ")}. A trading day counts once one of your own orders executes on it, or you trade shares; holding a position over a day, or an exercise, does not count.
        Days follow the plan's trading day, and a day's profit is {balance ? "what you close that day (net realised P&L after fees)" : "its change in equity"}.
        {r.min_trades || r.trade_consistency_percent ? <> Whole option trades count only when every round trip in the group is flat, including settlement, expiry, assignment and account liquidation. Net P&amp;L includes fees; rolls count once.</> : null}
        {r.trade_consistency_percent ? <> Trade consistency can hold the pass back but never fails the account. With no profitable closed trade, it is satisfied.</> : null}
        {r.microscalp_percent ? <> Microscalping sums positive net profit from completed round trips held under {r.microscalp_seconds} seconds, including protective and system exits; losses do not offset it. It can hold a pass back but never fails the account. Exactly the threshold is allowed.</> : null}
        {r.consistency_percent ? <> The consistency rule never fails the attempt: while the best day is too large a share, the pass waits, as if the target were higher{e.consistency_target ? <> (the best day so far needs {formatMoney(e.consistency_target)} of profit)</> : null}, or for more profitable days.</> : null}</> }] : []),
    ...timeRuleEntries(r),
    ...(r.size_scaling ? [{ title: "Account size scaling", body: <>{sizeScalingFact(r)}. Withdrawals count toward review profit. Each review restarts, whether it qualifies or not. Growth credits capital, leaves profit and payout room unchanged, raises the floor by the credit, and scales loss limits from the original size, rounded down to cents.</> }] : []),
    ...(r.scaling?.length ? [{ title: "Scaling plan", body: <>
        {scalingFact(r)}. Each option leg counts toward the contracts held at once; shares do not.
        {e.scaling && <> In force: {e.scaling.limit} contracts; {e.scaling.held} held; closed-balance profit {formatMoney(e.scaling.profit)}.</>}
        {" "}At the trading day's rollover, closed balance less starting balance selects the next session's limit, up or down.
        Working openings reserve capacity. Excess orders are refused with SCALING_LIMIT at acceptance and checked again at fill, with no grace period.
        A lower limit keeps existing positions; reducing orders remain allowed.
      </> }] : []),
    ...(p ? [{ title: "Payouts", body: <>
        A payout needs <strong className="text-foreground">{p.qualifying_days} qualifying days</strong> since the previous one: days that end with at least {formatMoney(p.qualifying_profit)} of net realised profit, after fees.
        Request it with no open positions or working orders. Each payout may take up to {p.withdrawal_percent}% of the profit above your {formatMoney(e.starting_balance)} starting balance,
        at least {formatMoney(p.minimum)}{p.caps.length ? <> and at most {p.caps.map((cap, i) => `${formatMoney(cap, 0)} for payout ${i + 1}${i === p.caps.length - 1 && i > 0 ? " and later" : ""}`).join(", ")}</> : null}; you keep {p.split_percent}%.
        Each finished day counts once, toward the payout cycle in progress when it closes.
        {payoutRuleFacts(p, e.starting_balance).map((fact) => <span className="block" key={fact}>{fact}.</span>)}
        {!!p.consistency_percents?.length && <span className="block">Consistency includes losses and the day in progress. The request day remains in the next cycle in full; earlier finished days reset after each payout.</span>}
        A withdrawal is not a loss: the day's starting equity {r.lock_balance ? "and a floor that has not locked yet move" : "moves"} down with it.
        {payoutCap(p.caps, e.payouts.length + 1) && <> Your next payout is number {e.payouts.length + 1}, capped at {formatMoney(payoutCap(p.caps, e.payouts.length + 1))}.</>}
      </> }] : []),
    ...(r.max_volume_percent ? [{ title: "Maximum share of option volume", body: `Acceptance and fill checks cap held plus opening contracts on each option leg at ${r.max_volume_percent}% of its current trading date’s volume. Ratios count. Resting orders that would exceed the cap cancel with MAX_VOLUME_SHARE. Unknown or stale volume refuses; pure reductions and shares are exempt. Scenario backtests support this rule with generated opening volume. Recording days need current-date option volume or the backtest request returns HTTP 400.` }] : []),
    ...(r.no_hedging ? [{ title: "No hedging", body: "Opening orders cannot oppose held dollar delta on the same underlying, including options and shares. Pure reductions and system exits remain available. Missing or stale valuations refuse an opening." }] : []),
    ...(r.no_counter_positions ? [{ title: "No counter positions", body: "Openings are checked against each other live account’s holdings and working opening orders separately. Resting orders that would oppose either direction cancel with COUNTER_POSITION at fill. Only the ordering account’s rule counts: enable it on every account you trade. Archived, replay and sandbox accounts are excluded; a single account or replay has no other accounts to check." }] : []),
    ...(r.max_contracts_held ? [{ title: "Contracts held at once", body: `At most ${r.max_contracts_held} option contracts held plus working opening contracts, counting every combo leg and its ratio. Shares and managed exits are excluded; closing orders remain available.` }] : []),
    ...(r.min_hold_seconds ? [{ title: "Minimum hold time", body: `User reductions need ${r.min_hold_seconds} seconds of market time from the first opening fill. Adding or partially closing does not restart the clock. Every closing leg must qualify. Protective triggers, bracket exits, OCO, trailing stops, flatten and system exits still execute.` }] : []),
    ...(r.require_stop_loss ? [{ title: "Stop-loss required", body: "Every opening order needs a protective bracket stop. Targets and limit prices alone do not qualify. Close the position before cancelling its required stop; flattening and automatic exits remain available. Share entries cannot attach stops." }] : []),
    ...((r.max_trade_risk && Number(r.max_trade_risk) > 0) || r.max_trade_risk_percent ? [{ title: "Maximum trade risk", body: <>
      {r.max_trade_risk && Number(r.max_trade_risk) > 0 ? <>At most {formatMoney(r.max_trade_risk)} per order. </> : null}
      {r.max_trade_risk_percent ? <>At most {r.max_trade_risk_percent}% of equity above the plan floor at entry or change; ignored without a floor. </> : null}
      When both apply, the tighter cap wins. Risk excludes fees and assumes a fill at the option or combo stop, using the entry limit or current market far side.
      Without a priced stop, including an underlying-price trigger, risk uses the bounded expiry payoff. Unbounded or unknown loss refuses. Stops can slip or remain unfilled.
    </> }] : []),
    { title: "Strategies", body: r.buy_only
      ? "Buy-only and single-leg: open positions by buying calls or puts. A sell may only close contracts you already hold, counting your other working sells. Multi-leg orders are not available. Put exercise must deliver against held long shares; exercise that would sell shares short is refused. Expiry auto-exercise and assignment still deliver shares."
      : r.defined_risk
        ? "Defined risk only: each short option needs a long of the same type on the same underlying that expires with it or later, so no position can lose without limit. Open spreads, condors and butterflies as one order from the Trade page's Strategy mode, or buy the long first; an order that would leave a short uncovered, now or once your open orders fill, is refused, and closing a short is always allowed."
        : r.account_type === "cash" ? "Cash account: buy options, write covered calls, or sell cash-secured puts."
          : r.account_type === "ira" ? "IRA: buy options, write covered calls, sell cash-secured puts, or trade covered spreads."
        : "Any strategy: buy or sell calls and puts, or place spreads, straddles, condors and butterflies of up to four legs as one order from the Trade page's Strategy mode. All legs fill together at a net debit or credit." },
    { title: "Buying power", body: <>{r.buying_power
      ? <>Orders that use buying power must fit within it, now {formatMoney(account.buying_power.available)}: {r.margin === "portfolio"
          ? "equity (cash and the positions at their marks)" : "cash"}, less working orders' reservations, less the positions' margin requirement. </>
      : <>Buying power is not enforced; cash may go negative. </>}
      {r.margin === "portfolio"
        ? <>Portfolio margin: each underlying holds its largest loss across 11 price shocks, from −8% to +6% for index products and −15% to +15% for stocks and ETFs,
          or $37.50 per standard option contract, long or short, if that is larger. {r.pm_vol_shock
            ? <>Options are repriced at implied volatility unchanged and {r.pm_vol_shock} points up and down, at unchanged time to expiry; </>
            : <>Options are repriced at unchanged volatility and time to expiry; </>}shares move with the underlying.
          Long options and shares count as collateral, so you can borrow against them.</>
        : r.account_type === "cash"
          ? <>Cash account: calls may be sold only against 100 held shares each. Short puts hold their strike × 100 per contract in cash.
            Long premium is paid in full, spreads are not netted, and short shares are prohibited.</>
          : r.account_type === "ira"
            ? <>IRA (limited margin): calls need 100 held shares or a long call on the same underlying that expires with them or later.
              Spreads net to their worst loss; unpaired short puts hold their strike × 100 per contract in cash. Long premium is paid in full,
              straddles do not pair, and short shares are prohibited.</>
            : <>Strategy margin: long premium is paid in full.
        A naked short holds its buy-back value plus 100 × max(20% of spot − out-of-the-money amount, 10% of spot or strike). Spreads are netted: a vertical holds its width,
        an iron condor its wider wing, a calendar nothing beyond its debit, and a position with a bounded worst case at expiry never more than that loss.
        Every 100 shares cover an option: a covered call holds nothing beyond its shares, a short put against short shares its buy-back value, and a long call caps 100 short shares at its strike.
        A short straddle or strangle holds its greater side plus the other side's buy-back value.</>}
      {r.house_margin_percent ? <> House margin adds {r.house_margin_percent}% {r.margin === "portfolio" ? "to each underlying's scan" : "to each naked requirement and short sale's margin"}.</> : null}
      {" "}Orders that free buying power still have to satisfy the account’s strategy and account-type rules. The Positions page shows what holds the requirement.</> },
    { title: "Expiring positions", body: minutes > 0
      ? `From ${minutes} minutes before a contract's last trade, working orders on it are cancelled, the position is closed at the bid or ask with the account's slippage, and only closing orders are accepted. Expiring index options such as SPXW and XSP last trade at 4:00 pm ET, SPY, QQQ, IWM, DIA and other ETF options at 4:15 pm, and AM-settled series at the regular close the day before.`
      : "Positions are held into expiry. Expiring index options trade until 4:00 pm ET and settle in cash; SPY, QQQ, IWM, DIA and other ETF options trade until 4:15 pm and deliver shares when a cent or more in the money at the 4:00 pm close." },
    { title: "Trading hours", body: `Every product trades in its regular session. SPX, XSP, VIX and RUT options also trade overnight, 8:15 pm to 9:25 am ET, and in the 4:15 to 5:00 pm curb session. Those sessions take limit orders only, with a condition or bracket only on a GTC limit; a day order lasts until its session ends, and stops, triggered orders, GTC limits and the account's own closing orders wait for the regular session. A trading day ends at ${end === "17:00" ? "5:00 pm" : clockText(end)} ET, so ${end === "17:00" ? "an overnight trade counts toward the next day" : "a trade after that counts toward the next trading day"}. A delayed or stalled feed refuses new orders rather than filling on stale quotes.` },
    { title: "Fills", body: <>{r.slippage_ticks
      ? <>Slippage is {r.slippage_ticks} {r.slippage_ticks === 1 ? "tick" : "ticks"}: buys pay more than the ask and sells receive less than the bid, never below zero, using the displayed price's tick size. This applies to every leg, bracket exits and automatic closes. </>
      : <>Slippage is 0 ticks: buys fill at the ask and sells at the bid. </>}
      {r.impact_ticks
        ? <>Simulated impact adds {r.impact_ticks} extra {r.impact_ticks === 1 ? "tick" : "ticks"} for each additional block of the displayed size, shared across orders on that quote. Each combo leg uses its own size. Fills wait if the resulting price or net exceeds your limit. </>
        : <>Fills use up to the displayed size. Single-leg fills stop at your limit; multi-leg orders wait if the slipped net exceeds the net limit. </>}
      {r.fill_latency_ms
        ? <>Latency is {r.fill_latency_ms} ms on market time. Orders wait for the first valid quote at or after that delay; stops start waiting when triggered. IOC orders make one attempt then cancel any remainder. Automatic closes also wait. </>
        : <>No fill latency. </>}
      {!!r.inside_fill_percent && <>Limits reaching {r.inside_fill_percent}% across the spread can fill inside at their limit, using only displayed size left; latency still applies and slippage does not worsen that price. </>}
      These models do not know queue position, hidden liquidity, or whether the market would have traded at all.
      Choose As displayed, Conservative or Inside at midpoint when starting a plan below. Midpoint enables inside fills at 50%. Conservative uses 1,000 ms latency, 1 tick of slippage and 1 extra tick per displayed-size block for practice on delayed feeds.
      {" "}
      Unfilled day orders rest until the session ends. {r.fees ? feeScheduleText(r.fees) : <>Each contract costs {fee ? formatMoney(fee) : "the configured fee"}.</>}</> },
    { title: "Daily loss limit", body: dailyLoss ? <>Losing more than {formatMoney(dailyLoss)} from the day's starting equity trips the kill switch into reduce-only mode. Opening orders are cancelled; closing orders and exits still work.</> : "Set in Positions → Edit limits." },
    { title: "Exercise and assignment", body: "Equity and ETF options a cent or more in the money at expiry are exercised or assigned into 100 shares a contract, and long ones can be exercised early. A short option that trades below its exercise value at the close, such as a deep put, or a call whose time value is less than a dividend going ex the next day, can be assigned overnight, in part and at random, as real assignments are. Shares are marked at the underlying's price and can be sold or bought back in the regular session. Dividends come from a dividend file, from Massive or, in the demo feed and scenario replays, from the simulated calendar and the scenario's own dividend entries. At the day's rollover into the ex-date, shares held then receive the dividend and short shares pay it." },
    { title: "Program step", body: phaseFact(r) },
    { title: "Program costs", body: <>New purchase {formatMoney(r.evaluation_fee ?? "0")}; same-plan reset {formatMoney(r.reset_fee ?? "0")};
      unlocked-step activation {formatMoney(r.activation_fee ?? "0")}. {r.max_resets ? `${r.max_resets} resets per plan` : "Unlimited resets"}.
      Fees are bookkeeping only and never affect paper cash or rule outcomes.</> },
  ]
}

export function RulesView() {
  const { trading, accountScope } = useLive()
  if (!trading) return null
  return <Rules key={accountScope} trading={trading} />
}

function Rules({ trading }: { trading: TradingStatus }) {
  const account = useAccount()
  const plans = usePlans()
  const risk = useRisk().data
  const [start, setStart] = useState<string | null>(null)
  const data = account.data
  if (account.error) return <TradingError error={account.error} />
  if (!data) return <Empty>Loading rules…</Empty>
  return (
    <div className="min-w-0 space-y-4">
      <PageHeader title="Rules" subtitle="What your account trades under, and the plans you can start" />
      <Panel title={<span className="flex items-center gap-2">{data.rules.plan ?? "Paper account"} {evaluationBadge(data, data.rules.plan)}</span>}>
        {ruleText(data, trading.fee_per_contract, risk?.limits.max_daily_loss).map((rule) => <Rule key={rule.title} title={rule.title}>{rule.body}</Rule>)}
      </Panel>
      {risk && <PersonalRules risk={risk} />}
      <BreachPanel breach={data.breach} />
      <TradingError error={plans.error} />
      {plans.data ? <>
        {/* Funded plans appear only when the terminal offers them (lib/features). */}
        <Panel title="Evaluation plans">
          <PlanTable label="Evaluation plans" plans={plans.data.plans.filter((p) => p.rules.phase !== "funded")} columns={[
            ["Step", (p) => phaseFact(p.rules)],
            ["Profit target", (p) => p.rules.profit_target ? formatMoney(p.rules.profit_target, 0) : "—", true],
            ["Drawdown", (p) => p.rules.max_drawdown ? formatMoney(p.rules.max_drawdown, 0) : "—", true],
            ["Floor moves", (p) => floorMoves(p.rules)],
            ["Daily loss limit", (p) => p.rules.daily_loss_limit ? <span title={dailyLossFact(p.rules) ?? undefined}>
              {formatMoney(p.rules.daily_loss_limit, 0)} · {p.rules.daily_loss_action === "fail" ? "fails" : "locks the day"}</span> : "—"],
            ["To pass", (p) => p.rules.profit_target ? [p.rules.profit_basis === "balance" ? "Target, closed" : "Target", ...objectiveFacts(p.rules)].join(" · ") : "—"],
            ["Strategies", (p) => p.rules.buy_only ? "Buy only" : p.rules.defined_risk ? "Defined risk" : "Any"],
            ["Margin", (p) => p.rules.margin === "portfolio" ? "Portfolio" : "Strategy"],
            ["Slippage", (p) => `${p.rules.slippage_ticks ?? 0} ticks`],
            ["Expiry auto-close", (p) => p.rules.expiry_cutoff_seconds ? `${Math.round(p.rules.expiry_cutoff_seconds / 60)} min before` : "—"],
          ]} lock={(p) => lockReason(p, plans.data.plans, data)} enabled={trading.enabled} onStart={setStart} />
        </Panel>
        {offeredPlans(plans.data.plans, data).some((p) => p.rules.phase === "funded") && <Panel title="Funded accounts">
          <PlanTable label="Funded accounts" plans={offeredPlans(plans.data.plans, data).filter((p) => p.rules.phase === "funded")} columns={[
            ["Trailing drawdown", (p) => p.rules.max_drawdown
              ? `${formatMoney(p.rules.max_drawdown, 0)} ${p.rules.drawdown_mode === "static" ? "static" : p.rules.drawdown_mode === "intraday" ? "intraday" : "at close"}` : "—"],
            ["Floor locks at", (p) => p.rules.lock_balance ? formatMoney(p.rules.lock_balance, 0) : "—", true],
            ["Strategies", (p) => p.rules.buy_only ? "Buy only" : p.rules.defined_risk ? "Defined risk" : "Any"],
            ["Margin", (p) => p.rules.margin === "portfolio" ? "Portfolio" : "Strategy"],
            ["Slippage", (p) => `${p.rules.slippage_ticks ?? 0} ticks`],
            ["Payout after", (p) => p.rules.payouts ? `${p.rules.payouts.qualifying_days} days of ${formatMoney(p.rules.payouts.qualifying_profit, 0)}+` : "—"],
            ["Your share", (p) => p.rules.payouts ? `${p.rules.payouts.split_percent}%` : "—"],
          ]} lock={(p) => lockReason(p, plans.data.plans, data)} enabled={trading.enabled} onStart={setStart} />
        </Panel>}
      </> : !plans.error && <p className="text-sm text-muted">Loading plans…</p>}
      {start && <ResetDialog trading={trading} attempt={data.evaluation.attempt} initial={start} onClose={() => setStart(null)} />}
    </div>
  )
}

function PlanTable({ label, plans, columns, lock, enabled, onStart }: {
  /** Header, cell and whether the column holds figures. */
  label: string; plans: Plan[]; columns: [string, (plan: Plan) => ReactNode, boolean?][]
  lock: (plan: Plan) => string | null; enabled: boolean; onStart: (id: string) => void
}) {
  return (
    <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label={label}>
      <table className="w-full text-left text-xs whitespace-nowrap">
        <thead className="text-[11px] uppercase tracking-wide text-muted"><tr>
          {["Plan", "Starting balance", ...columns.map(([header]) => header), ""].map((h) => <th key={h} className="px-2 py-2 font-normal">{h}</th>)}
        </tr></thead>
        <tbody>{plans.map((p) => {
          const locked = lock(p)
          return <tr key={p.id} className="border-t border-border/40">
            <td className="px-2 py-2"><div className="font-medium">{p.name}</div><div className="max-w-64 truncate text-[11px] text-muted" title={p.summary}>{p.summary}</div></td>
            <td className="px-2 py-2 tabular">{formatMoney(p.initial_cash, 0)}</td>
            {columns.map(([header, cell, numeric]) => <td key={header} className={`px-2 py-2 ${numeric ? "tabular" : ""}`}>{cell(p)}</td>)}
            <td className="px-2 py-2 text-right"><button type="button" className="trade-button" title={locked ?? undefined}
              disabled={!enabled || locked != null} onClick={() => onStart(p.id)}>{locked ? "Locked" : "Start"}</button></td>
          </tr>
        })}</tbody>
      </table>
    </div>
  )
}
