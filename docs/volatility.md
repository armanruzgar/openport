# Volatility metrics

`GET /api/underlyings/{symbol}/volatility` computes current values from an immutable
analytics snapshot and the underlying's CandleStore. It returns market `as_of`,
`spot`, the front expiry's `forward`, `version`, and `mfiv`, `atm`, `skew`, `term`,
`realized`, `vrp`, `implied_moves` and a `sources` note for each part. Missing
numbers are null, with reasons where applicable. All reported volatility, RR, BF,
slope and curvature values are in **vol points**; variance is decimal annual
variance. A 20 vol-point value has variance 0.04. Percent moves are percentages,
not fractions. Ratios are dimensionless.

These calculations normally run on the requesting API worker. Enabled playbook
conditions also read them on the Desk thread, using the shared snapshot cache. They share the lazy SVI fits and per-snapshot lock used by the
surface route. Successes and missing results are cached by snapshot ownership,
so independent engines cannot share results even if symbol and version match.
Candle estimates and static `--events` labels are read for that snapshot's first
volatility request; plan and daemon calendars are read for each event estimate.
The next snapshot refreshes the current results. The history worker shares the
same fits and metric cache. Rank and ex-post results read current history on each
request, so a backfill is visible even before the next analytics snapshot. The
pure definitions live in `analytics/volatility.hpp`, `realized.hpp` and
`volatility_history.hpp`.

## Model-free implied volatility

For each expiry, let F and D be the engine's parity forward and discount factor,
and T its ACT/365 time from market time to the settlement instant. K₀ is the
largest listed strike at or below F. Select puts below K₀ and calls above it;
at K₀ use the average of the call and put bid/ask mids. Both K₀ quotes must be
usable. In each wing exclude nonpositive/missing bids, nonpositive/missing asks,
crossed quotes and invalid adjusted mids. Stop after two consecutive excluded
strikes; one usable strike resets that count. The no-ask exclusion follows
Cboe's February 2025 methodology change.

For American options, subtract each known nonnegative `eep` from its mid.
`eep` reports `removed`, `partial`, `unavailable` or `not_applicable`. Missing
EEP leaves the raw mid and marks the result `proxy`. No exercise correction is
invented. Negative adjusted mids are unusable.

On the retained strikes, ΔK is half the distance between the two neighbours;
at each end it is the distance to the sole neighbour. Compute

```
variance = (2 Σ ΔK × Q(K) / (D × K²) − (F/K₀ − 1)²) / T
vol = 100 √variance
```

A negative or undefined variance is null. Each expiry reports `variance`, `vol`,
`k0`, `low`, `high`, `strikes`, `lower_stop`, `upper_stop`, `truncated`, `proxy`,
`eep` and `reason`. Stops are `zero_bids` (two exclusions, including absent asks)
or `window` (the subscribed chain ended first). Either window stop sets
`truncated`, including when it is the provider's full chain. The retained value
is still shown, with † in the terminal.

For 9, 30, 93, 182 and 365 calendar days, bracket the target in minutes from
market time, allowing only expiries at least 1,440 minutes away. Interpolate
**total variance**, then divide by target time:

```
variance(N) = [N₁ variance₁ (N₂ − N) + N₂ variance₂ (N − N₁)] / [(N₂ − N₁) N]
```

This is the Cboe time interpolation formula with N_year cancelling. An exact
listed maturity can stand alone. Outside the listed maturities, or with an
unusable bracketing expiry, return null and a reason; do not skip an unusable
bracket in search of a more distant one. SPX AM and SPXW PM expiries already
coexist in the underlying snapshot and both enter the calculation. Other
underlyings use all standard expiries. Bracket ids and truncation/proxy flags
are returned with each constant maturity.

This is **model-free implied volatility**, not Cboe's VIX or its other indices.
Differences include our parity-derived F and D, our available strike window,
all subscribed settlement families and bracketing expiries, our quote validation,
and estimated EEP corrections for American contracts. We do not reproduce Cboe's
index-specific expiry eligibility windows, rate inputs or index dissemination
rules. A finite strike strip can omit material tail variance.

## ATM and skew

