Notable changes in each release. Older releases: https://github.com/38st/openport/releases.

## Unreleased

### Providers and historical import

- Add `openport-probe --certify` with redacted JSON/Markdown reports
  for timed observations or a snapshot cycle, including event/contract coverage,
  clock checks, Greek scale hints and errors. Add a provider report issue template;
  demo/replay reports are explicitly labelled and do not claim live validation.
- Preserve unknown ThetaData snapshot times; remove unused DXLink
  Profile subscriptions and retain supplied Summary event times for open interest.
- Map Tradier chain and DXLink session volume without inventing trades
  or timestamps. Map documented ORATS/DXLink Greeks and ThetaData vega/rho scales;
  ThetaData theta remains unknown. Sample/mock tested, not yet validated live.
- **Import historical options from Massive (Polygon).** Historical contract discovery,
  paginated quotes and trades preserve vendor timestamps. Tested with generated
  saved-response fixtures; paid historical access has not been validated live.
  Quote and trade pages grow up to Massive's 50,000-row maximum within the buffer cap.
- **Import date ranges and resume.** All importers accept inclusive `--import-days`
  ranges and repeated dates, skip calendar closures, report progress and skip only
  matching complete recordings.

### Recording and replay

- **Record across trading dates.** `--record-dir` rolls at calendar boundaries and
  publishes only complete files, with optional oldest-first age/size retention that
  protects active files. Replay lists trading dates, symbols and sources across providers.
  Retention runs off the feed thread at rollover, reports files it cannot remove
  instead of stopping recording, and counts stale staging folders toward the size cap.
- **Preserve replay pacing across pauses.** Clarify consumption of accumulated paused
  duration and cover multiple pauses/resumes before the pacing loop observes them.

### Journal and practice

- Journal: add entry-context edge reports, persisted mistake/setup picks and
  with/without cost comparisons; transparent trade coach flags and an opt-in,
  user-configured HTTP AI review hook.
- Replay: start a fresh paused account from a reviewed entry with the original
  source/seed/date/plan, and mark authored scenario events on review/equity charts.
- Practice: grade declared objectives from journal states, retain scores and
  personal bests, and offer spaced fresh-seed practice with completion streaks.
- Scenarios: import/export packs and fixed challenges, including challenge inputs
  and recomputed drill results in replay verification receipts. Morning crush,
  Afternoon waterfall and Range bleed have objectives in a separate built-in
  objectives file, so their sources and earlier runs' hashes are unchanged.

### Terminal

- Add keyboard help, symbol jump, chain contract navigation, buy/sell prefills,
  review/cancel shortcuts and focused-position close through the existing ticket.
- Remember chain settings per symbol and save named route/preference workspaces.
- Add a subscribed-symbol watchlist and persistent local notification history with
  unread counts, mark-all-read and clear; keep existing toasts.
- Window large chains, reuse unchanged rows, and stack collapsible expiries.
- Add SMA, EMA and Bollinger studies plus saved per-symbol horizontal lines and
  trendlines, including price-alert creation. VWAP explicitly remains unavailable
  on the current volume-free candle API; its math is tested with sample volumes.
- Add chain, chart, positions and orders pop-outs, an optional wide-screen
  positions/orders split, and phone calls/puts selection with bottom-sheet tickets.

### Analyze

- **Analyze tab.** Trade's new tab draws up to four candidate strategies' P&L by
  price on today, a custom date and their first expiry, with or without the held
  book, under an IV-shift slider; a price-slices table (% moves, prices, ±σ,
  breakevens); strikes dragged on the chart or stepped per leg; and Send to ticket.
  `GET /api/risk/profile` takes hypothetical `legs` and `held=false` so candidates
  use the held book's own model.
