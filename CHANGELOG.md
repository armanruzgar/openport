Notable changes in each release. Older releases: https://github.com/38st/openport/releases.

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