At k = ln(K/F) = 0, use raw SVI only when admissibility, butterfly grid checks
and the snapshot's calendar checks pass. Both members of a flagged calendar pair
fall back to the observed smile. Otherwise interpolate smile IV linearly in k.
Return `source: svi` or `smile_interpolation`. Never extend either smile beyond
its observed/calibrated range. ATM constant maturities are 7, 30, 60, 90 and
180 calendar days, interpolated in total variance with no extrapolation.

For each 25Δ and 10Δ point, solve using that strike's own smile IV:

```
d₁ = −k / (σ(k) √T) + σ(k) √T / 2
call delta = Φ(d₁); put delta = Φ(d₁) − 1
RR = call vol − put vol
BF = (call vol + put vol)/2 − ATM vol
```

The convention is **Black-76 forward, premium-unadjusted, undiscounted**. It is
not the chain's spot delta. Scan the observed range to bracket each root, then
bisect. Unsupported delta points are null. At 30 days interpolate the call,
put and ATM points' total variances separately before computing RR and BF.
The 30-day delta-point strikes are null: no synthetic forward/strike is invented.

SVI ATM slope and curvature are analytic. With w = σ²T, they are
`100 σ w′/(2w)` and `100 σ [w″/(2w) − w′²/(4w²)]`. Interpolated smiles have no
reported derivatives. Term measures are MFIV 9d/30d and 30d/93d, plus ATM 30d
minus 7d. Missing or zero denominators give null ratios. ≈ marks interpolated
smiles, raw American mids, ATM move proxies and OHLC estimator fallbacks.

## Realized volatility and cones

Use completed regular sessions only, up to 2,600 daily bars. No future bar or
unfinished current daily bar enters daily RV. `daily_as_of` gives the last completed
daily bar actually available, shown in the terminal so stale history is visible. Windows are 5, 10, 21, 63, 126 and
252 consecutive sessions; N returns require N+1 closes. Parkinson and Garman–Klass
need only N OHLC bars; Yang–Zhang also needs the preceding close. Missing sessions or
nonpositive/nonfinite closes give null. Calendar continuity follows the `md`
calendar; before 2022 it knows weekdays only.

For N sessions, with r = ln(C/C_previous), o = ln(O/C_previous),
c = ln(C/O), h = ln(H/O), l = ln(L/O), the daily variance estimates are:

- Close-to-close: sample variance of r, demeaned, denominator N−1.
- Parkinson: mean of ln(H/L)² / (4 ln 2).
- Garman–Klass: mean of 0.5 ln(H/L)² − (2 ln 2−1)c².
- Yang–Zhang: sample variance of o + k × sample variance of c +
  (1−k) × mean[h(h−c) + l(l−c)], where k = 0.34 / (1.34 + (N+1)/(N−1)).

Report 100 √(252 × daily variance). Each OHLC estimator validates every bar it
uses: prices must be finite and positive and high/low must bracket open/close.
An invalid OHLC window uses its close-to-close estimate and reports `fallback`
and `invalid_ohlc_used_close_to_close`. The history parser and store retain
close-only daily rows for this purpose; candle charts still omit invalid OHLC.
Daily bars built from observed minutes inherit the coverage of those minutes.
They are not proof that every trade or opening print was received.

Cones use every valid rolling close-to-close estimate in the available history,
including the current completed window. Quantiles linearly interpolate ordered
observations at (count−1)p. Return min, p10, p25, p50, p75, p90, max, distinct
`days_used`, rolling `observations`, current RV and its percentile. The current
percentile is 100 × (count below + half the ties)/count. The chart adds ATM IV
interpolated to the close of the Nth coming business session, using the calendar
rather than converting sessions with 365/252. It remains null outside the chain.

Intraday RV uses complete regular-hours five-minute blocks built from minutes.
All five minutes must be present and finished at market `as_of`. Use consecutive
block closes, or a block's open for the first block and after a gap. Never include
an overnight or gap-spanning return. Sum squared log returns without demeaning,
multiply by session minutes / observed minutes and by 252, then take the square
root and multiply by 100. An early close has 210 rather than 390 minutes. Return
today's value and up to ten retained sessions, with observed minutes and a
`partial` flag. Annualising sparse delayed prints can be unreliable; coverage
is shown alongside the estimate.

