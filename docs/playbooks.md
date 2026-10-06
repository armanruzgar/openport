# Playbooks

A playbook is a named JSON setup with immutable numbered versions. It selects
listed contracts, checks entry conditions and sizes an order through the normal
server preview. It does not supply market prices. Orders and fills are simulated.

The Playbooks page lists definitions, edits their JSON, selects an account's mode
and compares rule adherence with results. Trade, Brief and Dashboard show staged
orders.
Archived setups retain their stats and saved versions on the page.
The Journal's Playbook filter includes every version of the selected setup; its
Tag filter can narrow that to one version.

## Definition

`POST /api/playbooks` accepts this example. It is a setup, not a recommendation:

```json
{
  "id": "morning-put",
  "name": "Morning put spread",
  "description": "Write the setup and its invalidation here.",
  "underlyings": ["SPX"],
  "window": {"start": "09:45", "end": "11:00", "weekdays": [1, 2, 3, 4, 5]},
  "conditions": {
    "price": {"reference": "day_open", "direction": "above", "value": 0},
    "dte": {"min": 0, "max": 1}
  },
  "structure": {
    "template": {
      "kind": "vertical", "type": "put", "direction": "credit",
      "target": {"mode": "delta", "value": 15}, "width": 5
    },
    "expiry": {"min": 0, "max": 1}
  },
  "sizing": {"units": 1},
  "management": {
    "take_profit_percent": 50, "stop_credit_multiple": 2, "close_by": "15:45"
  },
  "guardrails": {"max_entries_per_day": 1, "cooldown_minutes": 30}
}
```

All top-level fields above are required; `conditions` may be empty. `version` is
assigned on creation. An update must carry the current version; the server checks
it and increments it. Unknown fields, invalid types, nonfinite values, duplicate
symbols or weekdays, and reversed ranges are refused. IDs use 1–15 lowercase
letters, digits or hyphens. Names allow 100 bytes and descriptions 2,000; neither
accepts control characters. There may be 100 IDs, 20 underlyings per definition,
and at most 8 MiB of persisted history. Archived IDs cannot be reused. A refusal
names the missing or invalid field, for example `management.close_by is required`.

Windows use New York market time, including daylight saving time. Weekdays are
1–5, Monday–Friday. Start is inclusive and end exclusive; windows cannot cross
midnight. The normal product session and feed-freshness checks still apply.

| Optional condition | Meaning |
| --- | --- |
| `price` | `reference` is `level`, `prior_close` or `day_open`; `direction` is strictly `above` or `below`. `value` is an absolute level for `level`, otherwise a signed offset in points |
| `iv_rank: {min,max}` | Inclusive rank range, in fractions. Uses the same local and explicitly proxied history as the volatility view; rank can exceed 0–1 |
| `vrp_min` | 30-day model-free IV minus 21-session close-to-close realized volatility, strictly above this many vol points |
| `term_inverted` | Whether model-free 9d/30d is greater than one; false includes equality |
| `dte: {min,max}` | Inclusive ACT/365 calendar days to the selected expiry's settlement |
| `technical` | SMA/EMA price comparisons, RSI ranges and Bollinger bands; see technical rules below |
| `vix: {min,max}` | Inclusive current VIX index level |
| `gap: {min_percent,max_percent}` | Inclusive signed day-open gap vs prior close |

A missing input fails its condition. The publication's `reasons` and backtests'
`entry_reasons` name the first failed condition, checked in this fixed order: DTE,
IV rank, VRP, term structure, price, VIX, gap, then technical rules. Reasons include measured values and
thresholds, for example `IV rank 0.12 is outside 0.30-1.00` or
`Price 5912.30 is not above day open 5920.00 + 0.00`. Missing inputs name what is
absent: `IV rank unavailable (no IV history)`, `Day open unavailable (no 09:30 minute)`
or `Prior close unavailable`. Formatting uses fixed decimal precision and no locale.

Prior close uses the preceding business day's
official close (in a scenario replay or backtest, the previous close the generated
day opens with), or its stored daily close. Day open requires the 09:30 minute,
not the first later observation of a partially recorded day. No missing opening
price, IV history, realized volatility or expiry is synthesized. Volatility
conditions use the existing finite-strip and proxy estimates, with the limitations
in [Volatility](volatility.md).

`structure.expiry` selects the earliest unexpired settlement in its inclusive DTE
range. The C++ template picker also serves the terminal; there is one leg-selection
implementation. Its templates are vertical, condor, iron-butterfly, strangle,
straddle, butterfly, calendar and diagonal. Parameters match the Strategy templates
menu: delta is absolute 0–100, widths and offsets are points, and `moves` uses
forward × ATM IV × square root of years. Calendars and diagonals specify the exact
`farExpiry` ID and `offset`. Strikes snap to the nearest listed strike, with ties
to the lower strike. Missing quotes and wings fail without substituting another
contract. Calendar expiry IDs must be updated when moving to another session.

Sizing takes either `units` (1–100,000) or `floor_share` (greater than zero, at most
one). Both are capped by the preview's `max_units`, buying power and account limits.
Fixed sizing uses a floor share of one as its cap. Fractional sizing requires an
actual plan or personal floor. The preview's loss basis is shown: a scenario-grid
loss is an estimate, not a bound for an unbounded structure. A staged net limit
uses executable far sides and rounds to a cent. Submission and fills still run
all normal risk checks.

