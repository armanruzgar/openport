# Features

Everything OpenPort does today. The [README](../README.md) has the short version;
[paper trading](paper-trading.md) documents every rule, the HTTP contract and the
simulation's limits.

## Market analytics

- **Chain**: bid, ask and mid with bid/mid/ask IV, delta, gamma, vega, theta, vanna and
  open interest and the session's volume per strike, with each side's spread and a
  liquidity cue, centred on the money, with the provider's own IV alongside where it
  publishes one. SPX (AM-settled) and SPXW (PM-settled) expiring on the same day stay
  separate. Missing quotes, volume and open interest show as missing, never as zero,
  with coverage counts per expiry. The chain opens on the nearest expiry before its
  auto-close and counts down to it on the market-data clock.
- **Smile and term structure**: out-of-the-money smile per expiry with its SVI fit and
  arbitrage checks, and the ATM term structure on a square-root-of-time axis, with each
  expiry's forward and rate and where it came from.
- **Volatility metrics**: model-free IV and ATM term structures, delta risk reversals
  and butterflies, realized volatility and cones, the variance risk premium, and
  implied session moves with optional event labels. Local minute and close history
  adds IV rank and percentile, labelled index proxies, and ex-post variance risk
  premium for SPX (SPY by proxy). Values use current market time;
  missing, truncated and proxy estimates are marked. [Definitions](volatility.md).
- **Exposure**: GEX and VEX by strike and expiry, total gamma profile, gamma flip, and
  call and put walls.


## Trading terminal

- **Brief**: prior-session and observed overnight levels, implied moves and bands,
  volatility changes, positioning and supplied events for the selected underlying,
  beside the account's floor room, loss allowance, guardrails and open positions.
  Plan before the open and review after the close on market time, including replay.
  Key levels can be drawn on Trade's chart, with the choice saved in the browser.