- **Strategy optimizer.** `GET /api/underlyings/{symbol}/optimizer` searches
  verticals, iron condors, butterflies, calendars, singles and covered calls for a
  target price, date and loss budget, ranked by expected P&L under the market's or
  the user's shifted lognormal, probability of profit or return on risk. Candidate
  counts are capped (an even sample past the cap) and searches run on a two-worker
  pool that answers 503 `SEARCH_BUSY` when full, never on HTTP I/O threads; a search
  stops early when its client disconnects or the server stops, and queued ones are
  dropped at shutdown. A chain
  without an at-the-money term structure uses the expiry's ATM IV over the time
  (`variance_fallback`) rather than a zero-width distribution. Derived
  estimates, not advice; fees and early exercise are not modelled.
- **Hedge suggestions.** `GET /api/risk/hedges` gives the smallest share, ATM option
  or SPX/SPY beta hedge bringing a bucket's delta, vega or worst grid cell back inside
  its limit or above the floor, ranked by spread cost and residual risk. For a delta
  target the SPY/SPX hedge is sized on the beta-weighted dollar delta to SPY (the risk
  profile's betas, overridable with `betas=`) and reports the residual beta-weighted
  delta; a missing beta is named and the hedge falls back to one for one. Positions
  sends an option hedge to the what-if and then to its ticket.
- Python `optimize_strategies`, `hedges` and `risk_profile(legs=, held=)` with
  matching MCP tools.

### Volatility

- **Surface history:** persist per-expiry SVI parameters at scheduled intraday slots
  and the close; compare the same expiry against the prior close, week and month.
- **Event volatility:** derive an event variance premium from adjacent straddling
  expiries using supplied labels and earnings/news calendars, including BMO/AMC.
  Missing brackets and confounded solves remain explicit.
- **Vol scanner:** batch API and sortable terminal view for every subscribed symbol,
  with history coverage, feed timestamps, simulation labels and symbol navigation.
- **Python/MCP:** read stored volatility/surface history and the batch scanner,
  and select the account calendar for event estimates.
- **Volatility history:** ranks, percentiles and population z-scores for signed skew
  and term ratios, plus a same-session VIX-minus-MFIV tracking line when observed.
  Live comparison measurements remain pending.
- **Volatility fixes:** surface history keeps one close per session on disk, loads
  13 months and keeps 400 days in memory; range lists return at most 400 snapshots
  with a `truncated` flag, and Python defaults to comparisons. Scanner rows are cached
  per publication. Event estimates count one event per date and session, and an
  event at a settlement instant now solves after that expiry.

### Exposure

- Add OI-weighted delta, daily charm and vomma exposure, per-strike and
  expiry totals, signed walls and sticky-strike spot flips alongside GEX/VEX.
- Expose positioning conventions and snapshot-date 0DTE/ex-0DTE slices
  through the API, terminal, Python client/frames and MCP. Preserve the default
  long-call/short-put convention; report customer flow as unavailable without
  aggressor-side data. Document formulas, units, coverage and search limits.
- Charm's carry and yield rates now use the floored exposure horizon, so a
  small forward/spot gap near expiry no longer dominates 0DTE CEX. Flip searches
  bisect only the brackets nearest spot and reprice one metric; a convention or
  slice request copies only the selected expiries.
- API: the default exposure convention is named `long_calls_short_puts` in
  responses, the terminal, Python client and MCP. `open_interest` is still accepted
  as a deprecated input for the same convention.
- API: without a finite positive spot, exposure totals (`gex`, `vex` and the other
  metrics) and per-strike and per-expiry cells are null instead of 0, with
  `exposure.available=false` and a reason.

### Risk

- Cited historical stress scenarios and 1-day historical VaR/expected shortfall, with an observed ATM-IV-change variant when available.
- Gamma, theta, reachable grid-loss and gross short-vega caps, with tag and playbook scopes and evaluation rollover discipline.
- Account alerts for utilisation, grid loss, gamma and modelled floor-touch odds through existing notification channels.
- Terminal: persist contemporaneous portfolio Greeks beside equity; chart risk intraday and across days without reconstructing old values.
- API: read-only historical risk and scenario endpoints, with Python and MCP methods.
- Beta-aware spot × volatility × time scenario explorer, with optional term scaling; existing rule grids retain their defaults.
- Historical VaR/ES at 99% needs 100 observations (95% still needs 20); below that `var99`/`es99` are null and `tail99_reason` says why.
- A tagged order that reduces an untagged book over its scope cap is no longer refused; the scope is valued over the same positions with and without the order.
- Scenario grids for grid-loss caps and alerts are remembered by their exact inputs, so order checks stop repricing every open order's grid.

### Trading, financing and halts

- Opt-in seeded expiry pin risk models contrary exercise near the strike and warns before expiry; existing plans remain unchanged.
- Practice cash transfers, rollover credit/debit interest and stock-borrow fees are journaled and visible in Dashboard. Financing counts in equity and daily loss without adjusting its opening base.
- Single-name scenario/replay halts pause one underlying; the replay API and Dashboard support halt drills with recorded, verifiable commands.
- What-if accepts OTO/OCO chains and evaluates their current-quote fills and contingent orders on a private account copy.
- Financing rates and pin risk are program rules: changing them drops the preset's `plan_id`, and financing overrides on evaluation, verification and funded plans are refused with `PLAN_LOCKED`.
- What-if applies submission's opening gates (volume caps and other opening rules) to OTO/OCO parents and every child, as submit does.
- Shares halted through the close roll over on their last pre-halt print, reported as `halted`; prints inside a simulated halt are no longer taken as marks.

### API, Python and automation

- Versioned account WebSocket events with bounded delivery and typed Python iteration.
  The stream dispatches only to the addressed account's subscribers and sends `floor`
  only on entering or leaving the warning band.
- Outbound notification channels keep their previous default events; `order_state`
  is opt-in.
- `openport.bot` strategy hooks for live paper, replay and saved history, with REST fallback.
- Inbound playbook signals: webhook senders use a per-playbook, per-account secret
  (issued, rotated and revoked by an admin, stored only as SHA-256) as an HMAC
  signature with a ±300 s timestamp window, or in the body over HTTPS for senders
  such as TradingView that cannot set headers; bearer clients use
  `POST /api/playbooks/{id}/evaluate`. Nonces and rate limits are per secret, and
  saved manual/automatic modes remain authoritative.
- `DELETE /api/backtests/sweeps/{id}?purge=true` removes a saved sweep, as for single runs.
- Bounded backtest parameter sweeps with exact variant reports and historical ranking.
- Optional Arrow/Parquet exports preserving money text, with an offline example notebook.
- Python sdist/wheel release workflow using tag-triggered PyPI trusted publishing (OIDC),
  as the `openport-client` distribution (import name `openport`; the PyPI name
  `openport` is an unrelated project). Only tags on `main` publish, and the publish
  action is pinned to a commit SHA.
- Python client coverage for every documented HTTP operation, with a contract parity guard.
- MCP tools for playbooks, staged orders, backtests, alerts, journals, what-if,
  market views and order changes, retaining account scopes and preview guidance.

### Install, access and support

- **Open the terminal without hunting for a token.** `openportd --open` launches
  the browser after startup; `--print-url [--open]` reads existing token settings
  and exits. Homebrew caveats and installation examples use the helper.
  `--no-token-link` prints the `--print-url` command instead of the token link at
  startup; the systemd unit uses it so the token stays out of journald. `--open`
  documents, and on Linux warns, that the launcher's arguments are visible in `ps`.
  `--print-url --address localhost` names the host as given instead of failing.
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
  keeps its browser-demo button hidden until a real URL is configured. Nothing
  is hosted or published by this change.
- **Share diagnostics deliberately.** Status copies version/build/platform,
  provider/feed and recent error categories into a reviewable bug report. Local
  fatal signal logs contain no memory dump; there is no telemetry. `/api/status`
  adds optional build metadata for clients.

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