Management uses the existing combo bracket conventions. `take_profit_percent` is
the closing premium as a percentage of the entry premium: 50 buys back half a
credit; 150 receives one and a half times a debit. `stop_credit_multiple` must be
greater than one and needs a credit entry. A stop already reached by the current
closing natural fires as soon as the entry fills, so the room allowed by
`stop_credit_multiple` must exceed the spread's own bid-ask cost. Alternatively,
`stop_underlying` takes
`level` and `direction: above|below`. Bracket levels are fixed at submission and do
not follow later entry fills. The take-profit limit sits on the legs' smallest tick,
a nickel for SPX, NDX and RUT spreads and a cent for SPY, QQQ and IWM, and rounds to
the better price, so it never asks less than its percent; stop triggers take any cent.

`close_by` is required and must be at or after the window end. Without
`max_hold_days`, it is the entry day's deadline. With that optional 1–365 calendar
day allowance, it is that later day's deadline, moved back to the previous business
day if necessary. It is capped at the day's regular close. At the first market
update at or after the deadline, a time stop cancels the entry and its working exits
and submits a reducing market IOC. New runs label those cancellations
`PLAYBOOK_TIME_STOP` / `Playbook time stop`, visible in order reasons and detail,
the Orders page, Journal trade details, the reducer journal and trade CSV's
`time_stop_orders`. The close order retains `Playbook automatic time stop; entry N`
in its note, shown in Orders and Journal details. Older recorded runs keep their
original `USER_CANCEL` labels when verified.

The time stop checks the close through the normal preview first: while the checks would refuse it, for example because a leg has no bid or
its quotes are stale, it records no order and checks again at each update,
submitting once the close can be accepted. The delayed closure then fails the
time-stop adherence rule.

Guardrails allow 1–1,000 entries per day and a loss cooldown of 0–525,600 market
minutes. Entries include filled and working opening orders across versions of the
same ID, on this account and attempt. A cancelled unfilled entry does not count.
A closed losing strategy starts the cooldown. Personal guardrails remain in force.

## Storage and modes

Definitions and account mode bindings live in `playbooks.json` beside the main
journal. Writes use a temporary file and atomic rename. Every version stays in the
file after editing or archiving. Definitions are not reducer state. Staging and
same-day dismissals are transient; staging does not journal an order, consume
liquidity or alter account state. After restart the server evaluates again.

Each account chooses `off`, `stage` or `auto` for each ID. New definitions start
off. The server evaluates enabled playbooks when the analytics update, not on every
quote. On a live feed they refresh at most once a second when new data has arrived:
about once a second on a streaming feed, and after each 15-second poll on the Cboe
and demo feeds. In a replay they update at each complete snapshot, every 15 market
seconds for demo days and scenarios. A rule that turns on the clock, such as a
window opening, a cooldown ending or a `close_by` deadline, therefore takes effect
at the first update at or after that time: a cooldown ending at 10:05:07 allows the
next entry at 10:05:15, which trades that snapshot's quotes. A stage holds selected
contracts, units, exits and a version tag. Its `warnings` array contains the final
preview's `{code, message}` advice (including `STOP_REACHED`), or is empty; these
warnings are transient, never journaled, and do not change whether Auto sends.
Each stage's `order` is the exact body
shape accepted by `POST /api/orders` (or `/api/replay/orders`): decimal-string
prices, word-valued side/type/time_in_force, legs, optional bracket, tags, note and
client_order_id. Clients may review or submit it themselves; normal validation,
risk checks and client-ID deduplication still apply. `request` remains the legacy
internal journal encoding with integer micro-dollar prices and numeric enums.
Older servers omit `order`. The terminal continues using the stage send route.

A stage expires when conditions or the window end. Its ID stays the same while the same contracts are selected; its price, size
and exits follow the market. Send re-evaluates, then submits the stage's latest
order through the normal order path, so a click on a moving market still sends. When other contracts are selected, the old ID is refused
and the new stage is shown. Each entry gets a new client order ID, so a repeat send
cannot duplicate an entry and a later entry is never taken for a retry. Dismiss
suppresses that setup and underlying for the rest of the New York day. Staging
refuses contracts already held or covered by working orders, so separate setups do
not silently share a position.

**Auto is available on all live paper accounts**, main or named, practice or
an evaluation plan, as well as replay/scenario accounts. Enabling it uses the same
`{"mode":"auto"}` API request (admin scope); the terminal first names the account
and confirms that orders will be sent automatically on paper. A live account must
have a known market time before Auto can be enabled. Session, quote/feed freshness,
risk, guardrails, kill switch and plan checks still apply through the normal preview
and submission paths. Auto sends entries, cancels stale entries at their deadline,
and fires time stops on live updates just as in replay. A stale or stopped feed
cannot trigger an entry or close on stale prices. A close waits until its feed and
quotes are usable; working protection is not cancelled on a stale-feed close attempt.
A rejected automatic submission is suppressed for that New York day. On live
accounts its journaled rejection also preserves suppression after restart.
Preview failures are retried on later updates without recording rejected orders.
Successful entries remain subject to entry limits, overlap and cooldown.

Modes persist across restart. The first fresh update resumes Auto, sees journaled
orders for overlap and entry limits, and catches up an overdue time stop. A filled
entry is not sent again just because the server restarted. Switching to Off or
Stage stops automatic management; existing orders and positions remain for the
trader to manage. Archiving switches every live account's binding for that ID off.