VRP here is an ex-ante **volatility spread**: 30d MFIV minus 21-session
close-to-close RV, plus their ratio. It is not a forecast error or a difference
of squared variances. Missing inputs give null; zero RV makes only the ratio
null. MFIV truncation and EEP proxy flags carry through.

## Implied session moves and events

Use per-expiry MFIV total variance, or ATM total variance when MFIV is missing.
Subtract consecutive expiries' totals, starting with zero at market time. Divide
an interval's variance equally among its business sessions; a full early-close
session also counts as one. A partly elapsed regular session gets its remaining
fraction of that day's scheduled regular hours. Weekends and holidays get none.
An AM settlement's overnight-only increment belongs to its settlement session;
AM-to-PM variance adds to that same day's move. This is an allocation convention,
not an inferred overnight/intraday volatility pattern.

Report up to 15 coming sessions, one standard deviation in percent and points of
the interval's forward, with `shared`, `proxy` and `truncated` flags. Where AM and
PM intervals contribute to one date, sum the assigned variance and use the last
interval's forward. Count **all** sessions in an interval, including those beyond
the displayed 15. Null intervals stay missing. The `intervals` array also reports every expiry interval, including those beyond
the first 15 sessions, its forward variance and any missing-business-time reason.
Negative forward variance beyond
1e-12 is a calendar-arbitrage flag and produces a null session move. Roundoff
within that tolerance is zero. Today's remaining move uses the latest same-day
settlement's total variance when present, with its own flags. It is null without
a same-day expiry. A shared interval cannot isolate an event's premium.

`openportd --events FILE` reads supplied labels for every underlying:

```
YYYY-MM-DD,Label
```

Blank lines, `#` comments and an optional `date,label` header are allowed. Dates
must be valid and representable in the market timestamp range. Labels are trimmed,
1–160 bytes, with no control characters; commas after the first belong to the
label. Duplicate dates are rejected with a line number. Labels are read at startup
and only shown on matching sessions. No event dates ship with OpenPort.

## Earnings implied versus realized

For each `earnings` event in the daemon's `--event-calendar` whose symbol is a
subscribed underlying, `openportd` records once a minute, from the live feed only:

- **Implied move**: the front at-the-money straddle's mid over spot, from the last
  analytics before the news. The last session before the news is the event day for
  `after_close` and the business day before for `before_open`; only its analytics,
  from its 09:30 ET open until the news (09:30 or the close of the event day), are
  used. The straddle is in the first expiry settling at or after the close of the
  reaction session (the event day for `before_open`, the next business day for
  `after_close`), at the strike nearest that expiry's forward with both sides
  quoted. Straddle over spot is the usual rule of thumb, not a model-free move.
- **Realized move**: the reaction session's close over the last close before the
  news, from the local daily candles (the vendor's closes, or sessions built from
  recorded minutes), once the reaction session has closed.
- **Priced**: `over` when the implied move exceeded the realized move's size,
  `under` otherwise. The summary counts both and gives the share that was over.

The history is kept in `earnings.json` beside the candle directory
(`~/.openport/earnings.json` by default); replay and the demo keep it in memory and
never record simulated markets. `POST /api/earnings/import` seeds past quarters:

```
symbol,date,session,implied_move,realized_move,label
AAPL,2026-07-30,after_close,4.1,-2.3,Q3 FY26
```

Moves are percents (a trailing `%` is allowed); the label is optional. Imported
rows are labelled `imported` everywhere and never replace a recorded one for the
same symbol and date; the import reports them as skipped. A malformed file adds
nothing. `DELETE /api/earnings/imported` removes them all.

## Comparing with published indices

On a machine with network access, run an unrestricted SPX chain during the session:

```bash
./build/apps/openport-probe cboe SPX --compare-mfiv --seconds 900
```

The comparison runs for the requested duration rather than stopping after the
first chain. It reads Cboe's chart endpoint through the existing history client:
`https://cdn-api.cboe.com/api/global/delayed_quotes/charts/intraday/_VIX.json`,
and `_VIX9D`, `_VIX3M`, `_VIX6M`, `_VIX1Y`. Daily files use `historical` instead
of `intraday`. Index names appear only as reference-series identifiers.