- **Trade**: the chain with an order ticket docked beside it, and a candle chart of the
  underlying (one-minute to daily, backfilled from Cboe's free history) showing your
  strikes, armed triggers and the selected expiry's expected move.
- **Orders**: DAY, GTC, EXTO, GTC_EXTO and timestamp-bounded GTD limits, market orders, orders that wait for a price level,
  another underlying's price (enter if VIX reaches 20), a study (30-day IV, the 9/30-day
  IV ratio) or a time of day (close at 15:30), stop-limits, trailing stops (by dollars, percent or ticks, reading the bid/ask, mid
  or mark), orders that cancel each other or place the next when they fill, and
  brackets whose stop-loss (a stop or a stop-limit) and take-profit
  cancel each other, keeping a partly filled position protected; a contract already
  held takes the same pair from its Exits dialog. GTC orders wait
  outside the regular session and last until the contract's last trade or auto-close.
  Cancelling one order or all working orders asks for confirmation; the result stays
  visible when the last working order disappears. Working orders change in place: size, limit or trigger level. Each order keeps its
  history (accepted, triggered, every change and refused change, and when and why it
  ended, with the check's numbers), and a working order says what it is waiting for:
  its limit, trigger, the regular session, a fresh quote or fill latency. Entry notes and tags
  follow the trades into the Journal. Tickets warn on thin liquidity without blocking;
  market orders show the displayed size they take against. The chain and tickets mark
  quotes paper orders cannot fill on (one-sided, crossed or sizeless) and the displayed
  size your orders already took, so a partial IOC is no surprise. Every fill keeps the
  bid, ask and sizes it traded against, the displayed size left, and how old the quote was.
  EXTO/GTC_EXTO protection works overnight and curb with simulator-managed limit
  execution. Flatten can use limits that follow the touch across quotes in those
  sessions, preserving combo closes, size splits and bracket protection.

- **Strategies**: up to four legs (spreads, straddles, condors, butterflies, calendars
  and diagonals) picked on the chain, or built from a template such as a 10-wide put
  spread at 15 delta or a condor one expected move out, with presets saved in the
  browser. Legs fill together at a net debit or credit. The ticket shows the P&L at
  expiry and today, the expected move, and the probability of profit from the smile's
  risk-neutral distribution, skew included. A held strategy is one row with its combined
  open P&L before fees and dollar delta, gamma per 1% spot move, vega per volatility
  point and theta per day, closed or rolled in one order: to a later expiry, to new strikes in
  the same expiry, or a whole condor as one eight-leg order. A roll or an adjustment
  stays one whole trade with its own review. Spreads take OCO
  exits on entry or while held, with stops on the closing net or the underlying, and
  a condor or iron butterfly can roll its put or call vertical as one four-leg order.
- **Evaluations**: a profit target and a trailing drawdown floor (intraday or end of day)
  decide pass or fail, with buy-only, defined-risk and buying-power rules and auto-close
  before expiry. Limits tighten immediately; looser evaluation limits wait for the
  next trading day. Personal soft floors, trade limits, cooldowns and profit locks
  keep the account reduce-only when reached.
  The Dashboard charts minute and fill equity against the target and floor, and the Journal keeps a P&L
  calendar, win rate, profit factor, return on buying power and reports by hold time,
  weekday, month and tag,
  per contract or per strategy, with shares from exercise and assignment as trades of
  their own. Each trade's review shows the market and the account at entry and exit,
  how far it went against you and in your favour (MAE and MFE), what you gave back and
  its R-multiple and its P&L by Greek over its life, on the underlying's chart, and
  marks the exits a stop, a flatten or a liquidation forced. Trades take notes and tags, each day a plan and a review, and
  trades and fills download as CSV, each row naming its attempt and, from a replay,
  the run, scenario and seed. A new attempt keeps the history.
- **Risk**: Greeks per position, today's P&L split by delta, gamma, vega and theta
  (with costs apart, and contracts closed today still listed), dollar-delta and vega limits, a spot × volatility scenario grid,
  a daily loss limit and a reduce-only kill switch: closing orders and bracket exits
  keep working while opening orders are cancelled. The latch keeps the history of its
  trips, resets and releases, and says when a reset can clear it. Positions close together as one
  order, or flatten an underlying or the whole account at market: each short closes
  together with the long that covers it, so no short is left naked; closes larger than
  the order limit split, what a thin quote leaves works on later quotes until flat,
  bracket exits keep protecting whatever is still open, and the flatten reports what
  stays and why. A short quoted 0.00/0.05 is bought back at its ask, a long nobody
  bids for is abandoned at zero without a fee, and a long option can be
  told not to exercise at expiry. Server order
  previews show buying power, maximum loss, what fills at once and the full size's
  fill schedule, and the suggested size labelled by its binding constraint: floor,
  buying power or limits (never floor room on floorless accounts). They separate
  the size that fits buying power from the size that uses at
  most half the floor room; Edit order previews a change to a resting order the same way,
  while close and roll tickets start at the held quantity, cap it at the holding and
  omit opening-size suggestions. Flatten shows its dry run, what it would cancel and close and the account after it.
  Warnings name what the held book is close to: a delta or vega limit (by gamma, too),
  the soft floor, tonight's floor ratchet, delivery of in-the-money ETF options at
  expiry, early assignment and ex-dates. What-if compares candidate adjustments, each
  of up to four orders, against the held book on buying power, Greeks, grid loss and
  floor room before any is sent. Breach risk shows the spot moves that could reach the floor (the
  personal soft floor on an account without a plan floor) and labelled model
  estimates of touching it before the close. The risk profile curves the held book's
  P&L today, on later dates and at the first expiry, with an IV offset, against one
  underlying or the whole book beta-weighted to SPY or SPX, and marks where each
  date's curve reaches the floors and the odds of touching them by then. The
  Volatility tab gives probability cones and the odds of finishing beyond or touching
  chosen prices by chosen dates.
- **Playbooks**: versioned setups with entry windows and conditions, shared strategy
  templates, preview sizing and spread exits. Trade, Brief and Dashboard stage orders
  for review; live paper accounts, replays and scenarios can send them automatically. The Playbooks page
  compares adherence and expectancy, and the Journal filters by playbook. Dashboard
  and playbook pass odds are labelled estimates from past results, not predictions.
  Failed conditions name their inputs and thresholds; stages include an HTTP order
  body for review or submission. Automatic time-stop cancellations and closes are
  labelled in orders, Journal details and trade CSV. Live Auto records forward-test
  windows and results, resumes after restart, and compares each version with its
  latest saved backtest; Brief and Dashboard show automatic trading activity. [Rules and limits](playbooks.md).
- **Playbook rule breadth (F52, part 1)**: completed-bar SMA/EMA, Wilder RSI and
  Bollinger entries, VIX levels and signed opening gaps; trailing-profit, calendar
  DTE, trading-day deadlines and debit bracket stops across live Auto, replay and backtests.
- **Batch backtests**: run one pinned playbook or 2–8 jointly on one account over recorded, imported or seeded
  scenario days, with independent daily results and carried-account evaluation
  attempts with daily evaluation rows. Compare saved runs and chart the sum of
  independent daily P&L. Pin, delete and automatically retain saved reports and
  journals through the API and Backtest page; the CLI also writes verifiable reports.
  Results are simulated trading, not predictions or investment advice.
- **Accounts**: several named accounts at once, say a 50K evaluation beside a practice
  book, each with its own journal, rules and positions on the same market. Rename,
  archive/unarchive and confirm deletion in the terminal; deleted files are retained.
  Preset IDs appear beside plan names. Copy active limits and guardrails into new
  accounts or replays, with a replay notice when queued settings need a reset. Public
  demo visitors can create private sandbox accounts on simulated prices, removed
  after 24 hours unused or a server restart. Sandbox tokens default to their own
  account. Named tokens can be revoked without restarting, and replay tokens can
  configure isolated accounts. HTTP and the terminal edit per-underlying limits;
  startup flags select main-account execution settings.
- **Replay**: record every session and trade any recorded day again beside the live
  feed, on a practice or evaluation plan, at 1× to 300× or as fast as possible. Start
  at a chosen New York time, pause or skip, and keep each run's trades in its own journal.
  Resume a run a crash interrupted, on the same journal, where it stopped.
  Finished runs appear in the account switcher as they finish and open read-only in
  Journal and Dashboard. Replay journals sync to disk
  every 250 ms rather than every record, so a power cut can lose their last quarter
  second.

  - **Reproducible runs**: playback speed leaves batches, fills and journals unchanged.
    Relative and next-snapshot steps, batch-boundary abort/pause/stop, visible queued
    skips, paced play-to targets, and start times with seconds support repeatable drills.
    Restart from a time with the same commands into a new, verifiable journal.
    Step to a market time with `PUT /api/replay {"until":"10:30"}`, or a date and time
    in the terminal; check a saved run with **Finished runs → Verify** or
    `openportd --verify-run JOURNAL`. Final journal heads and counts detect missing
    tails; history flags torn and truncated runs. Verification runs in the background,
    survives restart and offers a JSON receipt naming inputs, plan, final equity,
    head/count, build and time. Changed journals invalidate saved results.
  - **Multi-day runs**: scenarios of several regular, curb and overnight sessions over
    several days play as one steppable run on one account, with the rollover, the
    end-of-day ratchet, GTC carry, gaps at the open, early assignment and dividends.
- **Historical days**: import a past trading date from Databento or your local
  Theta Terminal into a private recording, then trade it in Replay. Imported days
  show their provider and an imported label. The importers follow the documented
  APIs and have saved-response tests; they have not yet been run live with a key.
- **Demo market**: when markets are closed or the feed has stalled, the terminal offers
  twenty built-in simulated scenarios, including SPX, SPY, QQQ, XSP, NDX, RUT and
  VIX options, with drill objectives, gaps, volatility changes, overnight sessions
  and runs that span several sessions and days. Each run chooses a fresh seed, or
  repeats one you supply. Add your own JSON files with `--scenario-dir`; generated
  prices stay labelled simulated. `--provider demo` rotates regular scenarios as
  the server's own feed without network services. Index chains have product ticks,
  sessions, AM monthlies through their last trading day, same-date PM series and VIX
  forward term structure; earlier generator revisions stay reproducible.
  [Scenario format](scenarios.md). The developer demo soak supports `--seed` and
  sorted JSON summaries (`--json [PATH]`, stdout by default) for comparing runs.
- **Alerts**: price levels on an underlying (drawn on its chart) and every fill, shown in
  the terminal and as browser notifications with an optional chime while it is open.
  Account alerts on an option's quote, implied volatility or Greeks, a spread's net mark,
  an underlying's price or IV, or equity, day P&L, floor room and exposure are kept by
  the server and forwarded to notification channels with the terminal closed;
  assignments, exercises at expiry and dividends are always announced. Floor room,
  daily loss, guardrails, pending limits and a nearby profit target also raise alerts,
  once per threshold per account and trading day.
- **External notifications**: send fills, rejected orders, floor warnings, rule trips,
  assignments, exercises, ready playbooks, account alerts and stalled feeds to Discord, Telegram,
  ntfy or a webhook. Each channel has filters and a test button in Alerts settings.
  Delivery runs separately from trading. Demo, replay and interactive drill forwarding
  is opt-in with `include_simulated` in the server notification config; catch-up
  history and backtests never send.

## Operations

- **Status**: feed health per underlying, trading sessions, queue and analytics timing,
  and the local volatility history store.
- **Scripting and agents**: a checked OpenAPI contract, a standard-library Python
  client and an MCP server, with scoped tokens and journal actor attribution.
- **Broker data**: Tradier snapshot polling and tastytrade DXLink streams, for
  traders whose brokerage account includes real-time option data; credentials stay
  in the environment and neither adapter can place orders.
- **Install**: Homebrew on Apple Silicon, Docker Compose, release archives or source.
  Optional browser update notices are off by default.
- Light and dark themes, keyboard shortcuts (number keys follow the sidebar: Brief
  is 7 with a paper account, 2 on analytics-only servers; arrows step expiries on Trade).

## Paper trading

Custom plans can set an evaluation time window (F62), an inactivity deadline based
on the trader’s own executions (F58), and allowed underlyings and New York opening
hours (F31). Dashboard reports deadlines; tickets explain and block restricted
openings. Reducing orders and exits stay available. These optional rules persist
and work in replay and custom backtests.

The engine simulates orders on European cash-settled index options (SPX, XSP, NDX,
RUT and their weeklies) and American equity and ETF options (SPY, QQQ, single stocks)
against displayed quotes: market and marketable orders take the far side up to the
displayed size by default, with optional per-plan slippage of 0–10 ticks. Single-leg limits cap
the fill price; multi-leg orders wait if the slipped net exceeds their limit.
Rules offers **As displayed** (the existing defaults), **Inside at midpoint** (see
below), or **Conservative** for a new
account attempt: 1 second of market-time latency, 1 slippage tick and 1 extra tick
for each additional displayed-size block. Custom rules set latency up to 60 seconds
and impact up to 10 ticks per block. With impact, limits wait when the full price
or net exceeds them. This is simulated depth. These models do not simulate queue position,
hidden liquidity, or whether the market would have traded at all; a delayed feed
still gives hindsight. By default, resting limits fill when a later quote crosses them, and every fill
pays the flat per-contract fee or an itemized schedule with open/close rates,
per-leg commission caps, clearing, regulatory, index and per-contract exercise/assignment
fees. Positions are marked at the mid, and risk limits on dollar
delta, vega, order size, price bands and daily loss are checked before and at every
fill. A book the market has pushed over its delta or vega limit can still be closed or
hedged: only orders that would add to the excess are refused. Every product trades in its regular session (09:30 to 16:15 ET for index
options), and SPX, XSP, VIX and RUT options also trade in Cboe's overnight session
(20:15 to 09:25 ET) and the 16:15 to 17:00 curb, with limit orders only. The
market-wide circuit breakers halt trading when the S&P 500 falls 7%, 13% or 20%, with
a banner saying when trading resumes, and Cboe's published holiday schedule is read daily, so a closure it announces applies at
once. American equity
and ETF options deliver shares when exercised early or held into expiry a cent in the
money. A short one that trades below its exercise value at the close, or a call worth
less over it than a dividend going ex, can be assigned overnight, in part and at
random as real assignments are; the shares are marked, risked and closed at the
underlying's price. Stock and ETF shares can also be opened, added to and reversed
for delta hedges, covered calls and collars, with account checks, previews, recovery,
dividends and trade review. The chain and Positions have a share ticket; strategy
templates offer explicit share-first, option-second covered calls and collars.
Dividends are paid on them from a file you give the server (`--dividends FILE`), or
from Massive's API with a key from any of its stocks plans (`--dividends massive`),
whichever provider supplies the quotes. Revision 4 scenarios include a simulated
quarterly SPY/QQQ calendar; the demo feed uses it without an explicit dividend source.

Buying power follows each order's real margin: a naked short holds the usual
20%-of-spot requirement, while spreads, condors, butterflies, calendars and diagonals
hold only what they can lose, a covered call nothing beyond its shares, a short put
against short shares its value, a long call protecting short shares its strike, and a
short straddle or strangle its greater side plus the other side's value, as Reg T sets
it. An order that would use buying power must fit within it;
anything that frees buying power (closing, buying back a short, buying protection) is
always allowed, even when the account is short of it. A close whose longs also cover
other shorts must carry those shorts, like selling a spread's long leg alone.
The Positions page breaks the requirement down by underlying, naming the positions
each part holds (a spread, a covered call, a straddle, a naked short) and what it
needs, or the portfolio scan's worst point.
Custom plans can instead select portfolio margin, as Cboe's and FINRA's rules set it:
each underlying holds its largest loss across a price scan (−8% to +6% for index
products, ±15% for stocks and ETFs), at least $37.50 a contract, and buying power is
equity less that, so long options and shares count as collateral, optionally with an
implied-volatility shock. Buying power can match the account a trader really uses: a
cash account sells calls only against shares and secures each short put with its
strike, an IRA nets spreads but sells nothing naked or short, and a broker's house
margin can be added on top. Presets use strategy margin and As displayed fills unless
Conservative or another margin is selected when starting an attempt or creating an
account; the [paper-trading guide](paper-trading.md#account-rules-and-evaluations) has the details.

Plans set an account's rules: `practice` (the default: buying power only),
`intraday-25k|50k|100k` (buy-only, 10% target, 5% drawdown trailing every new high),
`eod-25k|50k|100k` (any strategy, 12% target, 6% drawdown trailing each close),
`static-25k|50k|100k` (a static floor, a target on the closed balance, a daily loss limit
that fails the attempt and four trading days) or `locking-25k|50k|100k` (a floor that
locks at the starting balance, a daily loss limit that locks the day and a best-day
consistency rule). Touching the floor fails the attempt and closes every position;
reaching the target passes it. Custom plans, set up when starting an attempt, take the
rules prop firms use most: a daily loss limit measured from the day's opening equity or
balance, the higher of the two or the day's high, which closes the positions and locks
the day or fails the attempt; static and lock-at-start floors; minimum trading and
profitable days and a best-day consistency rule that hold a pass back; a target counted
on the closed balance; and a trading day that ends at the plan's own time. The
Dashboard lists what a pass still needs, the daily loss room and what closing every
position now would leave. Decided attempts still accept closing orders, Flatten
and disposal of worthless longs. Equity history keeps the deciding observation,
closest-floor tracking stops there, and archived attempts retain their rules,
decision time/equity, peak and floor.
On the default 15-minute delayed feed a pass is practice, not proof: any real-time chart
shows where the market went next.
`--plan` picks the main account's first plan; start a new attempt on any plan from the
Dashboard or Rules page, and add accounts from the account switcher in the sidebar.

Ordinary paper accounts survive restarts through an append-only, hash-chained journal:
the main account at `~/.openport/paper-journal.jsonl` (`--paper-journal`), the others in an
`accounts` directory beside it. Each record reaches the disk before its transaction is
published, and carries what the transaction changed, with the whole state every
thousand records; `openportd --compact-journals` rewrites journals
from older builds that way, keeping each original as `.bak`, and `--repair-journals`
cuts off a last line a full disk tore. `--no-paper` turns trading off. The engine also
models the funded phase that follows a pass (`funded-*` plans with a locking floor and
payouts). Custom funded plans support payout-cycle best-day consistency (F37),
including escalating limits by payout number and the additional profit needed,
and a payout buffer above the starting balance for all or the first N payouts (F38).
Optional scaling plans (F39) start at reduced option size and change the contract
limit from the next session’s closed-balance step, up or down. Every option leg
counts, working openings reserve capacity, and the terminal shows and edits the plan.
The terminal offers verification with prerequisite locks. This is a simulator that
funds no one, so it hides funded plans and the Payouts page unless the account is
already funded; set `showFundedAccounts` in `web/src/lib/features.ts` to offer them.
[Paper trading](paper-trading.md) documents every rule, the HTTP contract and
the simulation's limits.

Opt-in inside fills fill limits at a deterministic share of the bid/ask spread
(midpoint preset: 50%), limited by each far side's displayed size left, with the
usual fees and risk checks. DAY/GTC single-leg and net combo limits can walk by a
chosen step and interval toward a cap on market time. Both tickets, previews,
order history and edits expose the walk; replay and journal recovery preserve it.

Settlement references and provenance are visible on Journal trade rows and in its
settlements list, account reads and trade exports, and survive journal recovery.
Demo and new scenario/recorded replays
settle AM positions on the expiry opening print, an approximation of the official
special opening quotation; live providers keep manual imports.

### Event restrictions (F17/F59)

Plans share a saved event calendar for scoped news blackouts and weekend,
earnings, ex-dividend and split holding restrictions. Configure `events`,
`news_before_minutes`, `news_after_minutes`, `news_action`, `hold_restrictions`
and `hold_cutoff` in custom plans. Imports use the daemon's `--event-calendar`
catalogue or known dividend ex-dates; saving freezes them into the plan. Rules,
Dashboard, chain and tickets explain active/next restrictions and permit reductions.
Once-only system closes, boundary failures and the holding business calendar are
journaled for deterministic recovery. See [rule semantics](paper-trading.md#news-blackouts-and-holding-restrictions-f17-f59).

F6, F17 and F59 can be combined with the time, inactivity, underlying, trading-hour,
contract-count, stop-loss and trade-risk rules. Rollover gives `OVERNIGHT_HOLD`
precedence over `HOLD_RESTRICTED`; overlapping scheduled closes retain the first
submitted close's label without duplicating orders.

## Roadmap

- [x] F39 funded scaling plan: closed-balance thresholds set the next session’s option-contract limit, with working openings, fill checks and terminal editing
- [x] F63 funded account-size scaling: periodic profit/payout reviews, linear capital growth capped at a maximum, proportional loss limits, journal recovery, custom rule editing and funded progress/history.
- [x] F38 payout buffer: retain starting balance plus a safety net for every payout or the first N
- [x] F37 payout consistency: net realised best-day limits per payout cycle, escalating percentages and remaining profit
- [x] F62 evaluation time windows, F58 inactivity limits, and F31 instrument whitelists and trading hours per plan
- [x] F26 stress-scenario authoring: abnormal books, quote stalls and stale marks,
  authored previous closes and market halts, all three circuit-breaker levels,
  wider strike ranges for 10%+ moves and margin floors, and early-close event scaling.
  Demo and stress recordings support market controls in format v4; ordinary live
  recordings retain format v2 compatibility. Authored-halt messages show New York
  resume times and recognize regular and early closes.
- [x] Technical, VIX and gap playbook entries; trailing, DTE, trading-day and debit exits (F52, part 1)
- [x] F52: backtest comparison, combined independent daily P&L, per-day evaluation rows,
      saved-run deletion, pins and bounded server retention
- [x] True joint multi-playbook backtests sharing account buying power and plan limits (F52)
- [x] F72: held-size close tickets, confirmed cancellations with persistent results,
      fresh finished runs in the switcher, held-strategy gamma and vega, and sizing labels
      that name the binding floor, buying power or limit
- [x] Specific playbook condition reasons, staged orders in HTTP shape and time-stop labels (F51, partial)
- [x] Automated forward testing on live paper, persisted version windows, restart recovery,
      journal-derived results and matching-version backtest comparison (F51)
- [x] Seeded demo soak with JSON run identity, counters and exit status (F73)
- [x] F57 part 1: named-account lifecycle, preset IDs, copied limits/guardrails and replay reset guidance
- [x] F57: HTTP per-underlying limit overrides and terminal editing, retaining driver-6 replay compatibility
- [x] F57: fill-model startup flag and numeric execution overrides
- [x] F57: replay tokens reset and configure their isolated accounts
- [x] F57: sandbox tokens default to their own account
- [x] F57: token reload without restart (automatic, SIGHUP and admin HTTP); account and token management complete
- [x] Opening share positions for hedges, covered calls and collars, with share previews and terminal shortcuts
- [x] A flatten that works until flat: spreads close as one order, large closes split,
      remainders work on later quotes and exits stay until flat; buying back shorts quoted
      only on the ask, abandoning worthless longs and do-not-exercise instructions
- [x] Headless playbook batch backtests, independent days and carried-account evaluation attempts, API and terminal reports
- [x] Homebrew formula, Docker Compose and opt-in browser update notices
- [x] External notifications through Discord, Telegram, ntfy and generic webhooks
- [x] Versioned playbooks, staged orders, live paper and replay Auto, adherence and historical pass-odds estimates.
  [Live paper Auto](playbooks.md#forward-tests-on-live-paper) requires a known market time
  and records forward-test windows; replays and backtests never inherit forward tests.
- [x] Private visitor sandbox accounts on public simulated demos, with idle expiry and rate limits
- [x] Checked OpenAPI contract, Python client and MCP tools, scoped tokens and actors
- [x] Plan-locked limits, personal guardrails, order previews and size to floor, breach
      estimates, intraday equity history and rule alerts
- [x] Pricing core: Black-76 and Black-Scholes-Merton with full Greeks, safeguarded IV
      solver, Cox-Ross-Rubinstein and Leisen-Reimer trees
- [x] Providers: Cboe, Databento, Massive, ThetaData, Tradier and tastytrade, with
      record and replay of any feed; broker adapters await live-account validation
- [x] Local Databento and ThetaData historical-day imports; live-key validation pending
- [x] Chain liquidity: session volume, spread percentages and ticket warnings
- [x] Chain analytics: parity forwards, IV and Greeks, SVI surfaces, GEX and VEX, and
      de-Americanised IV for equity options
- [x] Paper trading against live quotes: risk limits, scenarios, index and American
      equity and ETF options
- [x] Evaluation simulator: profit targets, trailing drawdowns, resets and a trade
      journal, with verification in the terminal and funded phases and payouts in the engine
- [x] Plan objectives: daily loss limits by basis that lock the day or fail, best-day
      consistency, minimum trading and profitable days, static and lock-at-start floors,
      closed-balance targets, exit costs and a trading day set per plan
- [x] Strategies: multi-leg orders with spread-aware buying power, held strategies as
      positions, rolls, risk graph, probability of profit with skew, and templates
- [x] Terminal: underlying chart, order changes in place, flatten, multiple named
      accounts and trading recorded days in replay
- [x] Paper trading in Cboe's overnight and curb sessions
- [x] GTC limit orders, order notes and tags, spread brackets and held-spread exits
- [x] EXTO/GTC_EXTO protection in all product sessions, timestamp GTD, and limit flatten in GTH/curb
- [x] Orders conditional on another underlying, an IV study or the time of day, verified in replays
- [x] Account alerts on options, spreads, Greeks, IV and account measures, forwarded off-screen
- [x] Roll the put or call side of a four-leg strategy
- [x] Trade notes and tags, with reports by tag, and price and fill alerts
- [x] Fill context, trade and strategy excursions, CSV export and day plans and reviews
- [x] P&L attribution by delta, gamma, vega and theta
- [x] Stock positions from early exercise and from exercise and assignment at expiry
- [x] Expiry hours as the exchanges run them: ETF options to 16:15, auto-close five
      minutes before each contract's last trade, with the chain staying on 0DTE until then
- [x] Demo market: simulated days to trade in Replay or rotate as an offline server feed
- [x] Scenario library: simulated drills with fresh or repeatable seeds, start times and
      replay journals that are kept
- [x] Deterministic replay batches, verifiable run journals and synchronous market-time stepping
- [x] F32 replay control: relative/next-snapshot steps, paced play-to, batch-boundary abort,
      queued-skip feedback, seconds in start times, and restart with the same commands
- [x] In-app run verification, persisted final head/count, damaged-run warnings and shareable JSON receipts
- [x] Multi-day and cross-session scenario runs: regular, curb and overnight sessions over
      several days on one account, steppable to a date and time, with journals that verify
- [x] Shares in the journal, and early assignment of shorts trading below exercise value
- [x] Partial, random early assignment, and dividend risk on short calls
- [x] Market-wide circuit breakers, with a banner and kept across restarts, and Cboe's
      holiday schedule read daily
- [x] Dividends from Massive's API
- [x] Visible settlement references and sources, with AM opening-print settlement in demo and replay
- [x] PM settlement on the provider's official close, revisions included
- [x] Cboe's delayed feed from its quote pages when its data files fall behind
- [x] Dividends from a file you supply
- [x] Optional market-time fill latency and simulated size impact, with As displayed and Conservative presets
- [x] Optional slippage, and portfolio margin as Cboe's and FINRA's rules set it
- [x] Margin realism: shares covering options, Reg T straddles, netted combined
      structures, a per-position margin breakdown, portfolio margin with an IV shock
      from the terminal, and cash, IRA and house-margin accounts
- [x] Known cash dividends in the American exercise model
- [x] IWM and DIA by default, with stocks and ETFs marked at their regular close
      outside the session
- [x] Cboe's delayed data from its new host, following redirects if it moves again
- [x] Current model-free IV, ATM/skew, realized volatility, cones and implied session moves
- [x] Local volatility history, IV rank and percentile, labelled index proxies and ex-post VRP
- [x] Morning brief with market levels, account allowance, day notes and chart overlays
- [x] Stop-limit exits on single contracts and spreads, banded around their stop, and
      exits that re-arm or shrink so a partly filled position stays protected
- [x] Resumable interrupted replay runs, archived runs' working orders ended with the
      run, and demo restarts that resume the rest of their date with a breaker reference
- [x] OCO exits for a contract already held, and cancelling an exit pair in one call
- [x] Trailing stops by dollars, percent or ticks on options and spreads, reading the
      bid/ask, mid or mark
- [x] One-cancels-other and one-triggers-other order chains, for breakout entries and
      staged scale-ins
- [x] Exports that name their replay run and attempt, trades that say what closed them,
      holiday-aware weekday reports and one win-rate definition
- [x] Order history: end times, changes and refusals, what a working order waits for,
      and the numbers behind every rejection or risk cancel
- [x] Fuller previews: fill schedules, order changes, flatten dry runs and sizing to
      buying power apart from the floor; held-book risk warnings; what-if comparison of
      candidate adjustments
- [x] Fill audit trail: the bid, ask, sizes, observation and first quote time on every fill
- [x] Executable-quote flags and the displayed size an account has left, in the chain, tickets and previews
- [x] P&L by Greek for each round trip and for contracts closed today, to the micro-dollar
      and flagged where it falls back; planned risk for combo stops and covered calendars;
      positions with their round trip and the contract's lifetime this attempt
- [x] Whole trades across rolls and adjustments, with their own review, and legs
      entered one by one grouped into one trade; both order tickets can join an open
      whole trade; held strategies over the API
- [x] Return on buying power for trades, strategies and whole trades, in the Journal,
      playbook reports and backtests
- [x] Same-expiry strike rolls and whole-condor rolls, exits resized in place and DAY or
      GTC changed on a resting order
- [x] F34: 0–99 percent floors and unused/carried-floor warnings; daily discipline survives account reset; per-order opening counts; standalone, trailing, OCO and bracket stop cooldowns; buy-only put exercise requires held shares
- [x] Kill latch history, when a reset can clear it, and guardrail refusals with their numbers
- [x] Client order IDs scoped to an attempt and looked up by ID, conflicting reuses
      recorded nowhere, shorts closed first on auto-close, and a buy-only manual close
      that supersedes its own armed stop
- [x] Preview warnings for stops, targets and triggers already reached, a stop given as a
      limit price, and slippage that pushes a market order outside the price band;
      IOC remainders that say whether the limit was short or the book was used up
- [x] Marketable buy limits reserve their expected fill and armed buy stops their level;
      dividends paid in whole cents; an idle account's views as of the feed's time;
      working-order ladders that no longer slow every command
- [x] F33: closing leftover positions after a decision, exact decision samples, frozen
      closest-floor tracking and archived attempt rules and decision details
- [x] F25: realistic itemized fee schedules, with per-leg caps, index fees, exercise
      and assignment charges, fee previews and fill breakdowns
- [x] F69 demo and replay coverage: XSP, NDX/NDXP, RUT/RUTW and VIX/VIXW,
      product ticks and sessions, AM last-trade cutoffs beside same-date PM series,
      VIX driven by SPX volatility and mean-reverting forwards
- [x] F69: American ETF option generation and quarterly cash dividends reflected in
      spot and option prices, Replay accounts and the demo feed
- [x] F35, F53, F54: risk profile of the held book on later dates and IV offsets with room to the floor,
      beta weighting to SPY or SPX, and probability cones with odds of reaching a price by a date
- [x] F46: fills inside the bid/ask spread and walking limits, with midpoint fills,
      market-time steps, editable caps, previews and durable order history
- [x] F68 operational robustness: filesystem diagnostics with free bytes and
  a 64 MiB reserve; damaged accounts remain readable at their verified prefix;
  repair dry-run and single-file modes; timestamped equity storage errors and
  recovery status; journal size and verification estimates/warnings; equity
  paging in live, replay and archived accounts; indexed recording end lookup
  and replay control queue shutdown.
- [x] F15: custom-plan caps on held option contracts plus working opening quantities,
      counting combo ratios, with numeric refusals and preview sizing; shares are excluded.
- [x] F60: custom plans require bracket stops and cap per-order risk in dollars or
      floor-room percent. Preview shows risk against the limit; required stops cannot
      be cancelled while their position is held.
- [x] F6: optional mandatory `flat_time` closes options and shares and blocks openings
      until plan day end; independent `no_overnight` fails held positions at rollover.
      Settlement-pending options are excluded. Journaled triggers and pending closes
      recover deterministically; custom editor, Rules, facts, Dashboard notices and
      option/strategy/share tickets expose both rules.
- [x] F29: per-trade consistency limits the best closed whole option trade to a percentage of attempt profit, with exact net P&L and pass-objective evidence.
- [x] F30: minimum closed whole option trades to pass, including system and settlement closures.
- [x] F61: minimum hold time refuses premature user reductions while protective/system exits execute; microscalping limits positive short-round-trip profit as a pass objective.
- [x] F65: custom plans can ban opening hedges within an account and counter positions across live accounts. Options and shares use current dollar-delta direction; previews and tickets explain refusals. The reducer enforces `no_hedging`; the desk checks `no_counter_positions` at acceptance and supplies explicit inputs for reducer fill checks, including other accounts’ working entries and excluding archived/replay/sandbox accounts. Enable the latter on each trading account.
- [x] F66: per-contract position caps as a whole percentage of current-date traded option volume, with exact ratio-aware checks, strict unknown/stale refusals, previews and terminal custom-plan controls. Acceptance checks use current feed volume; fill checks journal explicit volume inputs for recovery. Backtests support generated scenarios and recordings carrying current-date volume.
- [x] F27: two-step challenge and verification presets, prerequisite unlocks, phase history and confirmed next-step starts in the terminal.
- [x] F64: exact evaluation/reset/activation costs, per-plan reset limits, journaled attempt charges, payout net and terminal cost controls.

## Audit feature index

Status of the audit’s F1–F73 features. Links describe the rules and limits.

| ID | Feature | Status |
| --- | --- | --- |
| F1 | [Multi-day and cross-session replay](scenarios.md#several-sessions) | Done |
| F2 | [Best-day consistency](paper-trading.md#plan-objectives-and-the-daily-loss-limit) | Done |
| F3 | [Plan daily loss limits](paper-trading.md#plan-objectives-and-the-daily-loss-limit) | Done |
| F4 | [Flatten that gets flat](paper-trading.md#changing-cancelling-and-flattening) | Done |
| F5 | [Minimum trading or profitable days](paper-trading.md#plan-objectives-and-the-daily-loss-limit) | Done |
| F6 | [Mandatory flat time and no overnight holds](paper-trading.md#mandatory-flat-time-and-no-overnight-holds-f6) | Done |
| F8 | [Trailing stops](paper-trading.md#conditional-and-bracket-orders) | Done |
| F9 | [Stop-limit exits](paper-trading.md#conditional-and-bracket-orders) | Done |
| F10 | [Held-book risk warnings](paper-trading.md#risk-warnings) | Done |
| F11 | [Protection after partial exits](paper-trading.md#conditional-and-bracket-orders) | Done |
| F12 | [OCO exits for held single legs](paper-trading.md#conditional-and-bracket-orders) | Done |
| F13 | [Fill audit trail](paper-trading.md#orders-and-quote-matching) | Done |
| F14 | [Static and lock-at-start presets](paper-trading.md#plan-presets-with-objectives) | Done |
| F15 | [Contracts-held cap](paper-trading.md#account-rules-and-evaluations) | Done |
| F16 | [Closed-balance targets and decision equity](paper-trading.md#plan-objectives-and-the-daily-loss-limit) | Done |
| F17 | [News-event blackouts](paper-trading.md#news-blackouts-and-holding-restrictions-f17-f59) | Done |
| F19 | [Executable quotes and remaining size](runtime.md#chain-volume-and-liquidity) | Done |
| F20 | [Order, change and flatten previews](paper-trading.md#order-preview-and-breach-risk) | Done |
| F21 | [Order lifecycle and rejection detail](paper-trading.md#changing-cancelling-and-flattening) | Done |
| F22 | [Order-entry guards](paper-trading.md#orders-and-quote-matching) | Done |
| F23 | [Position disposal](paper-trading.md#disposing-of-worthless-positions) | Done |
| F24 | [Protection in all sessions](paper-trading.md#sessions) | Done |
| F25 | [Realistic fee schedules](paper-trading.md#fees) | Done |
| F26 | [Stress-scenario authoring](scenarios.md#events) | Done |
| F27 | [Multi-phase evaluations](paper-trading.md#two-step-programs-f27) | Done |
| F28 | [Plan trading-day boundaries](paper-trading.md#plan-objectives-and-the-daily-loss-limit) | Done |
| F29 | [Per-trade consistency](paper-trading.md#plan-objectives-and-the-daily-loss-limit) | Done |
| F30 | [Minimum trades to pass](paper-trading.md#plan-objectives-and-the-daily-loss-limit) | Done |
| F31 | [Instrument whitelists and trading hours](paper-trading.md#evaluation-time-inactivity-and-opening-restrictions) | Done |
| F32 | [Replay control](runtime.md#lockstep-stepping) | Done |
| F33 | [Post-decision handling and attempt records](paper-trading.md#account-rules-and-evaluations) | Done |
| F34 | [Guardrail and plan-rule edge handling](paper-trading.md#personal-guardrails) | Done |
| F35 | [Held-book risk profile by date and IV](paper-trading.md#risk-profile-and-probabilities) | Done |
| F36 | [What-if adjustments](paper-trading.md#what-if) | Done |
| F37 | [Payout consistency](paper-trading.md#funded-accounts-and-payouts) | Done |
| F38 | [Payout buffer](paper-trading.md#funded-accounts-and-payouts) | Done |
| F39 | [Funded contract scaling](paper-trading.md#funded-accounts-and-payouts) | Done |
| F40 | [Whole-trade P&L across rolls](paper-trading.md#whole-trades) | Done |
| F41 | [Roll and adjustment tooling](paper-trading.md#multi-leg-orders) | Done |
| F42 | [Standalone OCO and OTO chains](paper-trading.md#order-chains-one-cancels-other-and-one-triggers-other) | Done |
| F43 | [Conditional orders on symbols, studies and time](paper-trading.md#conditional-and-bracket-orders) | Done |
| F44 | [Richer alerts](paper-trading.md#account-alerts) | Done |
| F45 | [Visible settlement prints and sources](paper-trading.md#expiry-and-explicit-settlement) | Done |
| F46 | [Inside fills and walking limits](paper-trading.md#walking-limits) | Done |
| F47 | [Journal and review improvements](paper-trading.md#trade-review) | Done |
| F48 | [Run and attempt identity in exports](paper-trading.md#csv-downloads) | Done |
| F49 | [Margin realism and account types](paper-trading.md#account-rules-and-evaluations) | Done: certified whole-contract allocation across expiry pools and covers; exact oracle equality and zero fixed-seed subadditivity violations. Work-limit fallbacks are reported explicitly. |
| F50 | [In-app run verification](runtime.md#verifying-a-run) | Done |
| F51 | [Live forward testing and playbook ergonomics](playbooks.md#forward-tests-on-live-paper) | Done |
| F52 | [Backtest breadth](playbooks.md#batch-backtests) | Done (joint multi-playbook backtests share one account, with deterministic ID order, per-playbook attribution and verifiable journals; compare also sums independent runs) |
| F53 | [Beta weighting to SPY or SPX](paper-trading.md#risk-profile-and-probabilities) | Done |
| F54 | [Probability cones and reaching a price](paper-trading.md#risk-profile-and-probabilities) | Done |
| F55 | [Return on buying power](paper-trading.md#return-on-buying-power) | Done |
| F56 | [Opening share positions](playbooks.md#practising-strategies-with-shares) | Done |
| F57 | [Account and token management](paper-trading.md#accounts) | Done |
| F58 | [Inactivity rules](paper-trading.md#evaluation-time-inactivity-and-opening-restrictions) | Done |
| F59 | [Weekend and corporate-event holding restrictions](paper-trading.md#news-blackouts-and-holding-restrictions-f17-f59) | Done |
| F60 | [Required stops and maximum trade risk](paper-trading.md#account-rules-and-evaluations) | Done |
| F61 | [Minimum hold time and microscalping](paper-trading.md#plan-objectives-and-the-daily-loss-limit) | Done |
| F62 | [Evaluation time windows](paper-trading.md#evaluation-time-inactivity-and-opening-restrictions) | Done |
| F63 | [Funded account-size scaling](paper-trading.md#funded-accounts-and-payouts) | Done |
| F64 | [Reset and activation costs and limits](paper-trading.md#program-costs-and-reset-limits-f64) | Done |
| F65 | [Hedging and counter-position bans](paper-trading.md#direction-rules-f65) | Done |
| F66 | [Position size relative to traded volume](paper-trading.md#volume-share-rule-f66) | Done |
| F68 | [Operational robustness](paper-trading.md#journal-recovery-and-failure-handling) | Done |
| F69 | [Demo and replay coverage](scenarios.md) | Done |
| F70 | [API codes and messages](paper-trading.md#reason-codes) | Done |
| F71 | [Throughput and documented limits](paper-trading.md#commands-and-views) | Done |
| F72 | [UI polish](paper-trading.md#order-preview-and-breach-risk) | Done |
| F73 | [Seeded demo soak and JSON output](../CONTRIBUTING.md#python-and-api-contract) | Done |

### Out of scope

| ID | Feature | Reason |
| --- | --- | --- |
| F7 | [Real-time data out of the box for paper trading](configuration.md#providers) | Real-time OPRA option quotes need a paid, licensed feed that an open-source project cannot bundle or redistribute. Traders bring their own provider (Databento, Massive, Tradier or tastytrade). The default Cboe feed stays 15-minute delayed; a pass on it is practice. |
| F18 | [Years of historical option data built in](runtime.md#importing-a-historical-day) | Historical OPRA data is licensed and cannot be redistributed with the project. Recorded sessions, generated scenarios and local Databento/ThetaData historical-day imports with the trader’s own key cover replay and backtests. |
| F67 | [Futures and futures options (/ES options)](paper-trading.md#instruments-and-prices) | A new asset class with its own contract specifications, exchange sessions, SPAN-style margin, settlement into futures and data sources. OpenPort trades equity, ETF and index options and shares only. |
