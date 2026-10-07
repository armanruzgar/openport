# Features

What OpenPort does today. The [README](../README.md) has the short version;
[paper trading](paper-trading.md) specifies the rules and simulation limits.
All orders and accounts are simulated. On the default 15-minute delayed feed,
a pass is practice, not proof: a real-time chart shows what happened next.

## Market analytics

- **Chain**: bid, mid and ask with their implied volatilities, delta, gamma, vega,
  theta, vanna, open interest and session volume. Spread and liquidity cues appear
  beside each side, with the provider's IV alongside when supplied. Missing quotes,
  volume and open interest stay missing, with coverage counts per expiry.
- SPX AM-settled and SPXW PM-settled series stay separate even on the same date.
  The chain opens on the nearest expiry before auto-close and counts down to it
  on the market-data clock.
- **Smile and term structure**: out-of-the-money smiles with SVI fits and arbitrage
  checks, ATM term structure on a square-root-of-time axis, and each expiry's
  forward, rate and source. Forwards come from put-call parity; equity-option IV
  accounts for American exercise.
- **Volatility metrics**: model-free IV, delta risk reversals and butterflies,
  realized volatility and cones, variance risk premium and implied session moves
  with optional event labels. Local minute and close history adds IV rank and
  percentile, labelled index proxies and ex-post variance risk premium for SPX
  (SPY by proxy). Missing, truncated and proxy estimates are marked.
  [Definitions](volatility.md).
- **Exposure**: gamma and vanna by strike and expiry, total gamma profile, gamma
  flip, call and put walls. Dealer exposure assumes dealers are long calls and
  short puts; it does not reveal their actual positions. [Methods](methods.md).
- Pricing includes Black-76, Black-Scholes-Merton, a safeguarded IV solver,
  Cox-Ross-Rubinstein and Leisen-Reimer trees. [Architecture](architecture.md).

## Trading terminal

- **Brief** brings together prior-session and observed overnight levels, implied
  moves, volatility changes, positioning and supplied events, alongside floor
  room, loss allowance, guardrails and positions. It uses market time in replay
  too. Selected key levels can be drawn on Trade's chart and saved in the browser.
- **Trade** docks the order ticket beside the chain and the underlying's candle
  chart, from one-minute to daily bars, with Cboe history backfill. The chart shows
  held strikes, armed triggers and the selected expiry's expected move.
- **Dashboard** charts minute and fill equity against the target and floor. It
  shows remaining pass objectives, daily loss room, deadlines and estimated equity
  after closing the book. Positions groups held strategies with combined P&L and
  dollar delta, gamma per 1% spot move, vega per volatility point and daily theta.
- Light and dark themes and keyboard shortcuts are available. Number keys follow
  the sidebar; Brief is 7 with a paper account and 2 on analytics-only servers.
  Arrow keys step through expiries on Trade.

## Orders and fills

