Notable changes in each release. Older releases: https://github.com/armanruzgar/openport/releases.

## Unreleased

### Providers

- **Interactive Brokers option volume.** Snapshots also ask for field 7762, the day's
  volume, and keep it from real-time rows at their `_updated` time. Delayed and frozen
  rows, and a positive count beside a previous close or halted price or with no last
  price, leave volume unknown. The extra field takes no extra market-data line. Not
  yet run live with an account; reports welcome.

## 0.5.1 (2026-10-28)

The project's new home on GitHub, a Homebrew service that starts, and patch updates
to the web terminal's dependencies.

### Upgrading

- **New project home.** The GitHub account moved from `38st` to `armanruzgar`. The
  image is now `ghcr.io/armanruzgar/openport`, which holds every earlier release tag;
  `ghcr.io/38st/openport` no longer pulls. The repository's Compose files default to
  the new path, but an older checkout, a `.env` or an `OPENPORT_IMAGE` naming the old
  path needs the new one. A Homebrew tap added as `38st/openport` keeps working
  through GitHub's redirect, which holds only while nobody else owns that name;
  `brew tap --custom-remote 38st/openport https://github.com/armanruzgar/openport`
  points it at the new home. The update check, issue links and the server's
  User-Agent use the new address. The update check in 0.5.0 still asks the old
  address and refuses the redirect, so it never reports this release or a later one:
  upgrade 0.5.0 by hand.

### Fixes

- Homebrew: `brew services start openport` failed on current Homebrew, whose service
  block has no `opt_share`; the service now passes `opt_pkgshare/"web"` as its web
  root. The fix reached 0.5.0 installs through the tap.

## 0.5.0 (2026-10-26)

New market-data providers and feed failover, working share orders, splits, queue
fills and real-fill calibration, prop-firm rule types, an Analyze tab with a strategy
optimizer and hedge suggestions, historical stress and VaR, an option tape, outside
portfolio analysis, Linux packages and a backup and upgrade guide. Providers marked
"not yet run live" were tested against documentation-shaped samples or recorded
responses only; reports from live use are welcome.

### Upgrading

Back up the data root first ([upgrade and backup](docs/upgrade-and-backup.md)). An
older `openportd` cannot be assumed to read journals written by this release, and
these formats and defaults changed:

- **Recordings.** Providers that deliver a trade's venue (Databento, and ThetaData or
  Databento imports) now write recording format version 5, which 0.4.0 refuses at
  the header. Every other provider writes the same files as before. Daily
  `--record-dir` files and a feed behind `--fallback-provider` carry the venue
  header as well.
- **Fills CSV.** Every earlier column stays in place; the export ends with three new
  columns, `feed`, `stress_ticks` and `queued`. Fills in the API report the same
  fields, and share fills name their order as `stock_order`.
- **Named token files.** Each secret must be at least eight printable characters; a
  shorter secret makes the file invalid, as a malformed line does. A `#` starts a
  comment only at the start of a field, so a `#` inside a secret is part of it.
  Token files reload live on change, `SIGHUP` or `POST /api/tokens/reload`.
- **Journal maintenance.** `--compact-journals` and `--repair-journals` are separate
  runs: `--compact-journals` refuses `--dry-run` and `--file`, which belong to repair
  (a dry run used to compact for real). An empty write-token file no longer blocks
  startup; it is replaced.
- **Program rules.** Financing rates, pin risk, queue fills, stop stress and every new
  prop-firm rule type are program rules: setting one drops a preset's `plan_id`, and
  financing overrides on evaluation, verification and funded plans are refused with
  `PLAN_LOCKED`. All of them are off by default and in no built-in preset, so existing
  plans keep their journal bytes. Built-in presets are marked `illustrative` in
  `GET /api/plans`.
- **API responses.** The default exposure convention is named `long_calls_short_puts`
  (`open_interest` is still accepted as a deprecated input); without a finite positive
  spot, exposure totals and cells are null with `exposure.available=false` and a
  reason instead of 0. Series and surface-history errors use the standard error body.
  Account WebSocket events are versioned with bounded delivery. Outbound notification
  channels keep their previous default events; `order_state` is opt-in.
- **Docker and systemd.** The image starts through an entrypoint script that adds the
  server settings only to a server start; `--backtest`, `--import-day` and
  `--verify-run` run in the image without them. `docker-compose.yml` reads
  `OPENPORT_IMAGE` and `OPENPORT_VOLUME`. The new Linux packages' systemd unit starts
  with `--no-token-link`, so the token stays out of journald; open the terminal with
  `openportd --print-url --open`.
- **Python.** The client publishes to PyPI as `openport-client` (import name
  `openport`; the PyPI name `openport` is an unrelated project), from `python-v*` tags
  on `main` only.
- **Build.** The Interactive Brokers adapter is behind `OPENPORT_WITH_IBKR` (on by
  default). The build embeds the stress shocks and built-in scenarios and objectives
  from `data/`.

