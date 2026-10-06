# API and scripting

The HTTP routes the web terminal uses, by area. The [OpenAPI contract](openapi.yaml)
is the full reference, and [paper trading](paper-trading.md) documents every order
field, rule and reason code.

Money is exact decimal text, never a JSON number. Strings preserve every meaningful
micro-dollar with 2 to 6 decimal places, omitting trailing zeros beyond cents.
Computed requirements, buying power under portfolio margin and prorated values can
carry more than cents: for example, available buying power may be `"2033.725875"`
beside cash of `"100000.00"`. Clients should parse money as exact decimals and round
only for display (the terminal shows that buying power as `$2,033.73`). Booked values
remain exact integer micro-dollars.

## Status and market data

Status and WebSocket `circuit_breaker` include optional `inactive_reason`
(`MISSING_PREVIOUS_CLOSE` or null). Halt level 0 identifies an authored market-wide
scenario halt; levels 1–3 are price-triggered breakers. The terminal shows the
cause and resume time in New York time, or "for the rest of the day" when the
halt reaches the regular close. See [stress scenarios](scenarios.md#events).

| Route | Returns |
| --- | --- |
| `GET /api/status` | Running `version`, provider, market and per-underlying sessions, feed health (including the demo day title), engine counters, notification delivery status (no secrets), and optional sandbox availability |
| `POST /api/notifications/test` | Queue a test for `{ "channel": "ID" }`; requires admin |
| `PUT /api/notifications/channels/ID` | Change a channel's enabled state, event filters and floor distance for this process; requires admin |
| `GET /api/underlyings/{symbol}/summary` | Spot and its source, expiries with forward, rate and its source, ATM IV, GEX, VEX, coverage and `last_trade` / `auto_close` UTC ISO times |
| `GET /api/underlyings/{symbol}/chain?expiry={id}` | Every strike with both sides' quotes, IV, Greeks, early-exercise premium, `volume` (session contracts or null) and whether paper orders can fill on each quote (`executable`, `quote_issue`); coverage includes `volume` |
| `GET /api/underlyings/{symbol}/exposure?expiries=8` | GEX and VEX by strike and expiry, flip and walls |
| `GET /api/underlyings/{symbol}/surface?expiries=12` | Smile points per expiry |
| `GET /api/underlyings/{symbol}/volatility` | Current model-free IV, ATM/skew, realized vol, cones, implied moves, IV rank/percentile and ex-ante/ex-post VRP, with sources and history counts |
| `GET /api/underlyings/{symbol}/series?fields=mfiv30,atm30,rr25&from=&to=&interval=1d` | Selected metric history at `1m` or `1d`; bounded date ranges, null gaps and per-value sources ([fields and limits](volatility.md#series-api)) |
| `GET /api/underlyings/{symbol}/candles?interval=5m` | OHLC bars at 1m, 5m, 15m, 30m, 1h or 1d, oldest first |
| `GET /api/underlyings/{symbol}/probability?days=0,1,7,30&prices=` | Probability cones (one and two standard deviations) from the at-the-money term structure, and each price's odds of finishing above or below it or touching it by each horizon ([model](paper-trading.md#risk-profile-and-probabilities)) |
| `WS /ws` | A small tick each second with versions, so clients refetch only what changed; at most 256 open sessions, after which an upgrade gets 503 with `Retry-After: 5` |

Expiry ids are the date plus settlement, for example `2026-10-16AM`.

## Paper trading

Custom account/reset `rules` optionally accept `time_limit_days`, `inactivity_days`
(0–366, zero off), `underlyings` (up to 32 uppercase symbols; empty allows all),
and `trading_start`/`trading_end` (both `HH:MM` New York or null). The time limit is
refused in funded plans. Account `evaluation` reports `time_limit_days`, `deadline`,
`days_left`, `last_activity`, `inactive_days`, `inactivity_deadline` (null when off).
`TIME_LIMIT` and `INACTIVITY` fail overdue attempts; `INSTRUMENT_NOT_ALLOWED` and
`OUTSIDE_PLAN_HOURS` refuse openings while permitting reductions. Hours refusals
include `actual`/`limit` as `HH:MM` New York time strings and the underlying `scope`.
Invalid values for these custom rule fields return HTTP 422 `INVALID_RULES`,
with a message naming the field and its allowed values, on both create and reset.
Custom plans also accept `flat_time` (`HH:MM` New York, `00:00`–`23:59`, strictly
before `day_end`, or null) and `no_overnight` (boolean, default false). Invalid
values return 422 `INVALID_RULES`. Account `evaluation.flat_time` reports the time
or null and `flat_now` reports the opening block until day end. `FLAT_TIME` refuses
openings with HTTP 422, `actual` = current NY `HH:MM`, `limit` = flat `HH:MM`,
`scope` = `account`; opening previews also return HTTP 422 with that error.
Reductions remain allowed. Mandatory closes use `system:flat_time:N`; held
positions at rollover fail with `OVERNIGHT_HOLD` and close as `system:overnight:N`.
Shares use `rule` stock fills; settlement-pending options are excluded.
See [mandatory flat rules](paper-trading.md#mandatory-flat-time-and-no-overnight-holds-f6).
See [time rules](paper-trading.md#evaluation-time-inactivity-and-opening-restrictions).

With paper trading on, the same API is the account.

Create/reset accepts optional `fee_model: "flat" | "itemized"` or custom
`rules.fees` with open/close commission, per-leg cap, clearing, regulatory,
root-specific index fees and per-contract exercise fees. The preset overrides
custom fees. Fills and order/change previews return itemized `fees` (null under
flat fees); preview `fee` is the full remaining size’s total, null without a
projection. See [fee semantics and bounds](paper-trading.md#fees).

Example schedule for custom rules (omitted fields default to zero):

```json
{"fees":{"open":"1.00","close":"0.00","leg_cap":"10.00",
         "clearing":"0.10","regulatory":"0.02",
         "index":{"SPX":"0.60","SPXW":"0.60"},"exercise":"5.00"}}
```

`GET /api/account` returns a funded account's next payout quote in `payout` (null
outside the funded phase). `consistency_percent` is the next payout's optional
limit; `cycle_profit`, `best_day: {day, profit}` (nullable) and `consistency_needed`
show net realised cycle consistency. `buffer_balance` is the equity to retain
(starting balance plus buffer), null when it does not apply. `POST /api/account/payout` takes
`{"amount":"100.00"}` and returns the updated account. An unmet consistency rule
uses `PAYOUT_NOT_ELIGIBLE`, after qualifying days and before the minimum check.
Custom funded rules accept `payouts.consistency_percents` (empty disables;
1–100 integers indexed by payout number, last repeating), `buffer` (nonnegative
decimal money, zero disables), and `buffer_payouts` (0–100, zero means every payout).

Custom rules and backtest custom plans accept optional `scaling`, for example
`[{"profit":"0.00","contracts":2},{"profit":"1500.00","contracts":3},{"profit":"2000.00","contracts":5}]`.
Empty disables; at most 16 steps, first profit zero, profits strictly increasing,
contracts non-decreasing integers from 1 to 10,000. Invalid steps return 400
`INVALID_RULES`, wrong types 400 `INVALID_REQUEST`. Both phases support it.
`evaluation.scaling` is null without the rule, else `{limit, held, profit, next}`;
`next` is a `{profit, contracts}` step with a higher limit, or null. Profit is
closed balance less starting balance. Rollover selects the next session's limit;
profit during the day cannot raise it immediately, and losses or payouts can lower it.
Each option leg counts; shares do not. Working opening orders reserve capacity.
Excess increases return `SCALING_LIMIT` with actual/limit evidence, including in
previews and what-if; fills recheck and cancel with that code. Reducing orders
remain allowed. There is no grace period. The appended `funded-scaling-50k` preset
requires a passed `intraday-50k` evaluation, like the other funded preset gates.
See [Scaling plan](paper-trading.md#funded-accounts-and-payouts).

Custom funded reset and backtest rules accept nullable `size_scaling`:
`{profit_percent, payouts, days, increase_percent, max_balance}`. Counts are whole
numbers: profit/increase 1–100, payouts 0–100, days 1–366; maximum balance is a
money string at least the starting balance. Evaluation-phase use or invalid ranges
are `INVALID_RULES`; wrong types are `INVALID_REQUEST`. Disabled API rules expose
null. Every completed review restarts; qualifying active accounts receive linear
capital growth from the original balance. `evaluation.size_scaling` exposes size,
original/max/next size, review start/day count, net profit and payouts versus
requirements, and `{day, old, size}` history (schemas `SizeScaling`,
`SizeScalingStatus`). Account creation refuses custom funded rules: a funded account
starts from an account that passed its evaluation. Withdrawals are added back to
review profit; growth is not P&L. Resetting with the original starting balance and
unchanged displayed rules restores original loss and lock amounts from scaling
progress, retains the reset allowance and charges the reset fee.

The web terminal uses these routes, so anything it does can be scripted:

| Route | Does |
| --- | --- |
| `GET /api/portfolio`, `/api/orders`, `/api/fills`, `/api/risk`, `/api/account`, `/api/trades` | The account's positions with their margin breakdown, orders and fills with actors (each fill with the bid, ask and sizes it took and the quote's age), risk, warnings and breach estimates, rules and progress, and its round trips |
| `GET /api/risk/profile?underlying=&benchmark=SPY&days=0,1,expiry&iv=&range=10&steps=41&betas=` | The held book's P&L curve across moves of one underlying, or of SPY or SPX with each underlying beta-weighted to it, today and on later dates with a volatility offset; where each curve reaches the floors, with touch odds, and beta-weighted delta ([details](paper-trading.md#risk-profile-and-probabilities)) |
| `GET /api/orders/{id}` | One order with status, cancellation reason, note and changes; also available under replay and replay history. Unknown ids are 404 `UNKNOWN_ORDER` |
| `POST /api/orders/preview` | A pure order check, buying power, Greeks change, maximum loss, plan trade risk and its limit, size to buying power and to floor with the binding `max_units_basis` (floor, buying_power, limits or null), projected breach risk, and warnings about terms that act at once |
| `POST /api/orders/what-if` | Up to six candidate adjustments of up to four orders each, filled on a private copy of the account and compared on buying power, Greeks, grid loss and floor room |
| `POST /api/orders/{id}/preview` | The same preview of a change to a resting order, as `PUT /api/orders/{id}` would make it, without making it |
| `POST /api/stocks/trade` | `{symbol, side: "buy" or "sell", shares: 1–10000000}` opens, adds, reduces or reverses shares at a fresh underlying price during stock regular hours; returns Portfolio |
| `POST /api/stocks/trade/preview` | Same body; returns `decision` (`ok` or reason code), `reason`, price, signed cash cost, and current/after buying power and dollar delta without recording a trade |
| `POST /api/stocks/close` | `{symbol, shares?}` reduces only, all held shares if omitted; an oversized close is refused |
| `POST /api/positions/close/preview` | A flatten's dry run: what it would cancel and close now, and the account after it, without doing it |
| `GET/POST /api/playbooks`, `GET/PUT/DELETE /api/playbooks/{id}` | Versioned definitions, archive, account stages, forward-test windows/results and adherence/expectancy reports; `?version=N` reads an old version |
| `PUT /api/playbooks/{id}/mode`, `POST /api/playbooks/staged/{stage}/send`, `/dismiss` | Enable staging or automatic live paper/replay trading; send or dismiss a current stage |
| `GET /api/backtests/compare?ids=A,B` | Compare 2–8 finished/partial single or joint reports and sum independent run daily P&L; `combined.includes_joint` flags joint members; input/plan mismatches are flagged |
| `GET/POST/DELETE /api/backtests`, `GET/PUT/DELETE /api/backtests/{id}` | Start a batch with exactly one of `playbook: "ID[@VERSION]"` or joint `playbooks: ["a@2", "b"]` (2–8 distinct IDs, lexical ID order), watch progress, read reports, pin, cancel or purge (`?purge=true`); mutations need replay scope |
| `GET /api/account/pass-odds?days=N&samples=M&playbook=ID&seed=S` | Seeded estimate from historical equity days, not a prediction; needs ten days with intraday extremes (422 `PASS_ODDS_UNAVAILABLE` otherwise) |
| `GET /api/strategy-template?symbol=SPX&expiry=ID&template=JSON` | Shared server leg selection for terminal templates and playbooks |
| `GET /api/account/equity?from=&to=` | Persisted minute and fill equity, floor, high-water mark and target; optional UTC ISO time bounds |
| `POST /api/orders`, `PUT /api/orders/{id}`, `DELETE /api/orders/{id}` | Place an order (one contract, or `legs` for a strategy), attach exits to a held contract or spread with `exits_only`, change it or cancel it |
| `POST /api/orders/cancel`, `POST /api/positions/close` | Cancel every open order, one underlying's or a listed set (such as an exit pair) together, or flatten one underlying or all |
| `GET /api/settlements` | Account settlements, newest first across attempts: exact reference, signed quantity and cash, gross realised P&L, fee and nullable source; read scope |
| `POST /api/settlements` | Import an expired position’s decimal-string reference `{symbol, value}`; admin scope |
| `POST /api/positions/abandon` | Give up a long at zero without a fee: `{symbol}`, allowed with a fresh ask-only quote or after expiry |
| `POST /api/positions/instruction` | Set or withdraw a long's expiry instruction: `{symbol, do_not_exercise}`; true forfeits shares and cash settlement. Both routes return the portfolio and require trade scope (replay scope on the replay mirror) |
| `GET /api/trades.csv`, `/api/fills.csv` | Trades or fills, with context and excursions, filtered by account and New York `from`/`to` dates. Trade CSV includes `settlement_value` and `settlement_source` before the final `time_stop_orders` column for related playbook cancellations/closes. Fills include `fees.commission`, `fees.clearing`, `fees.regulatory`, and `fees.index` columns (empty for flat fees) |
| `PUT /api/days/{YYYY-MM-DD}/note` | The account's plan and review for a day; returned in `/api/trades` as `day_notes` |
| `PUT /api/trades/{id}/note` | A trade's note and tags, or a share trade's (`s1`, ...) |
| `GET/POST /api/alerts`, `DELETE /api/alerts/{id}` | The account's alerts on a contract, spread legs, an underlying or account measures, kept and checked by the server and forwarded to notification channels |
| `POST /api/trades/group`, `/api/trades/ungroup` | Join open round trips' trades into one whole trade, or take a round trip out of its trade |
| `PUT /api/risk/limits`, `PUT /api/risk/guardrails`, `POST /api/risk/kill` | Set per-symbol `underlying_overrides` (a full replacement map, omitted/empty removes all); tighten rules now or queue looser values for rollover; set personal guardrails; trip or reset the kill switch |
| `GET /api/plans`, `POST /api/account/reset` | The plans, and a new attempt; optional `fill_model` selects `as_displayed`, `conservative` or `midpoint`, and optional `margin`, `account_type` (`margin`, `cash`, `ira`), `house_margin_percent` and `pm_vol_shock` set the account's margin |
| `POST /api/sandboxes` | Create a private demo practice account and return its token once; unauthenticated when enabled, 404 when off, 429 at capacity or a creation rate limit |
| `PATCH /api/accounts/{id}`, `DELETE /api/accounts/{id}` | Admin: rename/archive/unarchive or delete named live accounts; archived accounts are readable but frozen, `GET /api/accounts?archived=true` includes them; deletion retains files and reserves the ID |
| `GET /api/accounts`, `POST /api/accounts` | List the accounts or create one (`plan_id` accompanies each plan name; null for custom evaluation/verification rules; custom funded rules are refused), with optional `copy_settings_from` (active limits and guardrails), `fill_model` and margin settings as for a reset; account routes take `?account=ID`; sandbox tokens default to their own account, other credentials to main |
| `GET`, `POST`, `PUT`, `DELETE /api/replay` | List recordings, scenarios and run history; start `{file}` or `{scenario}` (`demo` also accepted), with `plan`, `speed`, `start_at`, `paused` and scenario `seed`/`date`, or continue an interrupted saved run with `{resume}`; control or stop. `/api/replay/X` mirrors `/api/X`; replay scope also permits reset, limits, guardrails and kill on the isolated account |
| `PUT /api/replay {"until":"HH:MM[:SS]"}` | Advance through a New York session time (in a scenario of several sessions, its next occurrence at or after the replay's time), or a date and time such as `2026-09-17T10:30` (New York unless zoned), then pause; responds after analytics and trading settle, with `settled_through`. Meanwhile `stepping` is true and replay writes return `REPLAY_STEPPING`; a target past the recording's end returns 400 and plays nothing |
| `PUT /api/replay {"until":"+15s"}` | Relative whole-second `+Ns`, `+Nm`, `+Nh`, or `next` for one complete snapshot/batch; past EOF is refused before moving |
| `PUT /api/replay {"play_until":"14:30:15","speed":60}` | Paced playback through the target; returns immediately with `pause_at`. Same targets as until, optional speed. Pause, speed, until, abort and stop clear it |
| `PUT /api/replay {"abort":true}` | Interrupt a step at its next settled batch and pause. `paused:true` and DELETE also interrupt; the step returns `aborted:true` |
| `PUT /api/replay {"skip":true}` | While paused, queue visibly as `skip_pending:true` for the next resume; `skip:false` cancels |
| `POST /api/replay {"restart":"ID","at":"10:30:15"}` | New run, same inputs/settings and journaled commands through T inclusive, paused by default; optional speed/paused, at omitted uses source start. Source journal kept; state/history carry `restarted_from:{id,at}` |
| `GET /api/replay/history` | Saved runs with final `journal` head/count/bytes, `journal_found`, and `torn`/`bytes_cut` or `truncated`/`mismatch` integrity warnings |
| `POST /api/replay/history/ID/verify`, `GET /api/replay/history/ID/verify` | Start background verification (202, replay scope) or read `idle/running/passed/failed`; GET `?format=receipt` downloads the final JSON result |
| `GET /api/replay/history/ID/X`, `DELETE /api/replay/history/ID` | Read a finished run's account, equity history (`account/equity`), portfolio, trades, fills or settlements; delete its journal |

Account attempt history (`GET /api/account`, top-level `attempts[]`) includes the
full `rules` at reset, `decided_at`, `decided_equity`, `peak` and `floor`; older
journal summaries return null for these unrecorded values. Passed and failed
attempts accept closing orders, Flatten and disposal, while opening orders still
return `EVALUATION_CLOSED`. `GET /api/account/equity` includes the exact decision
observation before any same-time liquidation samples.

Personal guardrails accept `soft_floor_percent` from 0 to 99 on new writes; 100 returns
`INVALID_LIMITS` because it can latch at the peak immediately. Stored 100 settings remain
readable. `SOFT_FLOOR_UNUSED` is an informational risk warning for a percent floor on a
plan without drawdown. Account resets return the account's usual `warnings`, including
`SOFT_FLOOR` when a retained absolute floor meets or exceeds the new balance. Resets
apply pending settings while preserving daily profit/trade latches, opening-order
count and active cooldown; they clear and re-check the equity-based soft floor.
`kill.reset_blocked` describes the kill-switch reset, which cannot bypass any active
personal guardrail; it does not describe an account reset. Kill history records actual
changes, so retaining the same latch across an account reset adds no reset entry.
Opening orders count at their first opening fill only, even through partial fills across rollover. Trader-triggered closing stops start
the configured cooldown when the trigger direction is adverse to the held position;
profit-taking triggers do not. Trailing stops and bracket stop-losses always count.
A buy-only long-put exercise that would create short shares
returns `BUY_ONLY`; expiry delivery is unchanged. See [Personal guardrails](paper-trading.md#personal-guardrails).

Margin settings are returned in account rules. Invalid values or a cash/IRA account
without strategy margin and enforced buying power return HTTP 400 `INVALID_RULES`.
Account-type trading restrictions reject with `ACCOUNT_TYPE`.

This calendar buys the later put and sells the nearer one at a net debit of at most
6.60:

```bash
curl -X POST localhost:8080/api/orders -H 'Content-Type: application/json' -d '{
  "client_order_id": "calendar-1", "type": "limit", "quantity": 1,
  "limit_price": "6.60", "time_in_force": "day",
  "legs": [{"symbol": "SPXW  260925P07700000", "side": "buy"},
           {"symbol": "SPXW  260923P07700000", "side": "sell"}]}'
```

Orders accept `time_in_force: "exto"`, `"gtc_exto"` or `"gtd"` in live and replay
entry/preview routes. EXTO ends with the trading date's last session; GTC_EXTO
works all sessions until expiry/cutoff. GTD trades regular hours until `good_till`,
for example `"2026-09-23T14:15:00Z"`, a future market-time timestamp within 366 days.
Only GTD takes that field. Extended stops are simulator-held triggers that send
limits outside regular hours; stop-limit retains its price, while stop-market
executes once at the touch against displayed size. Extended entries' bracket exits
inherit all sessions. Plain market entries remain IOC and regular-only.

`PUT /api/orders/{id}` and `POST /api/orders/{id}/preview` reject a quantity below 1
with 400 `INVALID_REQUEST` and record nothing, including on replay mirrors. A positive
quantity at or below the filled quantity remains a recorded 422 `INVALID_ORDER` on PUT,
with the requested and filled quantities.

POST orders and PUT order changes take an optional `walk: {step, seconds, limit}`: money values are decimal strings, seconds is an integer from 1 to 3600.
Only untriggered DAY/GTC limits may walk; managed exits cannot. PUT omission keeps
the walk and `null` removes it. Orders report the current `limit_price`, `walk`,
and `next_walk: {time, limit_price}` (null when finished, at cap or past its deadline); previews also
return `next_walk`. Steps are market-time order changes with actor `walk`.
Combo nets, including negative credits, walk upward; single-leg sells downward.
Custom account rules accept integer `inside_fill_percent` (0–100, default 0,
omitted in responses at zero); `fill_model: "midpoint"` sets 50 with other execution
friction off. See [fill models and walks](paper-trading.md#walking-limits-f46).

`POST /api/positions/close` and `/api/positions/close/preview` take optional
`type: "limit"` and `limit_ticks: 0` (0–10). Limits reprice from each leg's touch,
work across quotes through the trading date and allow overnight/curb flattening.
A manual limit modification stops repricing. Market remains the default. Orders
report nullable `good_till` and `limit_ticks`; replay mirrors accept the same fields.
Python: `client.place_order(..., time_in_force="gtd", good_till="2026-09-23T14:15:00Z")`
and `client.flatten("SPX", type="limit", limit_ticks=1)`; `preview_flatten` takes the
same pricing arguments.

Sending the same order again with the same `client_order_id` is safe: it returns the
first answer instead of placing a second order, and `GET /api/orders?client_order_id=ID`
finds the order an ID placed. Other terms under a used ID get 409 and record nothing;
an account reset frees the IDs earlier attempts used. A 503 for a full command inbox carries
`Retry-After` seconds; wait that long and send the same body again. [Paper trading](paper-trading.md)
documents every field, rule and reason code.

Sandbox demos also offer `POST /api/sandboxes` without a token. It returns
`{account, token, idle_seconds, simulated: true}` once; it returns 404 when disabled
and 429 at capacity or a creation rate limit. `GET /api/status` includes
`sandboxes: {enabled: true, idle_seconds}` when offered.

Share trades and previews have `/api/replay/stocks/trade` and
`/api/replay/stocks/trade/preview` mirrors, and accept `?account=` live. Trade scope
(or replay scope) is required. Index underlyings are refused. Opening takes the
account's valuation, session, loss, exposure, buying-power and personal checks;
reducing remains available under the kill switch. A reversal is a close plus an
open, accepted or refused as a whole. Share writes have no idempotency key: inspect
positions after an uncertain response before retrying. Preview costs are decimal
money strings; a negative cost receives cash. A refused preview leaves holdings
unchanged. See [shares](paper-trading.md) for margin and defined-risk plan coverage.

## Scripting and agents

The [OpenAPI 3.1 contract](openapi.yaml) describes the terminal API, including
replay mirrors, errors and nullable data. CI validates responses from a running
container with `tools/contract_test.py`, including an order preview and a resting
limit placed and cancelled in an isolated simulated replay.

The [Python package](../python/README.md) has no runtime dependencies. `Client` reads
analytics and accounts, places paper orders and steps replays on market time.
Orders get a client ID; bounded HTTP 503 retries reuse it. Optional extras provide
pandas frames, WebSocket ticks and an MCP server using the official 2.x SDK.

The MCP server gives agents the same paper API. Every tool reports market time,
the provider and its delay, and whether prices are simulated. Writes require
`OPENPORT_WRITE_TOKEN` and an account name (optional for sandbox tokens). Orders carry an `agent:NAME` tag;
authenticated token names are recorded as actors on journal transactions, orders
and fills. Older journal entries show `unknown`.

Use `--token-file FILE` to give each script its own token. For example, a line
`research read,trade:practice,replay SECRET` permits reads, trading the practice
account and replay controls. Add `--require-token` to protect reads and loopback
writes as well. Named credentials reload on file mtime/size changes, `SIGHUP`, or
admin `POST /api/tokens/reload` (`{}`), which returns `names`, `count`, `loaded_at`
and never secrets. Malformed replacements keep the last good set; an empty
replacement revokes all named tokens. Revoked/changed file-token sockets close
before their next queued tick; accepted commands and in-flight messages may
finish. Legacy and sandbox tokens are unaffected. Status includes optional
`tokens: {loaded_at, count}`. Python wraps reload with `Client.reload_tokens()`.
See [Python setup and examples](../python/README.md).

Replay starts also accept `copy_settings_from` (live account ID, including `main`).
Replay risk replies expose `pending_requires_reset` when queued settings need an
account reset because no later trading day occurs in the run.

Status `trading.write` describes the server's write policy: `open` accepts writes
without credentials (loopback with no legacy token, `--require-token` or sandboxes;
named tokens alone leave it open), `token` requires a credential with the route's
scope, and `disabled` means no credential can write on that non-loopback bind.
A supplied token is always checked, even in `open` mode.

Playbook definitions optionally accept `conditions.technical`, `vix`, `gap` and
`management.trailing_stop`, `close_at_dte`, `max_days_in_trade`, `stop_loss_percent`.
[Playbook rules](playbooks.md) specify strict validation, indicator history and exit
semantics. Pinned backtest definitions retain these optional fields; old reports
remain readable. Adherence rule keys are extended only for configured exits.

Operational storage fields: `/api/accounts`, `/api/account` and status `accounts[]`
include `damaged: null | {reason, last_good_seq, last_good_time}` and
`journal_size: {bytes, records, warning}`. Damaged accounts keep their last verified
portfolio, fills and trades visible and refuse writes with 409 `ACCOUNT_DAMAGED`.
Zero sequence/null time means no verified record. Stop the server for
`--repair-journals --dry-run`; torn suffixes can be repaired, while mid-file damage
requires a verified backup. Low disk errors identify a filesystem device and
directory basename, available bytes and the 64 MiB reserve in the trading reason.

`GET /api/account/equity?limit=2000&cursor=...` pages equity history. `limit` must be
1–2000; `cursor` requires a limit and is the preceding response's opaque `next`.
Keep the same account and inclusive `from`/`to` filters while paging. Equal-time
samples (including combo fills) are distinct. `next: null` ends the range; omit
`limit` for the original unpaged response. These parameters also work under
`/api/replay/` and `/api/replay/history/{run_id}/`. This is a live history, not a
snapshot token: appends may appear and retention can remove old samples between
requests. Equity storage retains `error` as string/null and adds `error_time`
(wall UTC), `error_market_time` (nullable sample UTC) and `error_recovered`.
Later successful storage keeps the last error and timestamp while marking it
recovered; that error history lasts for the running process.

Journal warnings begin at 256 MiB or 100,000 verified records. Live accounts
suggest offline `--compact-journals`; replay warnings note that compaction breaks
exact run verification. Replay history adds `journal_size` and
`verification_cost: {estimated_seconds, warning}`; verification responses add
`cost` in the same shape. CLI and HTTP estimates use the maximum of the journal estimate
(the slower of 10 MiB/s or 2,000 records/s, minimum one second) and an input
estimate: (4 seconds + 5 seconds per played hour) × max(1, scenario underlyings / 3).
Played time is the journal start through its last market time or a later sidecar
`settled_through`. Scenarios count only time within their recorded-calendar sessions,
excluding closed nights/weekends; recordings count the continuous played span
without underlying scaling. If input metadata cannot be determined, only the journal
estimate is used. Seconds round up; warnings begin at 30 seconds. They are estimates,
not bounds: recording generation, analytics and hardware can take longer.
The terminal displays the warning before the trader presses Verify.

Custom account/reset rules accept optional `max_contracts_held`, `require_stop_loss`,
`max_trade_risk` (decimal string or null) and `max_trade_risk_percent`. Defaults turn
all four off. Opening previews report `trade_risk`, `trade_risk_limit` and
`trade_risk_basis`. Refusals use HTTP 422 `MAX_CONTRACTS_HELD`, `STOP_REQUIRED` or
`MAX_TRADE_RISK` with the usual `actual`, `limit`, `scope` evidence: contract counts
with scope `account`, or dollar amounts with scope `trade` (actual null for
unbounded/unknown risk). See [trade rules](paper-trading.md#account-rules-and-evaluations).

Evaluation and verification enforce the same pass objectives and trading restrictions below, including in backtests. Funded plans retain their restrictions and ignore trade consistency, minimum trades and microscalping pass objectives.

Custom account/reset and backtest rules accept `trade_consistency_percent` (integer 0–100). Account evaluation exposes nullable `best_trade: {id, pnl}` and `TRADE_CONSISTENCY` objective evidence (actual/required percent, message with trade ID and net P&L). Funded plans ignore the objective.

`min_trades` (integer 0–10000) is also accepted on custom account/reset/backtest rules. Evaluation reports nullable `closed_trades` and `MIN_TRADES` actual/required counts, including system closures; funded plans ignore it.

Custom account/reset/backtest rules accept `min_hold_seconds` (0–3600) and the paired `microscalp_seconds` (1–3600) / `microscalp_percent` (0–100); percent zero disables microscalping. Option/share order preview and submission report `MIN_HOLD` with `actual` seconds held, `limit` seconds required and underlying `scope`; protective/system exits bypass it. Evaluation exposes `MICROSCALPING` percentage evidence and nullable dollar `short_profit`.

Custom plan rules also accept boolean `no_hedging` and `no_counter_positions` (default false). `HEDGING` is a reducer refusal; `COUNTER_POSITION` refuses acceptance before order journaling and cancels resting orders at fill. Cross-account checks compare held delta and each working opening order separately. Both return HTTP 422, and previews return the same reason with structured direction `evidence` (underlying, order/held dollar delta, and other account ID for cross-account checks). Pure reductions are exempt. Only the ordering account’s cross-account setting counts; see [direction rules](paper-trading.md#direction-rules-f65).

`max_volume_percent` is an optional whole percent 0–100 (0 disables) on custom account/reset/backtest rules. Acceptance and each proposed fill check each option leg’s absolute held plus opening contracts against current-date volume, with exact integer arithmetic. Missing/stale volume refuses. `MAX_VOLUME_SHARE` returns HTTP 422 and `evidence: {contract, contracts, volume, percent}` (volume null if unknown); `actual` is held plus opening contracts and `limit` is `percent × volume / 100` unrounded, so it may be fractional. Previews report the same reason. Resting orders that would break the cap cancel with `MAX_VOLUME_SHARE` and its evidence; completed partial fills remain. Scenario backtests support the rule using current-revision generated option volume. Recording days without usable current-date volume reject the enabled rule with HTTP 400. See [volume-share rule](paper-trading.md#volume-share-rule-f66).

### Saved event calendars and opening restrictions (F17/F59)

`GET /api/calendar/events?from=2026-09-01&to=2026-10-01` (read scope) returns
`{"events":[...]}` from `--event-calendar`. Filters accept dates or ISO timestamps;
from is inclusive and to exclusive; corporate dates compare at UTC midnight.
Unknown/duplicate parameters, bad dates and reversed ranges return 400.
Create/reset rules accept `events`, `news_before_minutes`, `news_after_minutes`,
`news_action`, `hold_restrictions`, `hold_cutoff`; invalid values return 422
`INVALID_RULES`. Rules round trip in account, plan and attempt responses. Backtests
accept the same fields. Account evaluation exposes `next_event` and `active_events`.
NEWS_BLACKOUT HTTP evidence contains UTC actual/limit strings; HOLD_RESTRICTED has
actual null, cutoff HH:MM limit, and scope `kind:underlying` or `kind:account`.
The underlying summary's `expiries[].dividends[].ex_date` can be imported into
saved ex_dividend events. See [paper trading](paper-trading.md#news-blackouts-and-holding-restrictions-f17-f59).

Combined F6/F17/F59 rules round-trip together through create, reset and archived
attempt rules. If multiple opening restrictions apply, `FLAT_TIME` is checked
before calendar restrictions. `OVERNIGHT_HOLD` takes precedence over
`HOLD_RESTRICTED` at rollover. See [combined rule semantics](paper-trading.md#news-blackouts-and-holding-restrictions-f17-f59).

### Two-step programs and program costs (F27/F64)

Multi-phase programs (F27) use `rules.phase: "verification"` for the second step.
Plans return `phase`, `unlocked_by` and `unlocks`; account views return `next_plans`
(preset IDs, empty without a current unlocking pass). The `two-step-{25,50,100}k`
challenge presets unlock `two-step-verify-{size}k`, then `two-step-funded-{size}k`.
Create-account and replay start refuse locked presets; enter them by resetting
the account that passed. Read-back rules with a locked preset name obey the same lock.

Program costs (F64) are exact decimal money: create/reset accepts `evaluation_fee`,
`reset_fee`, `activation_fee`, and integer `max_resets` beside `plan` or in custom
rules. Zero fees and `max_resets: 0` (unlimited) are defaults. These costs do not
change paper cash. Same-plan resets retain omitted cost settings. A fresh purchase pays evaluation, the same plan pays reset,
and entry unlocked by a pass pays activation. Same-plan identity ignores execution,
broker margin and cost overrides. A same-plan reset over the requested limit returns
409 `RESET_LIMIT`, with attempted count in `actual` and allowed count in `limit`.
The account's `costs` reports amounts paid by kind, `total`, `resets_used`,
`resets_left` (null for unlimited), `payouts_received`, `net`, current `fee_charged`
and `fee_kind`. Attempts expose their starting charge and phase in `rules`.
See [program costs](paper-trading.md#program-costs-and-reset-limits-f64) for precise
identity, purchase, historical-journal and illustrative-pricing semantics.