Cboe's intraday `datetime` is the New York minute at the bar's **close**. The
existing parser converts it to a start time; comparison restores the closing
minute and matches the snapshot's market minute exactly. Skip zero/invalid
bars and unmatched minutes; never substitute daily closes or nearest timestamps.
Keep the last sample per market minute so polling frequency cannot overweight it.
The summary gives matched samples, median |Δ|, p90 |Δ|, max |Δ| and median signed
Δ, where Δ is our value minus the index, in vol points. It also reports truncated
samples and 30d RR25/BF25 differences against OTM `vendor_iv` interpolation using
the same forward-delta convention. No vendor IV means no skew comparison sample.

A recording can be compared against saved intraday reference files without
network access:

```bash
./build/apps/openport-probe replay SPX --compare-mfiv --seconds 900 \
  --option file=/path/day.oprec --option speed=max --option loop=off \
  --comparison-dir /path/reference
```

That directory must contain `_VIX.json`, `_VIX9D.json`, `_VIX3M.json`,
`_VIX6M.json` and `_VIX1Y.json` in the intraday shape, covering the recording's
minutes. Without `--comparison-dir` the probe fetches the current chart files;
those will not match an older recording. Replay completion or provider failure
ends the run early. `nan` in probe statistics means no usable matched samples.

Tests use small synthetic JSON in the intraday and historical shapes, synthetic
option chains, exact minute-alignment cases and hand-computed statistics. No real
Cboe data files are committed. A live accuracy comparison has not been measured
for this feature. [How the numbers are made](methods.md) will carry measured
numbers from a live session; no accuracy claim is inferred from the synthetic tests.


## Local history

The live engine samples each underlying at most once per market minute on a
separate worker. Only live or delayed, current feed states qualify; stale,
connecting, stopped and error states do not. The first published snapshot sampled
in a minute wins. No missing minute is synthesized. Replays and simulated demo
engines have no series store, including engines started by ReplayHost.

Each row stores spot, the front unexpired forward, model-free IV at 9/30/93/182/365
calendar days, ATM IV at 7/30/60/90/180 days, 30-day 25Δ and 10Δ RR/BF, the 9/30 and
30/93 model-free ratios, total GEX, gamma flip and call/put walls. Volatility and
skew use vol points. Missing values are empty CSV cells and JSON nulls. Exposure
is missing when no usable open interest was received. These are the existing
finite-strip and smile estimates, with the same limitations as current metrics.
The CSV does not retain each estimate's truncation or interpolation diagnostics.