### Providers and historical import

- **Charles Schwab market data.** `--provider schwab` polls Trader API option
  chains with the underlying quote, open interest, volume, delta and gamma, using
  an OAuth refresh token from the environment. Health follows each response's
  `isDelayed`; `isMini`/`isNonStandard` and `mini`/`nonStandard` both mark a
  nonstandard contract. Tested against documentation-shaped samples; not yet run live.
- **Alpaca market data.** `--provider alpaca` polls option snapshots for equity
  and ETF underlyings from the free indicative feed (reported as delayed, derived
  quotes) or OPRA with a subscription. Alpaca has no index options, so SPX and
  other indexes are refused. Not yet run live.
- **Interactive Brokers market data.** `--provider ibkr` reads Client Portal Web
  API snapshots through your logged-in local gateway: it keeps the session alive,
  maps contract ids to OSI symbols, paces requests and counts market-data lines.
  Requires `--expiries` and `--window`. Not yet run live.
- **Feed failover.** `--fallback-provider NAME` switches an underlying to a second
  provider once the main one has been in error or silent for
  `--fallback-seconds` (default 30), and back once it has been fresh for as long.
  A primary labelled stale whose snapshots advance keeps its underlyings, and none
  move onto a backup that could not start or reports an error. Contracts map by OSI
  symbol; the backup reports delayed with a fallback label in status and the
  terminal. Paper trading keeps its freshness checks, and every fill names the feed
  that priced it (`feed` in the fills API and journal).
- **Published AM settlement values.** `--am-settlement cboe` fetches the special
  opening quotations Cboe publishes for SPX, RUT, DJX and VIX (indices SET, RLS, DJS
  and VRO, from Cboe's public daily history) and settles live AM positions only on
  the history row for their exact expiry date, with source `published_soq`. Waiting
  positions show `settle_by: "soq"` while the expiry date is within the fetcher's
  14-day lookback, instead of turning `manual` at 17:00 ET on the expiry date; manual
  import remains the fallback. The index's delayed quote is not used, because its
  trade time cannot tell reliably when it shows the day's value. Options trading and
  rollover wait while a position awaits settlement; Cboe's posting lag is unmeasured.
  Tested with responses recorded on 2026-10-21.
- **Import historical options from Massive (Polygon).** Historical contract discovery,
  paginated quotes and trades preserve vendor timestamps. Quote and trade pages grow
  up to Massive's 50,000-row maximum within the buffer cap. Tested with generated
  saved-response fixtures; paid historical access has not been validated live.
- **Import date ranges and resume.** All importers accept inclusive `--import-days`
  ranges and repeated dates, skip calendar closures, report progress and skip only
  matching complete recordings.
- **Provider certification.** `openport-probe --certify` writes redacted JSON/Markdown
  reports for timed observations or a snapshot cycle, including event/contract
  coverage, clock checks, Greek scale hints and errors, for the new provider report
  issue template. Demo/replay reports are labelled as such and do not claim live
  validation.
- **Greek and volume mappings.** Documented ORATS/DXLink Greeks and ThetaData vega/rho
  scales are mapped; ThetaData theta remains unknown. Tradier chain and DXLink session
  volume are mapped without inventing trades or timestamps, and Tradier volume from an
  earlier trading date is withheld. Sample/mock tested, not yet validated live.
- Option prints carry the printing venue where the provider gives one. Unknown
  ThetaData snapshot times are preserved; unused DXLink Profile subscriptions are
  gone, supplied Summary event times are kept for open interest, and tastytrade
  subscribes only to the events it reads, raising the contracts its cap allows.
  A ThetaData `Retry-After` outside 1-60 s, or a date, backs off instead of failing
  the import. `/api/status` reports `provider.extended_hours_prints`, false for Cboe
  and the demo market.
- `--provider replay` without `--symbols` plays the recording's symbols.

### Recording and replay

- **Record across trading dates.** `--record-dir` rolls at calendar boundaries and
  publishes only complete files, with optional oldest-first age/size retention that
  protects active files. Replay lists trading dates, symbols and sources across
  providers. Retention runs off the feed thread at rollover, reports files it cannot
  remove instead of stopping recording, and counts stale staging folders toward the
  size cap.
- Replay pacing survives several pauses and resumes before the pacing loop observes
  them. A replay stall no longer moves orders to another account, and a new replay
  run drops the last run's market data in the terminal.
- Backtests and replays: a damaged run or sidecar no longer blocks the rest, failed
  resumes and cancelled checks lose nothing, listings resume, and rule codes and
  archived modes stay straight. Replay CSV exports name the run as the account.

### Trading, financing and halts

- **Working share orders.** Stock and ETF shares take resting market, limit, stop and
  stop-limit orders (`POST /api/stocks/orders`, `/preview`, `GET`,
  `DELETE /api/stocks/orders/{id}`), DAY or GTC, with extended-hours limits
  (premarket and after hours), brackets and OCO. Opening orders reserve buying power
  and see working sells in buy-only and cash-account checks; each fill takes the
  share checks, and a bracket stop satisfies `require_stop_loss`. Market share orders
  reserve buying power at the last price from acceptance and need one; resting share
  fills take `no_counter_positions` as option fills do. The share ticket places them
  and lists working ones; the Python client adds `place_stock_order`,
  `preview_stock_order`, `stock_orders` and `cancel_stock_order`. Buy-writes at one
  net price are not offered.
- Extended-hours share limits fill only on prints fresh within premarket or after
  hours, never on the regular close, which outside the regular session keeps its own
  print time even from a current snapshot. The share ticket disables Extended hours
  with a caption while the feed reports no extended-hours prints.
- **Splits and adjusted deliverables.** `--corporate-actions FILE`
  (`SYMBOL,YYYY-MM-DD,split,N:D`) splits held shares on their ex-date at the
  rollover, paying cash in lieu of a fraction, and adjusts the deliverable of every
  share-settled series on the underlying; settlement, exercise, assignment, strategy
  margin (lots of the adjusted deliverable), cash-account coverage and risk repricing
  follow it, and the desk quotes adjusted series from the standard series that pays
  the same. Listed adjusted series trade when their deliverable is supplied instead
  of `NONSTANDARD_UNSUPPORTED`. Positions report `deliverable`, accounts their
  `corporate_actions`; working share orders on a split stock end with
  `CORPORATE_ACTION`. Recorded runs keep the splits as an input.
- Adjusted series no listed series pays the same as, or whose listed equivalent never
  quotes, are valued by model from the strikes around them, floored at their
  deliverable's exercise value in the session's own decimal arithmetic, reported as
  `modelled_mark`, never fill, and no longer stall the rollover. A contract left with
  less than one share delivers cash only and is worth that fixed payoff:
  undiscounted for an American series, discounted for a European one, the discounted
  value being what historical stress margins a short one at.
- **Financing, pin risk and halts.** Practice cash transfers, rollover credit/debit
  interest and stock-borrow fees are journaled and visible in Dashboard; financing
  counts in equity and daily loss without adjusting its opening base. Opt-in seeded
  expiry pin risk models contrary exercise near the strike and warns before expiry.
  Single-name scenario/replay halts pause one underlying, with recorded, verifiable
  halt-drill commands in the replay API and Dashboard. Shares halted through the
  close roll over on their last pre-halt print, reported as `halted`; prints inside
  a simulated halt are not taken as marks.
- What-if accepts OTO/OCO chains, evaluates their current-quote fills and contingent
  orders on a private account copy, and applies submission's opening gates (volume
  caps and other opening rules) to parents and every child, as submit does.