- Market and limit orders, DAY/GTC, all-session EXTO/GTC_EXTO and timestamp-bounded
  GTD use the product's session and expiry deadlines. Resting GTC orders wait
  outside the regular session; new orders are refused between product sessions.
  SPX, XSP, VIX and RUT also trade overnight (20:15–09:25 ET) and curb
  (16:15–17:00), with limit execution only. [Sessions](paper-trading.md#sessions).
- Conditional orders can wait for an option or underlying price, another symbol
  (such as VIX), a study (30-day IV or the 9/30-day IV ratio), or a time of day.
  Trailing stops use dollars, percent or ticks and bid/ask, mid or mark references.
  Missing trigger data does not trigger an order.
- Stop-limits and stop-loss/take-profit brackets are available on entry or on held
  single legs and spreads. Partial entries grow protection; partial exits shrink
  it to the position left. Spread stops can read the closing net or underlying.
  EXTO/GTC_EXTO protection uses simulator-managed limits overnight and in curb.
  [Conditional orders and brackets](paper-trading.md#conditional-and-bracket-orders).
- Standalone OCO orders cancel their partner on the first fill. OTO orders submit
  the next order only after a complete fill, with fresh checks; chains allow up
  to four orders. [Order chains](paper-trading.md#order-chains-one-cancels-other-and-one-triggers-other).
- Working size, limit and trigger levels can change in place. Orders retain
  acceptance, trigger, fill, change and refused-change history, ending reasons and
  the checks' numbers. Waiting orders explain whether they need a limit, trigger,
  session, fresh quote, displayed size or latency to clear. Cancellations ask for
  confirmation and keep their results visible.
- **Flatten** closes each short with its covering long, splits sizes above the
  order limit and leaves remainders working on later quotes until filled or their
  deadline. Brackets protect the remainder. Limit flatten follows the touch in
  overnight and curb sessions; shares still need their regular session. Its dry
  run and result show cancellations, closes, residual positions and reasons.
  [Changing, cancelling and flattening](paper-trading.md#changing-cancelling-and-flattening).
- A short quoted only at the ask can be bought back. Explicit abandonment removes
  an unsellable long at zero without a fee; selling a worthless wing through a
  combo exit or flatten still pays the usual fee. Long options can carry a
  do-not-exercise instruction. [Disposal](paper-trading.md#disposing-of-worthless-positions).
- **As displayed** fills take the far side up to its remaining displayed size.
  Resting limits normally need a later quote crossing their price. Single-leg
  limits cap the fill; combos wait if their slipped net exceeds the limit.
- **Conservative** adds one second of market-time latency, one slippage tick and
  one extra tick per additional displayed-size block. Custom execution allows
  0–10 slippage ticks, up to 60 seconds of latency and 0–10 impact ticks per block.
  **Inside at midpoint** allows limits at or beyond midpoint to fill inside the
  spread; custom inside fills use a deterministic share of the spread and remain
  bounded by the far side's remaining size. [Fill models](paper-trading.md#optional-fill-models).
- DAY/GTC single-leg and net combo limits can walk by a chosen step and interval
  toward a cap on market time. Tickets, previews, edits and history expose the
  walk, which survives replay and recovery. [Walking limits](paper-trading.md#walking-limits).
- Tickets warn about thin liquidity and identify one-sided, crossed or sizeless
  quotes and size already consumed. Each fill retains bid, ask, sizes, remaining
  displayed size and quote age. Entry notes and tags follow into the Journal.
- Fees can be flat per contract or itemized: open/close commission, per-leg caps,
  clearing, regulatory, index and exercise/assignment charges. [Fees](paper-trading.md#fees).
- These models simulate depth, not queue position, hidden liquidity or whether a
  real market would trade. Delayed data still gives hindsight.

## Strategies and shares

- Build up to four option legs on one underlying, including spreads, straddles,
  condors, butterflies, calendars and diagonals. Templates select strikes by
  width, delta or expected move; browser presets retain choices. Legs fill
  together in ratio at a net debit or credit.
- Tickets show P&L today and at expiry, the expected move and probability of profit
  from the smile's risk-neutral distribution, including skew. These are model
  estimates. [Multi-leg orders](paper-trading.md#multi-leg-orders).
- Close or roll a strategy together, to another expiry or strikes. Whole-condor
  rolls allow eight legs; put or call verticals of condors and iron butterflies
  can roll as four-leg orders. Rolls and adjustments retain one whole-trade review.
- Stock and ETF shares can be opened, added to, closed or reversed, with buying
  power and dollar-delta previews, dividends, recovery and trade review. The chain
  and Positions offer share tickets; shares execute synchronously in the stock
  regular session, without working share orders.
- Covered-call and collar templates buy shares first, then submit options. These
  are separate orders: shares remain if the option step fails or fills partly.
  Automatic playbook stages remain option orders; share purchases are manual.
  [Strategies with shares](playbooks.md#practising-strategies-with-shares).
- European index options settle in cash; American equity and ETF options deliver
  shares on exercise or assignment. In-the-money expiry exercise, requested early
  exercise and simulated partial overnight assignment are supported. Assignment
  can follow low extrinsic value or an approaching ex-dividend date.
- Dividends come from a supplied file or Massive's API, independently of the quote
  provider. Current scenarios include simulated quarterly SPY/QQQ dividends; the
  demo feed uses them without an explicit dividend source.
- Settlement references and sources appear in Journal rows, settlement lists,
  account reads and exports. New demo/scenario and recorded replay runs use the
  expiry opening print for AM settlement, an approximation of the official special
  opening quotation. Live providers require manual settlement imports.
  [Settlement](paper-trading.md#expiry-and-explicit-settlement).

## Margin and buying power

- Strategy margin allocates whole contracts across spreads, expiry pools, share
  covers and straddles to minimise the model's requirement. It certifies the exact
  optimum; hard work or arithmetic limits use an explicitly labelled conservative
  fallback. Positions shows each underlying's allocation and requirement.
- Long options and shares are paid in full. Covered calls need no further margin
  beyond their shares; protective puts cost their premium. Spreads and expiry pools
  recognise bounded payoffs, while naked shorts and short shares hold their model
  requirements. A long expiring before a short does not cover it.
  [Allocation model and proof](margin-allocation.md).
- Cash accounts require share-covered calls and cash-secured puts. IRAs can net
  spreads but cannot hold naked calls or short shares. Margin accounts support
  strategy margin or portfolio margin, with optional house requirements.
- Portfolio margin scans each underlying separately from −8% to +6% for indexes
  and ±15% for stocks/ETFs, with a $37.50 minimum per standard option contract and
  optional IV shocks. Buying power is equity less requirements and reservations;
  gains on another underlying do not offset a scan's losses. Missing scan inputs
  use a conservative fallback, visibly marked incomplete.
- Orders reserve buying power. Orders that free it remain available when it is
  exhausted, subject to other rules; closing a covering long may need to include
  the shorts it protects. [Account rules and margin](paper-trading.md#account-rules-and-evaluations).

## Evaluation rules

### Presets and decisions

- `practice` checks buying power without a target or floor. Intraday presets are
  buy-only with a 10% target and 5% trailing drawdown; end-of-day presets allow any
  strategy with a 12% target and 6% drawdown ratcheting at the close. Sizes are
  25K, 50K and 100K.
- Static presets use a closed-balance target, fixed floor, daily-loss failure and
  four trading days. Locking presets use a floor that stops at starting balance,
  daily-loss day lock and best-day consistency. These are illustrative project
  rules, not a claim to reproduce a particular firm's current offering.
  [Preset objectives](paper-trading.md#plan-presets-with-objectives).
- Touching the floor fails; passing requires the target and all pass objectives.
  Closed-balance targets also require a flat account. Dashboard lists what remains.
  Decided attempts still permit closing, flattening and worthless-long disposal;
  history retains the deciding equity/time, rules, peak and floor.
- Limits tighten immediately; looser evaluation limits wait for the next trading
  day. Personal soft floors, trade limits, cooldowns and profit locks can keep an
  account reduce-only independently of its plan. [Guardrails](paper-trading.md#personal-guardrails).

### Custom-plan rules

- Buy-only and defined-risk rules restrict opening strategies; evaluation presets
  auto-close five minutes before last trade. Presets default to strategy margin
  and As displayed fills, with execution and margin choices at account/attempt start.
- Daily loss can be measured from opening equity, opening closed balance, the
  higher of those, or the day's equity high. Touching the limit closes positions
  and either locks the day or fails the attempt. The plan sets its trading-day end.
- Pass objectives include minimum trading days, profitable days with a profit
  threshold, closed whole option trades, best-day consistency and per-trade
  consistency. Whole-trade consistency includes rolls and fees but excludes shares.
  Consistency holds back a pass rather than failing an attempt.
- Minimum hold time blocks premature user reductions; protective and system exits
  still work. Microscalping rules limit the share of attempt profit from positive
  short round trips. [Objectives and daily loss](paper-trading.md#plan-objectives-and-the-daily-loss-limit).
- Evaluation time windows and inactivity deadlines use calendar days; inactivity
  follows the trader's own executions. Allowed underlyings and New York opening
  hours restrict entries while leaving reductions available. Dashboard and tickets
  explain deadlines and restrictions. [Time and opening rules](paper-trading.md#evaluation-time-inactivity-and-opening-restrictions).
- Mandatory flat time starts closes and blocks openings until day end. A separate
  no-overnight rule fails positions held at rollover, excluding settlement-pending
  options. Scheduled closes and pending remainders survive recovery.
- Saved event calendars support news blackouts and weekend, earnings, ex-dividend
  and split holding restrictions. Blackouts can block entries or flatten; holding
  cutoffs initiate closes and remaining prohibited holds can fail at the boundary.
  Imports from the event catalogue or known dividends are frozen into the plan.
- Contract caps count every option leg and reserve working entries. Required-stop
  rules protect openings and prevent removing their stops while held. Trade-risk
  caps use dollars or a percentage of floor room. Stops can slip and stop-limits
  can remain unfilled; their assumed execution is not a guaranteed loss bound.
- Optional direction rules ban hedges within an account or counter positions
  across live accounts, using dollar-delta direction for options and shares.
  Cross-account bans must be enabled on each participating trading account;
  archived, replay and sandbox accounts are excluded from those comparisons.
- Volume-share caps limit contracts held to a whole percentage of current-date
  traded option volume. Unknown or stale volume refuses entries; replays and
  backtests need recordings or generated scenarios carrying that volume.
  [Complete custom rules](paper-trading.md#account-rules-and-evaluations).

### Two-step, funded phase and costs

- Two-step presets progress from challenge to verification, then funded simulation.
  A current pass unlocks the next matching step; earlier passes cannot be reused.
  Terminal confirmations and attempt history show the phase and prerequisites.
- Funded plans have no profit target. Payout rules cover qualifying days, withdrawal
  share, trader split, minimums, caps, cycle best-day consistency and a retained
  buffer, optionally limited to the first payouts. All payouts are simulated.
- Contract scaling changes the next session's limit from closed-balance profit,
  up or down, with every option leg and working opening counted. It is available
  in evaluation, verification and funded phases. Funded account-size scaling uses
  periodic profit/payout reviews, capped capital growth and proportional loss limits.
  [Funded rules and scaling](paper-trading.md#funded-accounts-and-payouts).
- The terminal hides funded plans and Payouts unless the account is already funded
  or `showFundedAccounts` is enabled in `web/src/lib/features.ts`. OpenPort funds no one.
- Evaluation, reset and activation costs and reset limits are recorded per attempt.
  They inform total costs and payout net; they do not reduce trading cash, change
  floors or alter evaluation outcomes. New attempts preserve the previous history.

## Risk tools

- Position Greeks, dollar-delta and vega limits, a spot × volatility scenario grid
  and today's P&L attribution by delta, gamma, vega and theta include contracts
  closed today, with costs apart. Positions are marked at mid.
- The reduce-only kill switch cancels opening orders while closing and bracket
  exits remain available. Its trip/reset/release history explains when it can clear.
  A book pushed over exposure limits can still be reduced or hedged; a close that
  removes a hedge can be refused if it adds to the excess. [Risk](paper-trading.md#risk-and-kill-switch).
- Order and change previews show maximum loss, buying power, immediate fills and
  the full-size fill schedule. Suggested size names its binding floor, buying-power
  or limit constraint, separating buying-power capacity from half-floor-room size.
  Floorless accounts receive no floor-based suggestion. Closing and rolling tickets
  start at held size and omit opening-size suggestions.
  [Previews](paper-trading.md#order-preview-and-breach-risk).
- Held-book warnings cover delta/vega limits (including gamma effects), soft-floor
  room, tonight's floor ratchet, ETF delivery, early assignment and ex-dates.
  [Warnings](paper-trading.md#risk-warnings).
- What-if compares adjustments of up to four orders against the current book for
  buying power, Greeks, grid loss and floor room before submission.
  [What-if](paper-trading.md#what-if).
- Breach estimates show spot moves that could reach the plan or personal soft floor
  and modelled touch probabilities. Risk profiles plot P&L today, on later dates
  and at first expiry with an IV offset, for one underlying or the book beta-weighted
  to SPY/SPX. Each date marks floor crossings and estimated touch odds.
- Volatility probability cones estimate finishing beyond or touching chosen prices
  by chosen dates. These are labelled model estimates, not predictions.
  [Profiles and probabilities](paper-trading.md#risk-profile-and-probabilities).

## Journal and review

- A P&L calendar, win rate, profit factor, return on buying power and reports by
  hold time, weekday, month and tag cover contracts and strategies. Shares from
  trades, exercise and assignment have their own round trips.
- Whole trades retain rolls and adjustments. Reviews show entry/exit market and
  account context, adverse/favourable excursions, giveback, R-multiple and P&L by
  Greek over the trade, on the underlying's chart. Forced exits are labelled.
  [Trade review](paper-trading.md#trade-review), [whole trades](paper-trading.md#whole-trades).
- Trade notes and tags and daily plans/reviews persist. Trade and fill CSVs carry
  attempt identity and replay run, scenario and seed, plus settlement sources and
  automatic exit labels. [Exports](paper-trading.md#csv-downloads).

## Replay, scenarios and the demo market

- Record sessions and replay beside the live feed at 1×–300× or as fast as possible,
  with isolated practice/evaluation accounts and journals. Historical-day imports
  from Databento or a local Theta Terminal create private recordings. Importers
  have saved-response tests but have not been run live with a key.
  [Recording and replay](runtime.md#recording-and-replay).
- Start at a chosen New York time, pause, skip, step by time or snapshot, or use
  paced play-to targets. Queued skips and batch-boundary stops are visible.
  Restart from a time with the same commands into a new verifiable journal.
  Interrupted runs, including runs active at clean shutdown, can resume.
  [Stepping](runtime.md#lockstep-stepping), [restart](runtime.md#restarting-with-the-same-commands).
- Multi-day runs carry one account through regular, curb and overnight sessions,
  rollover, end-of-day ratchets, GTC carry, opening gaps, assignment and dividends.
  [Several sessions](scenarios.md#several-sessions).
- Finished runs open read-only in Journal and Dashboard. Verify them in the terminal
  or with `openportd --verify-run JOURNAL`; background verification survives restart
  and supplies a JSON receipt with inputs, plan, equity, head/count, build and time.
  Missing tails and changed inputs are detected; changed journals invalidate saved
  results. Playback speed does not change fills or journals on the same build/platform;
  floating-point results are not promised identical across platforms.
  [Verification](runtime.md#verifying-a-run).
- Replay, scenario and drill journals sync every 250 ms and at pause, stop, finish
  and teardown. A power cut can lose the last quarter second. Live paper accounts
  sync every transaction before publication.
- Twenty built-in scenarios include SPX, SPY, QQQ, XSP, NDX, RUT and VIX, with drill
  objectives, fresh or supplied seeds, gaps, volatility changes and multi-day runs.
  The terminal offers them when markets are closed or the feed stalls. Prices and
  volume are always labelled simulated. Custom JSON files use `--scenario-dir`.
- `--provider demo` rotates regular scenarios as the server feed without network
  services. Index chains use product ticks/sessions, AM monthlies through their last
  trading day, same-date PM series and VIX forward term structure. Earlier generator
  revisions remain reproducible. [Demo feed](runtime.md#demo-feed).
- Stress scenarios can author abnormal books, quote stalls, stale marks, previous
  closes, halts and all three circuit-breaker levels, with wider strikes for large
  moves and early-close timing. They are practice sessions, not reconstructions of
  historical events. [Scenario events](scenarios.md#events).

## Playbooks and backtests

- Versioned setups combine entry windows, conditions, shared strategy templates,
  preview sizing and spread exits. Trade, Brief and Dashboard stage HTTP-shaped
  orders for review; live paper, replay and scenarios can submit automatically.
  Failed conditions show their inputs and thresholds. [Playbooks](playbooks.md).
- Conditions include completed-bar SMA/EMA, Wilder RSI, Bollinger bands, VIX levels
  and signed opening gaps. Management includes trailing profit, calendar DTE,
  trading-day deadlines and debit bracket stops. Missing history prevents entries;
  no future bars are used. Automatic exits are labelled in orders and exports.
- The Playbooks page compares adherence and expectancy; Journal filters by playbook.
  Dashboard and playbook pass odds are estimates from past results, not predictions.
- Live Auto records forward-test windows and results, resumes after restart and
  compares each version with its latest saved backtest. Brief and Dashboard show
  activity. [Forward tests](playbooks.md#forward-tests-on-live-paper).
- Backtest one pinned version or 2–8 jointly on one account over recorded, imported
  or seeded scenario days. Joint tests share buying power and plan limits, with
  deterministic ordering and per-playbook attribution. Independent daily results
  and carried-account attempts include daily evaluation rows and verifiable journals.
  [Joint backtests](playbooks.md#joint-backtests).
- Compare saved runs and summed independent daily P&L; the latter is not shared-account
  trading. Pin, delete or automatically retain reports/journals in the Backtest page
  and API; the CLI also saves reports. Missing data stays missing and open positions
  at EOF are reported, not closed at invented prices. Simulations do not predict
  future results. [Batch backtests](playbooks.md#batch-backtests).

## Accounts, alerts and notifications

- Named accounts have separate rules, positions and journals on the same market.
  Rename, archive/unarchive or confirm deletion in the terminal; deleted files are
  retained. Preset IDs accompany names. Copy limits/guardrails to accounts or replays,
  with notices when queued settings need a reset. Per-underlying limits are editable.
  [Accounts](paper-trading.md#accounts).
- Public demo visitors get private simulated sandboxes, removed after 24 hours idle
  or restart, with capacity/rate limits. Scoped tokens default to their own account;
  replay tokens configure isolated accounts. Named tokens can be revoked without
  restart. [Sandboxes](runtime.md#public-sandboxes).
- Price-level chart alerts and fills appear in the terminal, with optional browser
  notifications and chimes while open. Server-kept account alerts cover option
  quotes, IV/Greeks, spread marks, underlying price/IV, equity, day P&L, floor room
  and exposure, forwarding even with the terminal closed. Assignments, expiry
  exercises and dividends are announced. Rule warnings fire once per threshold,
  account and trading day. [Account alerts](paper-trading.md#account-alerts).
- Discord, Telegram, ntfy and webhooks can receive fills, refusals, rule trips,
  floor warnings, assignments, exercises, ready playbooks, account alerts and feed
  stalls. Channels have filters and test buttons; delivery runs apart from trading.
  Simulated forwarding requires `include_simulated`; catch-up and backtests never
  send. [Notifications](runtime.md#external-notifications).

## Operations and scripting

- One C++20 process serves the feed, analytics, simulator and React terminal.
  Status shows per-underlying feed health, sessions, queue/analytics timings and
  local volatility history. Cboe's holiday schedule is read daily; market-wide
  breakers model S&P 500 drops of 7%, 13% and 20% with halt/resume banners.
- Cboe delayed, Databento, Massive, ThetaData, Tradier and tastytrade supply data.
  Except for Cboe, adapters have sample-response tests but have not been run live
  with a key/account. Broker adapters read data only and cannot place orders.
  [Provider configuration](configuration.md#providers).
- A checked [OpenAPI contract](openapi.yaml), standard-library Python client and
  MCP server use scoped tokens and record command actors. [Scripting](api.md).
- Append-only hash-chained journals recover accounts and detect damage. Compaction
  retains originals as `.bak`; repair can cut a torn final line. Bad journals, stale
  data and stalled feeds refuse new trading. `--no-paper` disables trading.
  [Recovery and failures](paper-trading.md#journal-recovery-and-failure-handling).
- Install through Homebrew on Apple Silicon, Docker Compose, release archives or
  source. Browser update notices are off by default. [Installation](install.md).
  Developer demo soak runs accept a seed and sorted JSON summaries for comparison.

## What OpenPort does not do

- **Bundle real-time option data.** OPRA quotes need a paid, licensed feed that the
  project cannot bundle or redistribute. Bring a provider; the free Cboe feed is
  15-minute delayed and suitable for practice. [Providers](configuration.md#providers).
- **Include years of historical option data.** Historical OPRA data is licensed.
  Use recorded sessions, generated scenarios or local Databento/ThetaData imports
  with your own access. [Historical imports](runtime.md#importing-a-historical-day).
- **Trade futures or futures options.** These need different contract specifications,
  sessions, SPAN-style margin, settlement into futures and data sources. OpenPort
  simulates equity, ETF and index options and shares.
  [Supported instruments](paper-trading.md#instruments-and-prices).

### Replay recovery and order destinations

Quiet replay ticks no longer send the terminal back to live by themselves. After
five seconds without ticks, the terminal shows “Reconnecting to the replay…” and
checks `GET /api/replay`. An existing unfinished run stays selected, including
while paused. A later tick clears the notice. A stopped or finished run returns
the terminal to live; server failures must persist for two minutes before that
fallback. Individual checks time out after ten seconds.

Source and account changes show a persistent banner naming where orders now go.
It includes **OK** to dismiss and **Back to replay** when the server still reports
a run. Sandbox expiry and a disappeared account also explain the change.

Tickets and other write controls retain the source, account, and replay run from
when they opened. If the destination changes before submission, no write is sent.
A confirmation names the new destination (for example, **Send to live account
Practice**); cancelling sends nothing. Closing and reopening the action also
selects the current destination. This applies to order changes, cancellations,
flattening, exits, stock/strategy actions, and settings writes as well as tickets.