ReplayHost copies the live definitions, with modes off and no forward-test windows,
into an isolated run.
Select auto after pausing or before stepping the replay. Changes in that run do not
edit the live catalogue. Its sidecar is `<run>.playbooks.json`; the initial
catalogue and subsequent definition commands also enter run provenance. Offline
verification and archived reports use those recorded versions. Older run inputs
without a catalogue still load. Replay volatility conditions cannot borrow the
live engine's history; unavailable replay inputs prevent entry.

Entries carry `playbook:<id>@v<version>` and the template tag. Their order note
retains the staged preview size, loss estimate, deadline and whether submission
was automatic. A sent stage records the sender as its actor, as any order does;
automatic entries and time stops record `system`. They do not use `Order::system`,
which is reserved for reducer liquidation and would change checks.

## Forward tests on live paper

Enabling Auto opens a window for the account, playbook ID and current version,
with the start market time and enabling actor. Repeating Auto is idempotent.
Off, Stage or archive closes the window at the current market time. Auto follows
the latest definition: saving a new version closes the previous window and opens
one for the new version on every live account running it; the editing actor is
recorded. Existing positions retain their entry version's management deadline.
Windows are stored in the optional `forward_tests` array in `playbooks.json` using
the same atomic write as modes and definitions. Older catalogues without it load
unchanged. The order-ID boundaries distinguish toggles at the same market instant.

`GET /api/playbooks?account=ID` includes `forward_tests`, keyed by playbook ID.
Each contains `windows`, `running`, `days_running`, `entries`, `time_stops`,
`rejected_entries`, `report` and reports grouped by version in `versions`.
Write responses include windows; fetch again for statistics. Windows include
`account`, `playbook`, `version`, `started`, nullable `ended`, `actor`, inclusive
`first_order` and exclusive nullable `end_order` (decimal strings). The results
reuse the adherence report's trade rows and statistics: trades, win rate,
expectancy, profit factor, average R, return on buying power and adherence.
They cover the journal's attempts, rather than just the current attempt. Only
opening orders marked automatic with actor `system`, matching the version and
window's order bounds, qualify; manually sent stages and other tagged trades do not.
Trades must first fill while a qualifying window is open; their later outcome
continues to update after the window ends. Entries count accepted orders, including
unfilled/cancelled entries. Rejected entries count journaled automatic submissions;
preview failures are not entries. Time stops count filled automatic close orders
for those entries, including delayed closes after a window ends. All values derive
from the catalogue and journal, with no wall-clock counters.

`days_running` sums elapsed market-time durations in 24-hour days, including
weekends and downtime, through the account's latest published market time for an
open window. It is not a count of observed sessions. A running window means Auto
is enabled, not that the feed is healthy or that a trade is currently eligible.
The Playbooks page shows these windows and results beside the latest completed
saved backtest matching the same ID and version. `GET /api/backtests` supplies
optional `playbook: {id, version}` and `summary` on saved runs with reports; older
saved reports remain readable. Comparison is descriptive: the backtest may use
different days and a different plan. Brief and Dashboard indicate when the current
account has automatic playbooks. Replays and backtests never inherit forward tests.

## Adherence and results

Reports use the current attempt, grouping a strategy once by the order that opened
its contract round trips. Each trade checks its original version's entry window,
size cap, specified entry exits, time-stop deadline and entry/cooldown guardrails.
Guardrails are checked as of the entry's acceptance, as the server checked them: a
losing strategy closed in the same market second counts only if its closing order
arrived before the entry.
A still-open trade's time stop is pending until its deadline. Missing evidence
cannot establish compliance: unknown sizing fails rather than receiving a pass.
Staged entries retain the preview sizing evidence in their immutable order note,
including personal-floor sizing and cross-expiry scenario estimates. Other tagged
entries use their recorded context, measurable strategy risk and entry fees; older
fills without that evidence may not be scoreable. This is a trading review, not an
authentication check on clients supplying their own tags or notes.