- Exercise of a defined-risk position never leaves short shares; starting cash must
  be positive; early-close days have their own hold cutoffs; ETF scenario prices
  settle at the 16:00 close; and expiry warns and alerts when share delivery would
  overdraw the account.
- Exercise opens shares through the plan's share-entry rules (entry windows,
  `require_stop_loss`, hedging, `max_trade_risk`, exposure) as a share trade does;
  shares that only reduce a holding are unaffected. A session refuses a size-scaling
  maximum below its starting balance at construction, as a reset does.
- **Copy and allocation trading.** Named account groups
  (`PUT/DELETE /api/account-groups/{name}`, `GET /api/account-groups`, admin to
  change) and `POST /api/orders/copy` (and `/preview`) mirror one order to every
  member as its own submission, sized by fixed ratio or split by marked equity, each
  with its own checks and journal; the reply lists each account's status and
  response. An equity-allocated copy skips an unmarked member with `STALE_QUOTE`
  instead of failing. The Python client adds `account_groups`, `put_account_group`,
  `delete_account_group`, `copy_order` and `preview_copy_order`; Manage accounts
  edits groups and the option ticket offers **Copy to group** with several live
  accounts.

### Evaluation and prop rules

All new rule types are off by default and in no built-in preset.

- Open-loss cap on one trade's marked loss, net across its legs (fixed, or a share of
  the start-of-day profit balance or drawdown room), that closes the whole trade (a
  spread as one order, shorts first, never leaving a naked leg, with adds refused
  while the close is pending), blocks adds or fails the attempt (`OPEN_LOSS_CAP`),
  journaled as `open_loss_cap` events.
- Balance drawdown basis: the floor follows and checks the closed balance, so it can
  be set per phase; a static floor ignores the hidden lock balance. Best-position cap
  on the profit one trade counts toward the target (`BEST_POSITION` objective);
  opening-order cap on the last minute's option volume, refused with `MINUTE_VOLUME`
  when that volume is unknown.
- Payout reviews of short-held, oversized or open-loss trades (share round trips
  included; a trade closed at a payout's own timestamp is reviewed by that payout
  only) escalate warning, forfeit and cap, in the payout preview and the journaled
  payout. The floor after a payout can stay, reset the account to a share of its
  starting balance, or move up to it; payout caps and margin follow the plan.