Daily rows are cached separately from minutes, indexed by session. Each uses
the last row from 09:30 through 16:00 ET, or through the scheduled early close
(usually 13:00 ET). A session becomes visible only once its close is at or before
the requested market time. Daily API results also exclude sessions unfinished at
the current snapshot, even when `to` requests a later date. Later and overnight
rows cannot displace that close.
The API reports both the session close `t` and the selected `sample_time`, so an
incomplete recording's last observation is visible. A daily row does not prove
full-session feed coverage. Reload and backfill rebuild the same daily view;
there is no second persisted copy of every close to reconcile. The minute cache
keeps the newest 45 days per underlying; older disk-backed minutes are read on
demand. Daily history is retained for all sessions. See
[storage and memory](runtime.md#volatility-series-storage).

`--series-dir DIR` selects storage, `--no-series` disables local collection, and
`--backfill-series FILE...` builds history offline and exits. Backfill applies
recorded events in order, with one analytics pass per `SnapshotComplete`, then
samples the first such snapshot of each minute. It uses the supplied `--rate`
when no parity curve is available. Recordings without snapshot markers produce
no rows. Existing minutes win unless `--force` is supplied. A forced change
appends a correction; an identical value appends nothing. Repeating the same
recording and settings is byte-identical. Demo recordings are refused before any
file in a multi-file invocation is processed. Backfill does not start a provider,
web server, trading account or network history fetch. See [storage](runtime.md#volatility-series-storage).

### Series API

`GET /api/underlyings/{symbol}/series?fields=mfiv30,atm30,rr25,rv21,proxy_iv30&interval=1d&from=2026-01-01&to=2026-09-22`

`interval` is `1m` or `1d` (default `1d`). `from` and `to` are inclusive Unix
seconds, ISO timestamps, or New York dates. A date's start is midnight; a `to`
date includes its full day. Defaults are one day for minutes and 366 days for
daily rows, ending at the current snapshot's market time, or server time before
any snapshot. Ranges over 7 days for minutes or 3,660 days for daily rows, reversed
ranges, malformed or duplicate parameters, unknown fields and results over
10,000 rows return 400. Unknown underlyings return 404.

Fields are `spot`, `forward`, `mfiv9`, `mfiv30`, `mfiv93`, `mfiv182`, `mfiv365`,
`atm7`, `atm30`, `atm60`, `atm90`, `atm180`, `rr25`, `bf25`, `rr10`, `bf10`,
`ratio9_30`, `ratio30_93`, `gex`, `gamma_flip`, `call_wall`, `put_wall`. Daily
queries also support `proxy_iv30`, `rv21` (trailing 21-session close RV),
`vix` and `vix_minus_mfiv` (SPX/SPY session comparisons).
Omitting fields returns all applicable fields. Rows are oldest first, each with
`t`, selected fields and a `sources` map per value (`own`, the named index,
`daily_closes`, or null). Missing sessions between daily observations have null
rows to break chart lines. Proxy IV never occupies a local MFIV or ATM column.

### IV rank and percentile

`/volatility` adds `iv_rank`, `iv_percentile`, `history_sessions`, `history_basis`,
`history_values` and `proxy`. The basis is the last 252 scheduled **completed**
sessions as of market time. A missing session reduces the count; it does not
extend the window. Local 30-day model-free IV is used, falling back to local ATM
30d. The current value follows the same fallback.

Rank is `(current − min) / (max − min)`. Equal min and max give null. Rank is not
clamped when current IV exceeds the historical range. Percentile is the fraction
of usable sessions **strictly below** current IV; ties are not counted. Both are
fractions in the API and percentages in the terminal. An empty window or missing
current IV gives null. The badge states “n of 252 sessions”, the current basis,
and the counts of local and proxy observations. `history_values` lists each
observation's date, value and source (`own_mfiv`, `own_atm`, or the proxy index).

Cboe daily closes optionally extend dates **before the first local daily row**:
VIX for SPX and SPY, VXN for QQQ, RVX for IWM, and VXD for DIA. These are proxies,
not OpenPort's model-free IV. Local rows win on overlap, even if a local value is
missing; gaps after collection starts stay missing. The API's `proxy` reports the
name, whether the rank used it, and its first date in the rank window. The
terminal draws proxy history separately. `--no-history` disables all Cboe history
fetches, including these proxies. They use the existing chart parser, hourly
fetcher and in-memory daily-bar cache, with no intraday index requests.

### Ex-post variance risk premium

SPX, and SPY explicitly by SPX proxy, add `ex_post_vrp`. Each completed starting
session uses SPX 30d model-free IV, or VIX before local history, then the SPX
close-to-close realized variance over the **following** 21 sessions. ATM is not
an ex-post fallback. All 22 closes must be present, consecutive and valid; the
last session must already be complete. The RV estimator is the same annualized,
demeaned sample variance used above. The existing pre-2022 calendar limitation
also applies. Unfinished outcomes and missing windows are excluded.

`variance = (IV² − RV²) / 10000` is decimal annual variance. `vol_points = IV − RV`
is a separate volatility spread, not the square root of that signed difference.
Each point reports its starting date, end date, IV, RV and implied source. The
chart shows variance terms and separates local and VIX-derived points.

Summaries cover starting sessions within 1, 3 and 10 calendar years of market
time. They report means and medians in both units, the share with positive
variance premium, observation and proxy counts, and actual first/last dates.
Partial histories remain labelled by those counts and dates. “Positive months”
means overlapping forward 21-session windows, one starting each session, not
independent calendar-month observations. SPY uses both SPX implied history and
SPX daily closes; it does not claim an ETF-specific realized premium. No series
is extrapolated to create a ten-year result.


## Terminal brief

Brief combines the selected underlying's summary, volatility, daily series and candles.
The header's symbol buttons select the underlying. Only that symbol is requested;
summary and volatility follow snapshot versions, while candle and close history update
at most once per market minute and poll for backfills. No aggregate route is needed.

The clock is the underlying's market timestamp. The engine's trading date identifies
the coming session overnight. Prior OHLC uses the previous-close date from the engine
when available; otherwise the latest available earlier daily bar is dated explicitly.
Daily bars may have incomplete observed coverage. The overnight range uses observed
minute bars from 20:15 ET on the preceding calendar evening through 09:30 ET, clipped
to market time. No candles means no range. Parity-inferred spot keeps its ≈ label.

Today's move uses the same-day expiry estimate. With no same-day expiry it uses that
session's share of the front interval. The remaining week's move includes today and
combines the supplied session percentage variances through Friday, then converts to
points at current spot. Holidays have no session row. An unusable session leaves the
week missing. Bands centre those point moves on current spot; they are model estimates.
Shared, proxy and truncated estimates stay marked. Events are only the supplied labels
on remaining sessions; the API does not retain today's label after its close.

IV and risk-reversal changes require the exact prior session's daily series row. No
older row or index proxy fills a gap. Sample time is shown because a daily row can be
the last observed minute rather than a closing quote. Replays and demo runs have no
local series store, so these changes normally remain unavailable there. Ratios above
one mark backwardation. Exposure keeps the open-interest convention caveat in
[how the numbers are made](methods.md).

The browser can remember a toggle to draw Brief levels on Trade's candle chart. It
uses the same level lines and edge markers as strikes and triggers. Coincident Brief
levels share one label; unavailable levels are omitted. The overlay fetches only while
the chart is visible and enabled, and does not fetch IV history.

## Playbook conditions

[Playbooks](playbooks.md) may require an IV-rank range, a minimum ex-ante VRP spread,
or an inverted 9d/30d model-free term ratio. They use the calculations and history
rules above. Missing values fail a required condition; no default IV or realized
history is supplied. Replays and scenarios use only their own available history.

### Skew and term ranks

`history_ranks` reports current value, rank, percentile, population z-score,
session count and a 252-session window for every stored field, including signed
RR25/BF25 and both MFIV term ratios. Only finite local completed-session values
enter these ranks; missing days do not extend the window and no index proxy is
used. Rank is `(current − min)/(max − min)` and is not clamped; percentile is the
fraction strictly below current. Z-score is `(current − mean)/population_sd`.
A flat or one-observation window has null rank and z-score; no history or missing
current gives null statistics. IV's proxy-aware rank also reports `iv_z_score`.

Daily series can request `vix` and `vix_minus_mfiv` for SPX/SPY. The difference
is VIX's daily close minus the same session's locally stored MFIV30, in vol
points. Unlike `proxy_iv30`, this reference remains available after local history
begins. Both observations must exist; there is no ATM substitution or gap filling.
`t` is the session close, `sample_time` is the actual local minute and
`vix_sample_time` is the VIX daily bar's start. These are session-aligned, not
simultaneous quotes. SPY is a cross-product comparison to the SPX-based VIX.
The UI only shows the tracking panel when VIX observations exist. Live measured
differences remain pending; synthetic tests establish arithmetic, not accuracy.

## Event variance extraction

`event_volatility` supplements the allocated session moves with a **derived**
event premium. It reads `--events` labels, earnings/news in `--event-calendar`,
and the requested account's plan calendar (`?account=...`, normally the selected
terminal account). Entries on the same date that resolve to the same session
count once, with their labels joined, so an earnings date imported into the plan or
listed by two sources is one event. Symbol-specific entries only apply to that
underlying. Dividends and splits are excluded: their mechanical
price adjustments are not event uncertainty. Plan changes are read on each
request even when the market snapshot has not advanced.

A date-only `--events` label assumes the event occurs at that session's open.
BMO earnings enter an opening settlement on the stated date. AMC earnings occur
after the stated date's closing settlement and belong to the next business
session, including across weekends/holidays. News uses its supplied exact time.
The response's `time` is null for date-only/BMO/AMC events; these timing conventions
do not fabricate a release or market timestamp.

Choose the **adjacent listed expiries** immediately before and after the event.
An event exactly at a settlement instant (news timed at the close) falls after that
expiry, matching the next-session assignment above; a BMO event still falls inside
the same morning's AM settlement.
Do not skip an unusable expiry. Both must still be unexpired at market `as_of`.
Let W₁ and W₂ be their total variances. N₁ is the business-session weight from
`as_of` to the earlier settlement; N₂ is N₁ plus the weight of the interval
between the two settlements. This uses the same fractional regular-hours and AM
overnight convention as session moves above: an AM overnight interval and a
subsequent full PM interval each count as one unit. Solve:

```
W₁ = b × N₁
W₂ = b × N₂ + E
b = W₁ / N₁
E = W₂ − b × N₂
implied event move (%) = 100 × sqrt(E)
implied event move (points) = later-expiry forward × sqrt(E)
```

`baseline_variance` is b per full session; `variance_premium` is E, in decimal
return variance (not annualized). The move describes the **extra event component**,
not ordinary session variance plus event variance. Use MFIV for both expiries;
if either is unavailable, use ATM for both and mark `proxy`. Do not mix their
levels in the solve. Strip truncation and EEP quality propagate.

This assumes constant ordinary variance per session and an additive one-off event
variance. It requires expiries on both sides, positive pre-event business time,
and exactly one known upcoming event through the later expiry. Otherwise return
null moves with `event_not_bracketed`, `no_baseline_sessions`,
`multiple_events_in_solve`, or `unusable_variance`. A negative premium remains
reported but has a null move (`negative_event_premium`); a decreasing total
variance flags `negative_forward_variance`. Roundoff within 1e-12 is zero.

News entries without a symbol, and every `--events` label, apply to **every**
underlying, so they count against each symbol's solve. A single stock usually lists
only weekly or monthly expiries; a macro release (CPI, payrolls, FOMC) almost always
falls between its earnings date and the later expiry, so its earnings estimate is
often `multiple_events_in_solve`. Index products with daily expiries are affected
far less. Give macro news a symbol, or keep separate calendars, when single-stock
earnings estimates matter more than macro labels on those symbols.
Unknown events, changing background volatility, coarse AM/PM allocation,
wide expiry spacing, risk premiums and illiquid/truncated strips can all distort
this estimate. It is not a uniquely identified event forecast or observed return.

## Cross-underlying scanner

`GET /api/volatility/scanner` returns one row per subscribed symbol, including
subscriptions without a snapshot. It takes no query parameters. The terminal's
Trade → Vol scanner tab sorts every metric (missing values always last) and opens
a symbol's Volatility view on click. Each row reports its own `as_of`, feed state,
history count, proxy provenance and `realized_as_of` (last available completed RV
bar date); the batch does not imply synchronized quotes or current daily history.
The response identifies its provider and whether the data is simulated.

Columns are IV30 (MFIV, otherwise explicitly labelled ATM), IV rank/percentile,
IV30 minus trailing 21 completed-session close RV, RR25 and its local-history rank,
MFIV 9d/30d term ratio, and today's implied move in percent and points. The IV/RV
spread uses the same IV basis as the displayed IV30. The individual volatility
endpoint's existing VRP remains MFIV-only. IV history can use the named index
proxy before local observations; RR history never does. Today's move requires an
unexpired same-day settlement; absence is null, not zero. Values retain the same
finite-strip, smile, coverage and staleness limits as the individual endpoints.

Scanner rows are cached per symbol publication: polling tabs reuse a row until that
symbol publishes a new snapshot, while feed state is read on every request. History
stored meanwhile enters the ranks with the next publication.

The Python client and MCP server expose `volatility_history`, `surface_history` and
`volatility_scanner`. Both default `surface_history` to the comparisons only. Python's
`series(symbol)` continues to mean listed expiry series for compatibility. Surface history storage and comparison details are in [SVI](svi.md#historical-surface-snapshots).