Adherence is passed rules divided by measured rules. Closed trades report win
rate (wins divided by decided trades, wins and losses: a breakeven is neither, as in
the Journal and backtests), average positive and negative net P&L,
expectancy (mean net P&L), profit factor (total wins divided by total losses), and
average R over trades with positive planned risk, and the average return on buying
power (each trade's `return_on_buying_power`: its net over the `buying_power` its
entry needed, see [trade review](paper-trading.md#return-on-buying-power)). Net
includes fees. No losses
with wins displays infinity; no observations display missing. Separate summaries
show followed-the-rules and deviating trades. Entry exits are checked as submitted;
the report does not reconstruct every later manual edit to an exit. Manually
mixing contracts from different strategies can make attribution incomplete.

## Pass odds

Configured time and inactivity deadlines are checked on projected plan trading
dates before each projected day’s assumed own execution; weekends and holidays
count toward these calendar-day limits.

`GET /api/account/pass-odds?days=20&samples=1000&playbook=morning-put&seed=81723`
returns `pass`, `fail`, `neither`, `median_days_to_pass`, `historical_days`, `seed`,
`days`, `samples`, `block_days`, `history_basis`, `path_assumption` and `label`.
Omit `playbook` for the account. Seed defaults to 81723 and is returned as a decimal
string. Days must be 1–252 and samples 1–10,000. The Dashboard and Playbooks page
label the result **Estimate from past results, not a prediction**.

The bootstrap draws circular blocks of three consecutive completed historical
days, adding each day's equity changes to the attempt's current equity. It uses
recorded open, low, high and close; low and high follow their stored timestamp
order. The recorded extremes begin at a day's first fully marked observation, so
they are widened to include its open and close: a day that gapped and kept going
stays in the sample. At each point it applies the same C++ target, floor-touch, trailing peak,
locking-floor and end-of-day ratchet functions as the trading reducer. End-of-day
plans first close the current day. No future price paths or returns are invented
and labelled as observations. Median days to pass is conditional on passing; it
is null if no sample passes. A decided attempt retains its outcome.

At least ten completed days with recorded extrema are required. Older days
without low/high observations are excluded. Account history is the current
attempt's equity-day history. Per-playbook estimates use only days attributable
exclusively to that playbook: mixed strategies, delivered shares and withdrawals
prevent reliable attribution of the account's intraday low. Those days are
excluded rather than allocating an invented low to one playbook.

The account plan rules are exact; the resampled equity path is an estimate. The
four observations do not describe every intraday excursion or intermediate high,
and block bootstrap does not model a changing edge, changed size or regime. It
has no confidence guarantee and is not a forecast of an evaluation outcome.

## API

All account routes accept `account=ID`, defaulting to `main`. Replay routes use the
usual `/api/replay/` prefix and the replay's account. Existing write protections
apply to every mutation. Creating, editing and archiving definitions and choosing
modes need the `admin` scope; sending and dismissing a stage need the account's
`trade` scope, or `replay` in a replay, as orders do.

| Route | Contract |
| --- | --- |
| `GET /api/playbooks` | Definitions including versions and archive flags, this account's modes, transient stages, reasons, adherence/expectancy reports and forward-test windows/results |
| `GET /api/playbooks/{id}?version=N` | One immutable definition; latest when version is omitted, including archived definitions |
| `POST /api/playbooks` | Create from the definition JSON |
| `PUT /api/playbooks/{id}` | Complete replacement definition with its current `version`; creates the next version |
| `DELETE /api/playbooks/{id}?version=N` | Archive after a version check; keep historical versions |
| `PUT /api/playbooks/{id}/mode` | `{"mode":"off"}`, `stage` or `auto` (live paper and replay; live requires market time) |
| `POST /api/playbooks/staged/{stage}/send` | Empty object; recheck and submit the stage's latest order |
| `POST /api/playbooks/staged/{stage}/dismiss` | Empty object; dismiss for this day |
| `GET /api/account/pass-odds` | Seeded, labelled estimate as above; 422 `PASS_ODDS_UNAVAILABLE` for insufficient history, incomplete marks or no evaluation rule, 400 `INVALID_REQUEST` for invalid inputs, 404 `UNKNOWN_ACCOUNT` for an unknown account |
| `GET /api/strategy-template?symbol=SPX&expiry=ID&template=JSON` | URL-encoded template JSON; return legs, tag and actual widths. Optional paired `min_strike`/`max_strike` preserve a terminal's loaded strike window |

## Batch backtests

Backtests are simulated trading on recorded or generated days. They are not
predictions or investment advice. The CLI, API and Backtest page use the same
headless runner, without a provider connection or HTTP server in the runner.

```sh
openportd --backtest morning-put@2 --plan eod-50k \
  --scenarios 20 --seed 81723 --out report.json
openportd --backtest morning-put --plan eod-50k \
  --recordings ./recordings --out recorded.json
openportd --backtest morning-put@2 --plan eod-50k \
  --days days.json --out mixed.json --workers 4
```

Definitions come from `playbooks.json` beside `--paper-journal`, or from
`--playbooks FILE`. Omitting `@VERSION` pins the latest version when the run starts.
Archived versions can be tested. The snapshot contains only the selected playbooks, in auto
mode on main; editing the live catalogue cannot change an ongoing run.

### Joint backtests

Use `--backtest morning-put@2,afternoon-call --plan eod-50k ...`, or replace
`playbook` with `"playbooks": ["morning-put@2", "afternoon-call"]` in
`POST /api/backtests`. Exactly one selector field is required. Joint selections
must contain 2–8 distinct IDs; selecting two versions of one ID is rejected.
Missing versions pin latest at start, including each definition's version history
for journal verification. In the terminal, use **Add playbook** and select each
version; removing the extra rows returns to a single run.

Every day uses one Desk and one main account, with all selected definitions in
Auto. Attempts carry that same shared account across days until pass/failure.
Cash, buying power, held-contract caps, scaling, daily loss, trade-risk and flat-time
limits, evaluation floor, target and consistency all apply to the account as a
whole. Entry windows, sizing, management and entry/cooldown guardrails remain
per playbook. Existing held/working-contract overlap checks still apply.

Entry evaluation and submission use **ascending lexical playbook ID order**,
regardless of request order; each definition's underlyings retain their array order.
The Desk handles existing automatic exits first in opening-order order, then
entries. Each submission refreshes the shared account before the next entry.
When a limit binds, the earlier ID wins; the later ID gets the normal entry-blocking
or rejection reason. Worker count changes only independent-day scheduling:
completed reports and journals are byte-identical at one or eight workers.

Joint reports use schema **3**, `mode: "joint"` and `playbooks` containing pinned
definitions in evaluation order; `playbook` is absent. Single reports retain their
existing schema and bytes. Account-level days, attempts and evaluation rows have
the same meaning. `per_playbook` on each day and attempt, and on `summary`, contains
closed/open trade counts, expectancy, win rate, adherence, return on buying power,
realised and marked option P&L, entry reasons and attributable rule trips. The
summary pools independent days, not attempts. Realised P&L includes fees and partial
closes; marked P&L adds unrealised option P&L and is null if an attributed open
position lacks fresh marks. Shares from assignment and dividends remain account
results, outside the existing playbook attribution. Account-wide rule trips are
untagged; identifiable order rejections carry `playbook`. Summary reason keys include
the date. Trades keep their versioned playbook tags.

Schema 1/2 saved reports still load in listings, compare and the terminal. Joint
listings carry `mode` and `playbooks` identities including names; `?playbook=ID` or
`?playbook=ID@VERSION` matches any member. Forward-test comparison uses the member's
attributed summary when the latest matching saved run is joint.

### Day inputs

`--days` reads a JSON array. Recording paths are relative to the manifest. Imported
recordings use the same `.oprec` format and follow the same replay path:

```json
[
  {"file":"recordings/2026-09-14.oprec"},
  {"scenario":"reversal","date":"2026-09-15","seed":"81723"}
]
```

Supply 1–252 days with distinct, increasing trading dates. Each recording must
contain one trading day. Partial recordings are accepted; results retain their
first and last observed market times and do not imply full-session coverage.
`--recordings` selects `.oprec` files ordered by header start time, then filename. `--scenarios N --seed S` cycles regular scenarios in
catalogue order, starting at the first scenario's configured date and advancing
one business day per entry. Seeds are S, S+1, …; overflow is refused. The API also
accepts `scenario: ID` to repeat one scenario. Individual manifest entries can
select overnight scenarios. The terminal's JSON manifest requires quoted seed
strings to preserve all 64 bits. Dates select calendars and expiries; generated days
do not reconstruct historical events. `--scenario-dir` supplies custom scenarios.

`--plan` takes an evaluation preset ID, or a JSON file with `initial_cash`, `rules`
and optional `fee_per_contract`. The API accepts that object directly as `plan`.
Custom rule money uses decimal strings; drawdown mode is `intraday`, `end_of_day` or
`static`, margin is `strategy` or `portfolio`, `account_type` is `margin`, `cash` or `ira`
(with `house_margin_percent` and `pm_vol_shock` as whole numbers), and `expiry_cutoff` is
nanoseconds. The plan objectives and daily loss rules take their journal names and forms:
`daily_loss_limit` and `profitable_day_profit` are decimal strings, `profit_basis`,
`daily_loss_basis`, `daily_loss_action` and `consistency_basis` their API words,
`lock_at_start` true or false, and `consistency_percent`, `min_trading_days`,
`min_profitable_days` and `day_end_minutes` (minutes after New York midnight) whole
numbers. `max_contracts_held` (0–100000) caps held options plus working opening contracts. `require_stop_loss` is a boolean; `max_trade_risk` is decimal-string dollars and `max_trade_risk_percent` a whole percentage (0–100). Normal preview and entry checks enforce them for automatic entries too. Time and instrument rules accept `time_limit_days` and `inactivity_days`
(0–366 calendar days), `underlyings` (up to 32 unique uppercase symbols; empty
allows all), and `trading_start`/`trading_end` (both `HH:MM` New York or null).
`flat_time` is optional `HH:MM` New York (`00:00`–`23:59`, before
`day_end_minutes`) or null. It closes options and shares and blocks automatic
openings with `FLAT_TIME` until plan day end; reductions remain available.
`no_overnight` is independently boolean, default false, and fails a held position
at rollover with `OVERNIGHT_HOLD`, excluding options awaiting settlement.
System option closes use `system:flat_time:N` / `system:overnight:N`; shares use
`rule` fills. Remainders retry on executable quotes. Both fields are carried
through backtest plans, playbook entries, journals and recovery.
They apply through the same reducer checks as manual trading. Optional `scaling` takes up to 16 `{profit, contracts}` steps (decimal
profit strings, integer limits): first profit zero, profits strictly increasing,
contracts non-decreasing from 1 to 10,000. Empty disables. It caps option contracts
held including working openings; each leg counts, shares do not. Rollover uses
closed balance less start to select the next session's cap, up or down; automatic
entries receive `SCALING_LIMIT` like manual orders. Omitted rules use `AccountRules` defaults. Positive starting cash and a profit target are
required for evaluation plans; practice and funded preset IDs are refused. Custom
funded plans may instead set `phase: "funded"`, no profit target, and `payouts` with
at least one qualifying day. Payout fields use their account API names; money
(including `buffer` and `caps`) is decimal strings, `consistency_percents` is an
array of 1–100 integers and `buffer_payouts` is 0–100. Other omitted payout fields
use `PayoutRules` defaults. A funded backtest carries these rules in its journals
and account views but does not request withdrawals automatically; a surviving
funded attempt stays open. An evaluation example is:

Custom funded backtests also accept `size_scaling` with `profit_percent`, `payouts`,
`days`, `increase_percent` and decimal-string `max_balance`. Each periodic review
counts finished plan days and realised profit plus withdrawals; qualifying accounts
receive a capital credit based on the original size. It is excluded from reported
P&L, raises the floor, and scales the loss limits. Absent/null disables the rule.

```json
{"initial_cash":"50000","rules":{"plan":"Example evaluation",
 "profit_target":"6000","max_drawdown":"3000","drawdown_mode":"end_of_day",
 "buying_power":true,"defined_risk":true,"expiry_cutoff":300000000000}}
```

### Days and attempts

The independent daily pass runs a fresh account under the plan for each supplied
day. A bounded pool uses four workers by default; `--workers 1` runs sequentially,
and 16 is the maximum. Temporary generated recordings are removed after each day
and regenerated for the attempt pass, so they do not accumulate with the batch
size. Every selected replay batch reaches the Desk in file order. Each day admits
its recording's full subscription, preserving cross-underlying analytics and
history inputs; attempts
admit the union of their supplied inputs' subscriptions.

The attempt pass then replays those inputs in order on an actual carried account.
Cash, positions, orders, fees, evaluation progress and candle history survive day
boundaries. Instrument IDs and the analytics book are reset between input files,
since IDs belong to their recordings. Position marks remain the reducer's last
observations until new data supplies them. This pass is sequential: sizing and risk
checks depend on the earlier days' account state. It does not add independent daily
P&Ls or resample four equity points.

An attempt starts at the plan's initial cash on the next unconsumed day. It ends at
the first batch whose account has passed or failed. The reducer checks fully marked
equity after transactions and atomic fills: touching the drawdown floor fails;
otherwise reaching initial cash plus target passes. Intraday floors follow each
high; end-of-day floors ratchet at the next observed trading-date rollover. The
next attempt starts fresh on the following supplied day; unused observations on
the decision day are not reused. A fresh attempt also starts with empty candle
history; it uses recorded observations and earlier days within that attempt, not
the prior attempt's market context. The last undecided attempt is `open`. Gaps between
supplied trading dates are allowed, but no missing day's prices are invented.
An entry rejection or daily-loss kill latch is recorded where it occurs; it is not
an evaluation failure unless the reducer's evaluation rules fail the account.

### Reports and reproducibility

Single runs retain report schema 2, which adds optional `input_set` and attempt
`day_rows`; schema 1 reports remain readable. Joint runs use schema 3 with the
ordered definitions and attribution described above. Each attempt has one row per consumed supplied day, captured at
the last replay observation (or the decision batch). Rows contain the date and
zero-based input index, start/end closed balance and equity, equity-change P&L,
valuation completeness, current floor and distance, profit target and progress,
peak, daily lock, decision code/status, trips and counts of trades opened/closed.
These read the session snapshot and shared reducer evaluation helpers. They do not
advance the clock or ratchet an end-of-day floor early: that still happens at the
next observed plan-day rollover. Equity fields are last-mark estimates; incomplete
marks make P&L/floor distance null. Target progress follows the plan's balance or
equity basis. The terminal expands each attempt into its daily evaluation table.

Each daily result includes marked P&L, maximum observed peak-to-later-trough equity
drawdown, the smallest observed equity-minus-floor distance, rule trips (evaluation
failures, plan daily-loss locks, kill latches and order rejections), strategy
trades, option and share fills, share round trips, adherence, entry-blocking reasons,
mark quality and open positions. Entry-blocking reasons give, per setup and
underlying, the last reason an evaluation gave inside the entry window, such as
missing floor room; a day whose window never opened says `Outside entry window`.
Drawdown includes the starting balance, pre-fill marks reconstructed from committed
option executions, and committed fill observations. Missing
final marks make P&L null; the last-mark estimate is separate. Floor distance is
null when the plan has no drawdown floor. Adherence uses the normal playbook report,
including pending checks. No trades means null expectancy and win rate.

Share round trips are reported separately from strategy expectancy. Trade expectancy
is mean net P&L per closed opening strategy in the independent
daily pass. Win rate is strictly profitable closed strategies over decided ones
(profitable or losing); breakevens are left out, as in the Journal and the playbook
report. `average_return_on_buying_power` is the mean of the closed strategies' return
on the buying power their entries needed. Day win rate likewise divides profitable days by days with a profit or loss. Daily P&L and drawdown distributions contain sorted decimal
values, min, lower-index quartiles, max and an exact-money mean. Daily P&L statistics
exclude incomplete final marks. Worst days are up to ten day indices sorted by P&L,
with input order breaking ties. Pass rate is passed / (passed + failed) attempts;
it is null without decided attempts. Open attempts are reported separately.

The report uses no runtime timestamps, random IDs or worker counts. For unchanged
inputs, configuration, initiating actor, calendar and build/platform, completed
JSON reports and journals are byte-identical at every worker count. Results use decimal money
strings; the complete `config` preserves journal encoding (integer micro-dollars
and journal enum encodings) for reproducibility. Seeds in displayed input identities are
decimal strings. Floating-point analytics are not guaranteed identical across
platforms, as with individual replays.

The CLI writes `REPORT.json` and a new `REPORT.json.d/` directory; it refuses to
reuse that directory. Journal paths are relative to it, for example:

```sh
openportd --verify-run report.json.d/days/000001.jsonl
openportd --verify-run report.json.d/attempts/000001.jsonl
```

Attempt journals record each input transition. Older single-input journals still
load and verify. Recordings and custom scenario definitions must remain available
and unchanged for verification. The report's `journals` list also includes kept
prefixes interrupted by cancellation. SIGINT/SIGTERM or API cancellation checks
between replay batches and jobs; an in-progress scenario generation finishes first.
Completed results remain in a labelled partial report. Unfinished days are null;
unfinished attempts are excluded from aggregate statistics. Input or storage errors
produce a failed report rather than zero-return days.

No new volatility-history inputs, prices or settlements are invented. A playbook
needing unavailable history will not enter. Open positions at EOF are reported,
not forcibly closed at fabricated prices. Attempt liquidation can also remain
incomplete at its final batch if quotes lack executable liquidity. Reusing a small
set of generated scenarios says nothing about future market performance.
The CLI uses default analytics and an empty dividend calendar. API runs use the
server's startup analytics and configured dividends, without later provider
updates. Both are recorded in the report; neither path fetches additional history.

### API and saved runs

| Route | Contract |
| --- | --- |
| `POST /api/backtests` | Start `{playbook:"ID@VERSION",plan:"eod-50k",days:[…]}` or `{playbook,plan,scenarios:N,seed:"S"}`; replace `playbook` with `playbooks:["a@2","b"]` for joint mode; optional `scenario` and `workers`; returns 202 |
| `GET /api/backtests` | Saved run summaries and the active ID; `playbook: {id,version}` or joint `playbooks`, plus `summary`; optional `?playbook=ID[@VERSION]` filter |
| `GET /api/backtests/{id}` | Progress and the completed or partial report |
| `GET /api/backtests/compare?ids=A,B` | Compare 2–8 distinct finished reports, including partial reports, and their combined independent daily P&L |
| `DELETE /api/backtests/{id}` | Request cancellation; `DELETE /api/backtests` cancels the active run (unchanged) |
| `DELETE /api/backtests/{id}?purge=true` | Permanently delete all saved files; 409 while active: cancel first and wait |
| `PUT /api/backtests/{id}` | Set `{"keep":true}` to exempt from retention, or false to release the pin; works while active too |

Only one batch job runs at a time per server; another start returns 409. Mutations
need `replay` scope (or `admin`); reads follow the existing `read` token policy.
API recording paths are confined to the configured recording directory, including
symlink checks. Writes are disabled with paper trading or server writes disabled.
Reports, journals and progress files live in `backtests/ID/` beside the main paper
journal. The response's `directory` resolves its relative journal paths. Saved
reports remain readable after restart; unfinished runs are marked `interrupted`
and do not resume automatically. The terminal shows saved reports and progress
without switching the active trading account. Listings include `bytes` (logical file
sizes, not allocated filesystem blocks) and `keep` (false for older runs). Purge
removes reports, journals and progress, including pinned runs; unknown IDs return
404. Paths stay inside the backtests root; symlink roots, runs or children are
refused with 409 `BACKTEST_UNSAFE_PATH`. The terminal asks before permanent deletion.

`--backtest-keep N` defaults to **20**, with **0 = unlimited**. Before starting a new
run, remove the oldest finished unpinned runs beyond N, in creation/ID order.
Pinned runs do not count against N. Cancelled, failed and interrupted runs count;
active runs never do. Thus a completed new run can leave N+1 unpinned reports until
the next start. Pins survive restart. Keep important reports before starting more
runs. CLI output directories are outside this server retention policy.

Comparison returns each run's playbook@version (or ordered joint playbooks), full plan configuration in the
report's existing journal encoding, input identities and summary. `daily` aligns
independent daily P&L over the union of supplied dates; a missing or unfinished day
is null. `different_inputs`, `different_plans` and `incomplete_inputs` flag
apples-to-oranges comparisons. Legacy reports derive identities from completed
days and flag missing identities in partial reports. A selected active run returns
409, a missing ID 404, and a finished run without a report 422
`BACKTEST_REPORT_UNAVAILABLE`. Invalid or duplicate selections return 400.
Exact-money overflow returns 422 `BACKTEST_COMPARISON_UNAVAILABLE`.

`combined` is a **sum of independent run days**: there is no shared buying power,
risk limit or plan floor across the selected runs. When any selected run is already
joint, `combined.includes_joint: true` and its label make that explicit. Single-only
comparison output is unchanged. Missing dates contribute nothing; supplied unfinished/unmarked days make that combined day
null and the cumulative curve unknown from then on. Its distribution and worst
days omit those unknown daily totals. The curve begins at zero; maximum drawdown
is its peak-to-later-trough decline (null if incomplete), not intraday drawdown.
The exact-money distribution uses the existing quartile and mean conventions;
day win rate excludes breakevens. Worst-day ties use date order. The terminal
shows the comparison table, combined curve and distribution. To trade several
playbooks on one account, start a joint backtest as described above.

## Practising strategies with shares

The terminal's chain and Positions pages offer **Trade shares** for stocks and ETFs.
Its preview shows cash cost, buying power and dollar delta from the account's margin
model. Shares trade only during the stock regular session, including early closes.

The chain's **Strategy → Templates** menu offers **Covered call** and **Collar**.
Choose listed strikes and contracts, buy 100 shares per contract, then review and
send the call (or the collar's long put and short call). These are two separate
orders, not atomic: the shares remain if the option order fails or fills partly.
Closing the dialog also keeps any purchased shares. Existing shares are not deducted
from the purchase; use the ordinary option ticket to write against shares already
held. The option step uses market IOC; inspect its fill result and any remaining
shares. Buy-only and defined-risk plans refuse these shortcuts: the latter keeps
its rule requiring long calls behind short calls even though shares cover them for
margin. A collar's long put costs its premium, without extra strategy margin.

Playbook definitions and automatic stages remain option orders; share purchases
are manual terminal/API trades. Python `trade_stock` and `preview_stock` and the
matching MCP tools support live named accounts and replay.

## Technical, VIX and gap rules

Optional `conditions.technical` is an AND array of 1–20 rules. Each has
`interval: "minute"|"day"` and integer `period` 2–500:

| Indicator | Other required fields | Passes when |
| --- | --- | --- |
| `sma`, `ema` | `direction: "above"|"below"` | Current underlying spot is strictly above/below the average |
| `rsi` | `min`, `max` (0–100) | Wilder RSI is within the inclusive range |
| `bollinger` | `direction: "above"|"below"`, `k` (>0, ≤10) | Spot is strictly above the upper/below the lower band |

For example, `{"technical":[{"indicator":"rsi","interval":"minute","period":14,"min":30,"max":70}]}`.
Calculations use chronological completed candle closes, at most the latest 1,000.
Minute bars count once their start plus one minute is at or before the underlying's
market time. Daily bars count only before its current New York date. The current
partial minute and entire current day are excluded. Missing minutes/sessions are
not filled. SMA uses the last N closes; EMA starts with the first N closes' SMA
and applies alpha=2/(N+1) to the remaining closes in that retained window. Wilder
RSI starts with mean gains/losses over N changes, then smooths each with alpha=1/N;
a flat series is 50, only gains is 100, only losses is zero. It needs N+1 closes.
Bollinger uses the last N closes' SMA and population standard deviation.
Changing available warm-up history can change EMA/RSI. A replay/backtest uses only
its observed candle history, never future bars or downloaded history.

`conditions.vix: {min,max}` is an inclusive index-level range (0–1,000), using
the observed underlying spot keyed by `VIX`, no more than five market minutes older than the traded
underlying and never newer. Demo revision 3+ supplies VIX when subscribed; live
feeds and recordings must actually carry VIX. Neither SPX implied volatility nor an option-implied VIX forward substitutes for the index.
Missing/stale data fails with `VIX unavailable (no current VIX spot)`.

`conditions.gap: {min_percent,max_percent}` is the signed percentage
`100 × (09:30 open / prior business close − 1)`, inclusive, bounds −100 to 10,000.
It uses the same official/stored prior close and exact opening minute as `price`.
Positive ranges select up gaps; negative ranges select down gaps. A missing open
or prior close fails explicitly. New checks follow the existing conditions:
VIX, gap, then technical rules in array order. Missing history reports, for example,
`SMA(20, day) unavailable: 12 of 20 daily bars`; threshold failures include the
measured indicator/band and comparison. No fast/slow crossover rule is included.

## Additional management rules

- `trailing_stop: {percent: P}` (0 < P ≤ 100) closes after P percent of the
  highest positive gross strategy profit is given back. Profit uses closing natural
  prices (sell longs at bid, buy shorts at ask), as combo stops do, actual opening
  fills, multipliers and any realized leg profit, excluding fees. It activates at
  any positive peak; there is no separate activation threshold. The reducer samples
  fresh positive quotes at its marking/execution cadence. Peak and first trigger
  are journaled only for entries carrying this rule, survive restart and latch
  until closed, including while a close cannot be accepted. Entries retain the rule
  in their order note. Missing/shared strategy evidence leaves adherence unknown.
- `close_at_dte: N` (integer 0–3,650) closes when the nearest remaining leg has
  at most N ACT/365 calendar DTE to settlement, on the first update at/after that
  instant, without a time-of-day restriction. Already-due entries are refused and
  pending entries are cancelled. Expiry settlement and account risk
  liquidation can close first. The regular time deadline still applies.
- `max_days_in_trade: N` (integer 1–365) moves `close_by` N exchange business days
  after the entry date (entry day is zero), skipping weekends/holidays, capped at
  the destination day's regular close. It is mutually exclusive with calendar
  `max_hold_days`; without either allowance, `close_by` remains the entry day.
- `stop_loss_percent: P` (0 < P ≤ 100) requires a debit entry. It attaches a combo
  bracket stop at signed closing debit `−entry_debit × (1 − P/100)`, rounded to a
  cent, triggering at-or-above that level. Like credit stops it is fixed at
  submission; it is mutually exclusive with credit-multiple and underlying stops.

Auto applies these rules in live paper, replay and backtest engines. Stage shows
all management rules for the trader to follow; submitted brackets still work.
Time/days deadlines take precedence over DTE, then trailing when simultaneous.
Automatic closes use reducing market IOC orders and retry on later updates if the
normal close checks refuse them. The new exits check before cancelling working
protection. Cancellations use `PLAYBOOK_TRAILING_STOP`, `PLAYBOOK_DTE_STOP` or
`PLAYBOOK_DAYS_IN_TRADE_STOP`; close notes name the rule and entry order. Orders,
Journal details and the existing CSV `time_stop_orders` column include them.

Adherence adds `close_at_dte`, `max_days_in_trade`, `stop_loss_percent` and
`trailing_stop` only when configured. Deadline rules are pending until due or
closed and pass if fully closed by the exact deadline, like `time_stop`; an update
arriving later can therefore close automatically but fail adherence. Debit stops
check submitted bracket levels. Trailing checks closure by its recorded first
trigger; a closed trade with sampled evidence and no trigger passes, absent
evidence is null. Stage/off stop automatic closes but do not erase trigger evidence.

Event rules also apply to playbook entries: `events`, `news_before_minutes`,
`news_after_minutes`, `news_action`, `hold_restrictions`, `hold_cutoff` use the same
saved AccountRules calendar in live, replay and custom backtest plans. Blackouts
and holding cutoffs block/cancel openings with NEWS_BLACKOUT/HOLD_RESTRICTED;
managed exits and reductions remain available. System closes do not count as
trader executions. See [F17/F59](paper-trading.md#news-blackouts-and-holding-restrictions-f17-f59).

Custom backtests preserve combined F6/F17/F59 rules alongside F31/F58/F62 and
F15/F60 in daily and attempt journals, including the frozen holding calendar.
Scheduled closes share one planner: flat time acts first on same-transaction ties,
and a pending close from an earlier trigger keeps its label without duplication.
At rollover, `OVERNIGHT_HOLD` precedes `HOLD_RESTRICTED`. See
[combined rule semantics](paper-trading.md#news-blackouts-and-holding-restrictions-f17-f59).