- `POST /api/account/rules-compare` replays the attempt's recorded days against
  presets or custom rules side by side, adding back everything a payout took out
  (forfeits and reset removals too); payout reviews and the floor after a payout are
  listed as not replayed. The Rules page shows where each passes, fails or locks.
- Versioned rule packs (`effective_from`, compared with today's New York date,
  `source`, `verify`) validate through `POST /api/rule-packs/validate`, which refuses
  `unlocked_by` cycles; the terminal imports, exports and starts custom attempts from
  them, apart from the presets.
- `pass-odds?economics=true` adds challenge economics: expected value and break-even
  pass rate from each program step's odds, fees and resets and the first funded
  payout.
- Opt-in discipline guardrails: a losing-streak cooldown and size-creep warnings or
  blocks against the median of recent opening orders, journaled as `tilt_flag`
  events.
- The flat-time countdown knows weekends and market holidays. Plan imports keep edits
  made meanwhile.
- Python client and MCP: `rules_compare`, `validate_rule_pack` and economics on
  `pass_odds`.

### Fill models and fill quality

- **Queue fills** (`fill_model: "queue"`, `queue_fills`, `queue_seed`): a resting
  single-leg limit at the touch waits behind the size displayed when it joins; trade
  prints at its price use that queue up before it fills at its limit, prints through
  its price fill it, and cancellations come out of the queue in proportion. Without
  prints, volume increases while the touch stays at its price (half each side) or a
  documented five-minute turnover of the displayed size stand in; the turnover fills
  orders even when nothing traded at their price. Time priority follows when an order
  reached its price (after latency, or the change that sent it there), and only a
  smaller size keeps its place: a larger size or any other change goes to the back.
  Fractions round at random from the seed, so the same feed always gives the same
  fills. Orders show the estimate and wait with `QUEUE`. Recordings' trade prints
  reach only accounts using it; others keep their bytes.
- **Stop stress** (`stop_stress_ticks`, `stop_stress_percent`): stops reached while a
  contract's spread or quote rate is a chosen multiple of its normal slip extra ticks
  on their first fill attempt, never past a stop-limit's limit, reported on the order
  and its fills. A stop-limit that rests after it fills later unstressed. Off by
  default.
- Daemon flags `--paper-fill-model queue`, `--paper-queue-seed`,
  `--paper-stop-stress-ticks` and `--paper-stop-stress-percent` seed a new main
  journal. Latency, slippage, impact and inside fills stay outside the program rules.
- **Fill quality** (`GET /api/fills/quality`, Journal): fills placed between the mid
  and the natural by time of day, spread width and DTE, with a heuristic
  Conservative calibration.
- **Real-fill calibration** (`POST /api/fills/calibration`, Journal): import generic,
  tastytrade, thinkorswim/Schwab or IBKR fills, compare each with the same-time
  recorded quote and what the paper models would have done, and get suggested
  settings. Analysis only: nothing is stored or traded. It needs a read token (not
  anonymous where reads are public; never a sandbox), runs one at a time on a worker
  (503 `CALIBRATION_BUSY`) and searches at most 31 days of fills, 32 recordings,
  4 GiB and 20 seconds, newest first, stopping once every fill has a quote; `search`
  reports how far it got. Quantities above a million contracts are skipped. The
  broker layouts are tested on sample files, not live exports.
- Python `Client.fill_quality()` and `Client.calibrate_fills()`.

### Risk

- **Historical stress and VaR.** Cited historical stress scenarios and 1-day
  historical VaR/expected shortfall, with an observed ATM-IV-change variant when
  available. 99% VaR/ES needs 100 observations (95% still needs 20); below that
  `var99`/`es99` are null and `tail99_reason` says why. Read-only historical risk and
  scenario endpoints, with Python and MCP methods.
- **Scenario explorer.** Beta-aware spot × volatility × time scenario grids, with
  optional term scaling; existing rule grids retain their defaults. The spot ×
  volatility grid and the risk profile can reprice with sticky-moneyness or
  sticky-delta smiles along each underlying's fitted SSVI surface (`dynamics=`),
  beside the default sticky strike, in every explorer mode with a Smile selector;
  underlyings without a surface stay sticky strike and are named. Rules, margin and
  breach estimates are unchanged.
- **Caps and alerts.** Gamma, theta, reachable grid-loss and gross short-vega caps,
  with tag and playbook scopes and evaluation rollover discipline. Account alerts
  for utilisation, grid loss, gamma and modelled floor-touch odds through existing
  notification channels. A tagged order that reduces an untagged book over its scope
  cap is no longer refused: the scope is valued over the same positions with and
  without the order. Scenario grids for grid-loss caps and alerts are remembered by
  their exact inputs, so order checks stop repricing every open order's grid.
- Portfolio Greeks persist beside equity, so the terminal charts risk intraday and
  across days without reconstructing old values.

### Analyze

- **Analyze tab.** Trade's new tab draws up to four candidate strategies' P&L by
  price on today, a custom date and their first expiry, with or without the held
  book, under an IV-shift slider; a price-slices table (% moves, prices, ±σ,
  breakevens); strikes dragged on the chart or stepped per leg; and Send to ticket.
  `GET /api/risk/profile` takes hypothetical `legs` and `held=false` so candidates
  use the held book's own model; leg quantities are bounded and doubled signs
  refused.
- **Strategy optimizer.** `GET /api/underlyings/{symbol}/optimizer` searches
  verticals, iron condors, butterflies, calendars, singles and covered calls for a
  target price, date and loss budget, ranked by expected P&L under the market's or
  the user's shifted lognormal, probability of profit or return on risk. Candidate
  counts are capped (an even sample past the cap) and searches run on a two-worker
  pool that answers 503 `SEARCH_BUSY` when full, never on HTTP I/O threads; a search
  stops early when its client disconnects or the server stops, and queued ones are
  dropped at shutdown. A chain without an at-the-money term structure uses the
  expiry's ATM IV over the time (`variance_fallback`) rather than a zero-width
  distribution. Derived estimates, not advice; fees and early exercise are not
  modelled.
- **Hedge suggestions.** `GET /api/risk/hedges` gives the smallest share, ATM option
  or SPX/SPY beta hedge bringing a bucket's delta, vega or worst grid cell back
  inside its limit or above the floor, ranked by spread cost and residual risk. For
  a delta target the SPY/SPX hedge is sized on the beta-weighted dollar delta to SPY
  (the risk profile's betas, overridable with `betas=`) and reports the residual
  beta-weighted delta; a missing beta is named and the hedge falls back to one for
  one. Positions sends an option hedge to the what-if and then to its ticket.
- Python `optimize_strategies`, `hedges` and `risk_profile(legs=, held=)` with
  matching MCP tools.

### Volatility

- **Surface history.** Per-expiry SVI parameters persist at scheduled intraday slots
  and the close, one close per session on disk, loading 13 months and keeping 400
  days in memory; compare the same expiry against the prior close, week and month.
  Range lists return at most 400 snapshots with a `truncated` flag, and Python
  defaults to comparisons.
- **Event volatility.** An event variance premium is derived from adjacent straddling
  expiries using supplied labels and earnings/news calendars, including BMO/AMC;
  missing brackets and confounded solves remain explicit. Estimates count one event
  per date and session, and an event at a settlement instant solves after that
  expiry.
- **Vol scanner.** Batch API and sortable terminal view for every subscribed symbol,
  with history coverage, feed timestamps, simulation labels and symbol navigation;
  rows are cached per publication.
- **Volatility history.** Ranks, percentiles and population z-scores for signed skew
  and term ratios, plus a same-session VIX-minus-MFIV tracking line when observed.
  Live comparison measurements remain pending.
- Python and MCP read stored volatility and surface history and the batch scanner,
  and select the account calendar for event estimates.
- Deep in the money, time value at the price's rounding noise is reported as below
  intrinsic instead of solved to a wrong volatility.

### Exposure

- OI-weighted delta, daily charm and vomma exposure, per-strike and expiry totals,
  signed walls and sticky-strike spot flips alongside GEX/VEX.
- Positioning conventions and snapshot-date 0DTE/ex-0DTE slices through the API,
  terminal, Python client/frames and MCP. The default long-call/short-put convention
  is preserved and named `long_calls_short_puts`; customer flow is reported as
  unavailable without aggressor-side data. Formulas, units, coverage and search
  limits are documented.
- Charm's carry and yield rates use the floored exposure horizon, so a small
  forward/spot gap near expiry no longer dominates 0DTE CEX. Flip searches bisect
  only the brackets nearest spot and reprice one metric; a convention or slice
  request copies only the selected expiries.

### Pricing models

- **Closed-form American models.** `--american-model bs2002|baw` removes the early
  exercise premium with Bjerksund-Stensland (2002) or Barone-Adesi-Whaley instead of
  the 31-step Leisen-Reimer tree. The tree stays the default: measured against a
  converged tree it was more accurate, and no slower. Expiries with cash dividends
  always use the tree; each expiry reports its `eep_model`.
- **SABR beside SVI.** The Volatility view can overlay per-expiry SABR fits (Hagan
  lognormal, β fixed at 1 or ½, or fitted) with their parameters, RMSE and the same
  butterfly and calendar checks. The surface API fits them only when asked
  (`sabr_beta`, 0 to 1 in steps of 0.1, or `fit`; at most 60 expiries), so nothing
  changes for analytics or other readers.
- **Heston calibration (research, API only).** `GET /api/underlyings/{symbol}/heston`
  fits one Heston parameter set to the leading expiries and reports it beside each
  expiry's SVI and SSVI RMSE. Its parameters are weakly identified on real smiles;
  nothing else uses it. Heston and SABR fits run on a background worker, one per
  snapshot at a time, so they never stall other API reads; a request for a snapshot
  already fitting gets 503 with `Retry-After`.

### Flow, earnings and outside portfolios

- **Option tape.** A Flow tab on Trade shows time and sales from feeds that deliver
  option trades (Databento, and recordings or imports that hold them), each print
  classified at the bid, at the ask, mid or outside. Sweeps, blocks and best-effort
  multi-leg groups are inferred, and net premium is summed by expiry and strike.
  Feeds without trades say so plainly; only the demo market adds prints, labelled
  simulated and never grouped into spreads. `GET /api/underlyings/{symbol}/flow`.
- **Earnings moves.** For each earnings event in the event calendar, the front
  straddle's implied move before the news and the realized move after it build a
  history on the Volatility page, with how often the straddle priced more than came.
  Past quarters can be imported from a CSV and stay labelled imported.
  `GET /api/earnings`.
- **Outside portfolios.** Import a positions CSV (generic columns, or tastytrade,
  Schwab or IBKR exports) into an analysis-only portfolio and see its Greeks, risk
  profile, scenario grid, historical stress tests and one-day historical
  VaR/expected shortfall on your feed, without the floor or paper margin an account
  would have. It is not a paper account and can never be traded. Every route, reads
  included, needs admin; imports may be up to 4 MiB. `/api/analysis/portfolios`.
- The Python client and MCP server read the tape, the earnings history and analysis
  portfolios.

### Journal and practice

- Journal: entry-context edge reports, persisted mistake/setup picks and
  with/without cost comparisons; transparent trade coach flags and an opt-in,
  user-configured HTTP AI review hook. A journal fill-quality panel.
- Replay: start a fresh paused account from a reviewed entry with the original
  source/seed/date/plan, and mark authored scenario events on review/equity charts.
  Drill eligibility derives from the hash chain.
- Practice: grade declared objectives from journal states, retain scores and
  personal bests, and offer spaced fresh-seed practice with completion streaks.
- Scenarios: import/export packs and fixed challenges, including challenge inputs
  and recomputed drill results in replay verification receipts. Morning crush,
  Afternoon waterfall and Range bleed have objectives in a separate built-in
  objectives file, so their sources and earlier runs' hashes are unchanged. Two user
  scenario files with one id are reported.

### Terminal

- Keyboard help, symbol jump, chain contract navigation, buy/sell prefills,
  review/cancel shortcuts and focused-position close through the existing ticket.
  Enter on buttons and quotes acts on their own side; Escape keeps strategy legs and
  any mode change clears tickets.
- Chain settings remembered per symbol, named route/preference workspaces, a
  subscribed-symbol watchlist, and persistent local notification history with unread
  counts, mark-all-read and clear beside the existing toasts.
- Large chains are windowed with unchanged rows reused and collapsible stacked
  expiries; the chain steps and scrolls from the expiry shown, and chain legs quote
  outside the strike window.
- SMA, EMA and Bollinger studies plus saved per-symbol horizontal lines and
  trendlines, including price-alert creation; plan levels draw as dashed lines and
  slow swipes pan. VWAP remains unavailable on the current volume-free candle API;
  its math is tested with sample volumes.
- Chain, chart, positions and orders pop-outs, an optional wide-screen
  positions/orders split, and phone calls/puts selection with bottom-sheet tickets.
- Orders: edits follow the live order, corrected orders and exits go out under a new
  client order ID with take-profits on the entry's side, picks drop closed legs,
  rolls name the chosen strike, and trade risk reads in dollars. Backtests drop
  deleted runs and balance targets measure the balance.
- Alerts: the opt-in update check runs on plain HTTP, price alerts wait for a price
  and stay in step across tabs, a new attempt alerts again, and saved alert claims
  and brief levels stay current. A socket blip keeps the account scope; a slow
  browser socket keeps the newest frame of each tick stream, so live status no
  longer freezes behind a replay.
- A `#token=` link opened in a tab already showing the terminal is adopted there.

### API, Python and automation

- Versioned account WebSocket events with bounded delivery and typed Python
  iteration; the stream dispatches only to the addressed account's subscribers and
  sends `floor` only on entering or leaving the warning band.
- `openport.bot` strategy hooks for live paper, replay and saved history, with REST
  fallback.
- Inbound playbook signals: webhook senders use a per-playbook, per-account secret
  (issued, rotated and revoked by an admin, stored only as SHA-256) as an HMAC
  signature with a ±300 s timestamp window, or in the body over HTTPS for senders
  such as TradingView that cannot set headers; bearer clients use
  `POST /api/playbooks/{id}/evaluate`. Nonces and rate limits are per secret, and
  saved manual/automatic modes remain authoritative. Removing an account or expiring
  a sandbox ends its forward tests and playbook modes.
- Bounded backtest parameter sweeps with exact variant reports and historical
  ranking; `DELETE /api/backtests/sweeps/{id}?purge=true` removes a saved sweep, as
  for single runs. Optional sweep fields and comparison IDs are validated.
- Optional Arrow/Parquet exports preserving money text, with an offline example
  notebook.
- Python client coverage for every documented HTTP operation, with a contract parity
  guard; MCP `place_order` returns the client order id it sent, and `Fill` carries
  the optional feed. MCP tools for playbooks, staged orders, backtests, alerts,
  journals, what-if, market views and order changes retain account scopes and
  preview guidance.
- Server: open HTTP connections are capped at 512, a failed accept waits instead of
  spinning, browser sockets accept legacy tokens with spaces or UTF-8, a failed HTTP
  retry closes its connection, and CSV exports neutralise cells a spreadsheet would
  run as formulas.

### Install, access and support

- **Open the terminal without hunting for a token.** `openportd --open` launches
  the browser after startup; `--print-url [--open]` reads existing token settings
  and exits, and `--address localhost` names the host as given. The startup link
  percent-encodes its token, so tokens with `&`, `#` or `%` work. `--no-token-link`
  prints the `--print-url` command instead of the token link at startup. `--open`
  documents, and on Linux warns, that the launcher's arguments are visible in `ps`.
- **Install on more platforms.** Release automation packages Linux amd64/arm64
  `.deb` and `.rpm` assets with a private systemd service. Intel Macs build the
  release tag through Homebrew; Windows Docker Desktop/WSL2 paths are documented.
- **Keep data through upgrades.** New backup, restore, rollback and journal-repair
  guidance covers native, Homebrew, Docker and systemd storage. Remote/phone
  recipes use Tailscale or Caddy with cookie authentication.
- **Start and find help more easily.** The site adds Homebrew installation tabs,
  first-ten-minutes links and Discussions. Bug forms accept sanitized diagnostics;
  Discussions templates cover questions and ideas.
- **Prepare a hosted demo.** `deploy/demo` runs simulated prices and bounded
  sandboxes behind authenticated HTTPS, with container resource limits. The site
  keeps its browser-demo button hidden until a real URL is configured. Nothing is
  hosted or published by this change.
- **Share diagnostics deliberately.** Status copies version/build/platform,
  provider/feed and recent error categories into a reviewable bug report. Local
  fatal signal logs (`crash.log`) contain no memory dump; there is no telemetry.
  `/api/status` adds optional build metadata for clients.
- The demo market's scenario revision 9 covers seven underlyings, gives every strike
  of an ETF monthly its monthly open interest, holds SPY option vols after the close
  and an overnight index at `previous_close`, and makes an authored dividend its
  quarter's only payment.

### Fixes

- A clean shutdown no longer sends a feed-stalled notification for every
  underlying.
- Playbooks saved under the 8 MiB limit are written within it, so startup accepts
  them; a journal failure behind an automatic playbook cancel or close fails only
  that account instead of the engine thread.
- A holding window ending at `24:00` is midnight of the next day, so hold
  restrictions are enforced. The Cboe holiday listing drops an upcoming closure it
  no longer lists.
- Imported fill quantities stay within a million contracts and paper outcomes sum
  whole blocks, so misread columns cannot overflow totals.
- Market-halt state is synced before rename and a failed write is retried; sandbox
  notes, alerts and trade grouping share the order rate limit.

## 0.4.0 (2026-10-06)

### Evaluation plans

- **Practise more kinds of evaluations.** Static floors, floors that stop trailing,
  daily loss rules, minimum days and best-day consistency join the custom plan
  editor. Dashboard shows what remains to pass.
- **Set trade-level objectives.** Require closed whole option trades, limit the
  biggest trade's share of profit, set minimum holding time or cap profit from
  very short trades. Protective and system exits still work during a minimum hold.
- **Control when and what you trade.** Set evaluation and inactivity deadlines,
  allowed symbols, entry hours, mandatory flat time and no-overnight rules. Saved
  event calendars add news blackouts and weekend, earnings and dividend restrictions.
- **Limit opening exposure.** Cap contracts held, require stops, limit risk per
  trade, forbid opposing positions or cap holdings against traded option volume.
  Volume caps refuse entries when volume is unknown or stale; stops can still slip.
- **Follow a two-step programme.** A passed challenge unlocks verification, then
  simulated funded trading. Add payout consistency, retained buffers, contract
  scaling and funded size reviews. Fees and reset limits track programme costs
  without deducting trading cash. Funded screens stay hidden unless enabled or
  already funded; OpenPort funds no one and pays no real withdrawals.

### Orders, exits and fills

- **Keep protection through partial fills.** Brackets grow with entries and shrink
  with exits. Attach stop/target pairs to held options or spreads, use stop-limits
  or trailing stops, or link orders so one cancels or triggers another.
- **Trade across sessions.** All-session orders and timestamp expiry join DAY/GTC.
  Conditions can watch another symbol, volatility or a New York time. Overnight
  and curb execution needs limits; new orders, including GTC, are refused between
  product sessions. Missing trigger data does not trigger an order.
- **Choose inside fills and walking limits.** Limits can fill at midpoint or a
  chosen share of the spread, or move in steps toward a cap. These are optional;
  As displayed remains the default. Neither model simulates queue position.
- **Preview costs, fills and order changes.** Choose flat or itemized fees and see
  why an order waits. Fills retain the quote, displayed size and quote age.
- **Close and adjust the whole position.** Roll strikes or an entire condor;
  Flatten keeps spread closes working across quotes and preserves remaining
  protection. Preview what it leaves behind, or use limit flatten outside regular
  hours. Worthless longs can be abandoned, or marked do-not-exercise for expiry.

### Shares, margin and risk

- **Trade stock and ETF shares** during stock regular hours, with buying-power and
  delta previews. Covered-call and collar templates buy shares first: shares remain
  if the option order fails or partly fills. Automatic playbooks still trade options.
- **Choose cash, IRA or margin accounts.** Add house requirements or portfolio
  volatility shocks. Strategy margin recognises share covers, straddles and
  combinations across expiries, showing the allocation and any conservative fallback.
- **Compare adjustments before trading.** What-if compares buying power, Greeks,
  scenario losses and floor room. Held-book warnings and per-symbol limits help
  track exposure; risk profiles add later dates, IV changes, beta weighting and
  probability cones. These are model estimates, not predictions.

### Journal and account review

- **Keep rolls and adjustments in one whole trade.** Group entries for review,
  retain planned risk, and compare return on buying power and P&L by Greek.
  Exports identify the attempt, replay, scenario and seed, and label forced exits.
- **Inspect settlement sources.** Journal shows references and provenance. New
  demo and replay runs use the expiry opening print for AM settlement, an
  approximation of the official quotation; live feeds still need manual imports.
- **Manage accounts.** Rename, archive or delete accounts, or copy risk settings.
  Deleted files remain; damaged journals are readable through verified history.

### Replays and practice days

- **Carry one account through several sessions** with overnight gaps, GTC orders,
  assignment and dividends. Scenarios add XSP, NDX, RUT and VIX, American ETF
  pricing and simulated quarterly SPY/QQQ dividends. Stress days can include bad
  quotes, feed stalls, halts and early closes; these are not historical reconstructions.
- **Step, watch or retry a drill.** Advance by time or the next snapshot, play at
  a chosen speed to a target, or interrupt a long step. Restart at a chosen time
  with the original commands through that point in a new run, keeping the source.
- **Resume unfinished runs after shutdown** and verify finished runs in the
  terminal, with a downloadable receipt. Verification detects changed inputs and
  damaged history; identical results are not promised across different platforms.

### Playbooks and backtests

- **Add technical entry and exit rules.** Use moving averages, RSI, Bollinger
  bands, VIX levels and opening gaps; manage trailing profit, days to expiry and
  trading-day deadlines. Missing history blocks entries rather than inventing data.
- **Forward-test automatically on live paper accounts.** Auto resumes after restart
  and compares each version with saved backtests. Normal trading checks still apply.
- **Test two to eight playbooks together** on one account with shared buying power
  and plan limits, plus results for each playbook and daily evaluation progress.
  Comparing and summing separate runs remains an independent-results comparison.
  Open positions at the end stay reported, without invented closing prices.

### Alerts and upgrade notes

- **Keep account alerts running with the terminal closed.** Watch options, spreads,
  IV, equity, floor room and exposure, with optional external delivery. Demo and
  replay forwarding needs explicit opt-in; backtests and sandboxes never forward.
- **Older journals still load.** Saved scenario runs retain their original price
  generation for verification. Resume/restart requires supported runs and unchanged
  inputs; for an older live demo journal, keep its original revision and day settings.
- **Resets preserve daily discipline.** Profit locks, trade limits, opening counts
  and active cooldowns survive an account reset. New 100% soft-floor settings are
  refused, though stored ones still load. Passed or failed attempts can still close.
- **Pin backtests you want to keep.** The server now defaults to retaining 20
  finished unpinned runs, pruning older excess runs before starting another.
  Set retention to zero for unlimited storage; CLI reports are outside this policy.
- **Practice is not a prediction.** Delayed quotes give hindsight, and generated
  days cannot establish future performance. Historical importers and providers
  other than Cboe, Demo and Replay still have not been run live with a key/account.

### Fixed

- **Closing and buying power** now handle partial protection, worthless wings,
  shared covers and margin released by fills more consistently. Reductions can
  proceed over exposure limits, but removing a hedge can still increase risk and fail.
- **Replay timing** keeps same-time quotes together, preserves gaps after stepping
  and refuses out-of-range steps before moving. Storage failures are clearer.
- **P&L and history** preserve prior-day marks, dividends and whole-trade labels.
  Resets restore original scaled plan limits. Profit-taking exits no longer start
  a stop cooldown unless the separate loss threshold is met.
