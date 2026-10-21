# SVI and SSVI volatility surfaces

OpenPort fits raw SVI independently to each expiry's out-of-the-money smile:

\[
k=\log(K/F),\qquad w(k)=a+b\{\rho(k-m)+\sqrt{(k-m)^2+\sigma^2}\},
\qquad IV(k)=\sqrt{w(k)/T}.
\]

Here **w is total variance**, T is years, and IV is decimal annual volatility.
These conventions follow Gatheral's raw SVI formulation; they matter when applying
wing bounds. The fit is descriptive: it does not repair arbitrage or interpolate
between expiries.

## Calibration

Use the chain's existing smile IV and the corresponding OTM side (call for K≥F,
put otherwise). Require finite positive bid, finite ask≥bid, and positive finite
bid IV≤smile IV≤ask IV. Missing, crossed, zero-bid and invalid IV markets do not
enter calibration. At least five distinct log-strikes and a range ≥1e-5 are needed.
Fitting uses the entire eligible expiry, independently of the API display window;
the existing market-display filter still hides price spreads above half the mid.

For IV spread s, initial weight is 1/max(s, 0.0001)². Cap it at four times the upper median
weight, then lower the common cap until no observation has more than 25% of the
normalized total weight. The spread floor is 0.01 vol points. Minimize weighted
**total-variance** squared residuals against IV²T. Report
100√Σ weight·(fitted IV−market IV)² as RMSE in vol points; it is a diagnostic in
vol units, not the minimized objective.

Following Zeliade's dimension reduction, fix (m,σ), put y=(k−m)/σ, c=bσ and d=ρc.
Then w=a+dy+c√(1+y²), a three-variable constrained least-squares problem. A primal
active-set quadratic solver handles the linear bounds. The curved minimum-variance
constraint is enforced by adding supporting planes at its analytic minimizer;
this allows negative a, unlike restricting a≥0. Only residuals ≤1e-12 in total
variance are rounded onto an active boundary. The inner solve has finite iteration
limits and rejects nonconvergence.

The outer search evaluates 20 fixed seeds: m at the range midpoint plus
{−0.5,−0.25,0,0.25,0.5} times the range, and σ at {0.05,0.15,0.4,1} times the range.
Refine the best four using two-dimensional Nelder–Mead in (m,log σ), at most 400
iterations each. Search bounds are midpoint±2 ranges for m and [1e-4,5] ranges for
σ. Convergence requires simplex widths below 1e-8 ranges in m and 1e-8 in log σ.
This is a deterministic bounded local search, not a global optimality guarantee.
Flat smiles can have non-identifiable parameters. Failure returns no parameters,
an explicit reason, eligible point count and measured time.

## Constraints and diagnostics

Enforce b≥0, |ρ|≤1−1e-7, σ>0 and a+bσ√(1−ρ²)≥0. The wing limit is
b(1+|ρ|)≤2, with numerical admissibility tolerance 1e-12, at every tenor.
Lee's moment formula bounds the slope of **total variance**, independent of T.
The previous 4/T cap was incorrect and has been removed, including for T>2.
See [Lee's moment formula](https://math.uchicago.edu/~rl/moment.pdf).

With x=k−m and h=√(x²+σ²), derivatives are w′=b(ρ+x/h), w″=bσ²/h³. Evaluate

\[
g(k)=\left(1-\frac{kw'}{2w}\right)^2-
\frac{(w')^2}{4}\left(\frac1w+\frac14\right)+\frac{w''}{2}
\]

on 2,001 uniformly spaced k values, including both calibration endpoints. Report
the minimum and its k. Any negative g fails; undefined density, including zero w,
also fails. For calendars, sort successful fits by T and compare consecutive pairs
across all settlement families on 2,001 points over the intersection of their
calibration ranges; skip disjoint ranges. At each k, the later expiry's IV increase
needed to remove a crossing is `100 * (sqrt(w_earlier / T_later) - sqrt(w_later / T_later))`
vol points. At each k, linearly interpolate each expiry's eligible two-sided
quoted IV half-width `(ask_iv - bid_iv)/2`, convert to vol points, and use the wider
of the two. The tolerance is `max(0.1, RMSE_earlier, RMSE_later,
100 * half_width_earlier(k), 100 * half_width_later(k))` vol points. Duplicate
quote k values use the widest half-spread. The full observed half-width is used,
without the weighting floor/cap. Fits with no quote metadata use the floor/RMSEs.
Flag only increases strictly above that local tolerance (plus 1e-12 roundoff).
Report the largest **flagged** increase, its k and the tolerance at that k;
a larger crossing inside a wider spread does not hide a smaller significant one.
No judgement is made about raw SVI extrapolation beyond quotes.

These are **finite-grid diagnostics**, not a global no-arbitrage certificate:
violations between grid points or outside the checked range can be missed, and
wing conditions alone do not ensure a valid density. Fits remain unchanged when a
check fails. Raw-SVI diagnostics say “Grid checks pass”, never “arbitrage-free”.

## SSVI: one arbitrage-free surface

For the entire underlying, fit a shared (ρ,η,γ) and an ATM total variance θ_t
per usable expiry:

\[
w(k,\theta_t)=\frac{\theta_t}{2}\left[1+\rho\varphi(\theta_t)k+
\sqrt{(\varphi(\theta_t)k+\rho)^2+1-\rho^2}\right],\qquad
\varphi(\theta)=\frac{\eta}{\theta^\gamma(1+\theta)^{1-\gamma}}.
\]

IV is √(w/T). This is Gatheral–Jacquier (2014), Eq. (4.1) with the modified
power law of Eq. (4.5), Remark 4.4. The implementation enforces θ_t>0,
nondecreasing θ_t in T, |ρ|<1, η>0, 0<γ≤1/2 and η(1+|ρ|)≤2.
The paper's **Theorem 4.1** requires nondecreasing θ and

\[
0\leq\frac{\partial_\theta(\theta\varphi)}{\varphi}
\leq\frac{1+\sqrt{1-\rho^2}}{\rho^2}
\quad\text{(upper bound infinite at ρ=0)}.
\]

For this φ the middle quantity equals (1−γ)/(1+θ), so the condition holds.
**Theorem 4.2** requires θφ(1+|ρ|)<4 and θφ²(1+|ρ|)≤4 for butterflies.
Writing x=θ/(1+θ), θφ=ηx^(1−γ)<η and
θφ²=η²x^(1−2γ)(1−x)≤η². Our η bound implies both inequalities, including
strictness of the first. Thus **Corollary 4.1** supplies a static-arbitrage
certificate, rather than relying on the finite diagnostic grid.
See the [paper's Section 4](https://arxiv.org/html/1204.0646#S4).

The initial condition is θ_0=0 and θφ(θ)→0 as θ→0. If interpolating in time,
use a continuous nondecreasing θ curve (piecewise linear through (0,0) and the
calibrated knots is sufficient), and preserve monotonicity when extending it.
The API currently returns the fitted expiry knots rather than time interpolation.
The calendar interpretation uses proportional dividends and forward log-moneyness,
as in the paper's Lemma 2.1. No guarantee is asserted for market quotes themselves.

Calibration has two stages:

1. Reuse SVI's eligible OTM quotes. Interpolate **IV**, linearly in k at zero,
   then set θ=ATM IV²T. An exact ATM quote takes precedence; duplicate k values
   average their IVs. Require five distinct points spanning at least 1e-5 in k
   and quotes bracketing zero; skip unusable expiries with explicit reasons.
   Require at least two usable distinct tenors for a surface fit. Sort by T and
   run equal-expiry-weight pool-adjacent-violators regression on θ. Equal tenors
   are pooled first, ensuring identical θ there. `monotone_adjusted` reports
   whether the repair changed any θ beyond relative roundoff (1e-14).
2. Hold repaired θ fixed and minimize the weighted total-variance squared error
   across every usable expiry. Quote weights are **exactly** the same normalized
   per-expiry weights as SVI, so expiries have equal aggregate weight. Optimize
   (ρ,q,γ), where q=η(1+|ρ|)/2. Bounds are ρ∈[−1+1e-7,1−1e-7],
   q∈[1e-8,1], γ∈[1e-6,1/2]; small positive numerical floors exclude degenerate
   parameters. Evaluate 45 fixed seeds: ρ∈{−.8,−.4,0,.4,.8}, q∈{.15,.4,.8},
   γ∈{.15,.35,.5}. Refine the best four with bounded three-dimensional
   Nelder–Mead, at most 600 iterations each, until all simplex coordinate widths
   are <1e-9. This deterministic local search has no random restarts and no
   global-optimum claim. An unconverged fit returns a failure reason and null
   parameters. Per-expiry RMSE uses the same IV-unit diagnostic as SVI; overall
   RMSE is the square root of the mean per-expiry squared RMSE. Time includes
   quote preparation, theta repair, optimization and RMSE calculation.

Raw SVI uses five independent parameters per expiry and usually fits each smile
more tightly. Its ρ alone does not identify observed skew (m and σ also affect it),
and neither wing caps nor grid checks remove static arbitrage. SSVI ties the
expiries together, with ATM skew having the sign of its common ρ; it may have
larger residuals but supplies globally safe strike extrapolation. Sparse or flat
surfaces can still have weakly identified parameters. Tests convert SSVI to its
exact raw-SVI representation for the existing density and calendar checks, and
also test total-variance ordering directly without calendar tolerance.

## API and display

`/api/underlyings/{symbol}/surface` retains existing fields. Each expiry adds
`svi` with `a,b,rho,m,sigma,rmse_vol_points,points,status,reason,fit_ms,`
`butterfly_min_g,butterfly_k,butterfly_ok`; `points[].svi_iv` is decimal fitted IV.
A failed fit has `svi: null` and null point estimates. Expiry-level `svi_status`,
`svi_reason`, `svi_points`, `svi_fit_ms` preserve failure information.
`svi_years,svi_min_k,svi_max_k` support sampling without rounded-tenor errors.
Parameters retain full precision. Top-level `calendar_violations` contains
`{earlier,later,k,vol_points,tolerance_vol_points}` using existing expiry IDs;
it covers only returned expiries. `vol_points` is the full later-expiry IV increase, not the excess over
tolerance. `tolerance_vol_points` is the local threshold used. The table shows
the size, k and counterpart expiry for both members.

Top-level `ssvi` always contains
`{rho,eta,gamma,rmse_vol_points,status,reason,monotone_adjusted,fit_ms}`.
Parameters/RMSE are null on failure; status and reason remain available.
Each expiry adds `ssvi_theta`, `ssvi_rmse_vol_points`, `ssvi_reason`,
`ssvi_min_k`, `ssvi_max_k`, and each market point adds decimal `ssvi_iv`.
Skipped expiry theta/RMSE/IV are null with an expiry reason; if the entire
surface fails, all estimates are null and the top-level reason explains why.
Theta and shared parameters retain full precision for client curve sampling.

Fitting runs only in the surface and volatility APIs. A per-snapshot mutex coalesces concurrent
requests; the engine never takes it. Cache successes and failures for each
immutable (symbol, metrics version) snapshot, using weak snapshot ownership to
isolate different sources and remove expired entries on subsequent requests.
Larger expiry prefixes fit only the additional raw-SVI expiries. SSVI fits all
usable expiries in the underlying snapshot on its first request, independently
of the requested prefix; both successes and failures are cached. Changing the
prefix/window reuses the same shared SSVI fit. Calendar comparisons and JSON
serialization still run.

The Volatility view offers Market / SVI / SSVI / All, draws market dots and 241
samples uniform in k (also on the strike axis). Solid raw-SVI curves are clipped
to the requested window and calibration range; dashed SSVI curves cover the
requested window, including beyond quotes. The table shows shared SSVI parameters,
overall RMSE, fit time, any monotone adjustment, per-expiry θ and SSVI RMSE next to
SVI parameters/RMSE/counts and butterfly/calendar or failure labels. The explanation
reads “one surface, arbitrage-free by construction; per-expiry SVI fits tighter but
can admit arbitrage”. Colors use existing light/dark theme tokens.

## Validation and timing

Tests cover exact parameter recovery including negative a; bounded recovery under
small deterministic noise; short and long tenors; active constraints and flat
smiles; invalid, missing and degenerate inputs; capped quote weighting; determinism;
flat and known safe densities; Vogt's published butterfly counterexample; calendar
ordering; API fields, nulls, concurrent cache reuse, windows, prefixes and version
invalidation; and web sampling, toggles and diagnostic rendering. SSVI tests cover
exact and noisy recovery (positive/negative/zero ρ, including γ=1/2), constrained endpoints,
wide outlier weighting, ATM interpolation, missing ATM, invalid inputs, equal
maturity pooling and PAVA repair, determinism, grids to |k|=100 and θ from 1e-8
to 100, API fields/nulls, concurrent cold fits and cache invalidation.

Run the repeatable synthetic workload with `./build/bench/openport_measure_svi`.
It uses 250 OTM points per expiry, deterministic small IV noise, varying quote
spreads, 60 tenors from 2 days to about 2 years, five warmups and 25 timed samples.
The SSVI timing is one shared fit over all 15,000 points, including quote preparation and theta repair.
It is SPX-sized, **not a live SPX calibration accuracy measurement**. Local Release
build timings (ms):

| Workload | Median | p90 |
| --- | ---: | ---: |
| One SSVI surface fit, 60 × 250 points | 40.404 | 40.660 |
| One raw-SVI fit, 250 points, including butterfly check | 2.651 | 2.716 |
| 60 raw-SVI fits plus butterfly and calendar checks | 155.363 | 157.282 |
| Full API, cold SVI + SSVI and 15,000 serialized market points | 248.623 | 253.399 |
| Full API, cached fits, same points and calendar checks | 55.621 | 60.360 |

No time is added to the engine analytics pass. The first surface response pays
calibration cost on its API worker; repeated responses still pay serialization cost.

## References

- Gatheral (2004), *A parsimonious arbitrage-free implied volatility parameterization
  with application to the valuation of volatility derivatives*, Global Derivatives,
  Madrid; [bibliographic entry in Gatheral & Jacquier](https://arxiv.org/html/1204.0646#bib.bib13).
- Zeliade Systems (2009; revised 2012),
  [*Quasi-Explicit Calibration of Gatheral's SVI model*](https://www.zeliade.com/wp-content/uploads/whitepapers/zwp-0005-SVICalibration.pdf).
  OpenPort uses the dimension reduction with weighted observations and the full
  nonnegative-minimum constraint, rather than the paper's simpler nonnegative-a box.
- Gatheral & Jacquier (2014),
  [*Arbitrage-free SVI volatility surfaces*](https://arxiv.org/abs/1204.0646),
  Quantitative Finance 14(1), 59–71. Density diagnostic: Lemma 2.2; Axel Vogt example:
  a=−0.0410, b=0.1331, ρ=0.3060, m=0.3586, σ=0.4153.

## Historical surface snapshots

The live series worker retains raw SVI parameters per listed expiry at 10:00,
12:00 and 14:00 New York time (the first sampled snapshot in the following five
minutes), and the latest sampled snapshot in the session's final five minutes
through the scheduled close. Like the minute series, collection samples the first
publication observed in each market minute. Early closes use their actual close; there is no
14:00 capture after a 13:00 close. Missing windows stay missing. Replays and demos
do not collect history. Offline non-demo recordings can populate these snapshots
with `--backfill-series`.

`GET /api/underlyings/{symbol}/surface-history` returns `snapshots` and three
`comparisons`: `prior_close`, `week`, and `month`. `from`/`to` accept ISO timestamps
or New York dates (45 days by default, at most 366 days); `compare=true` omits the
range list. The range list holds at most `limit` (400) snapshots, the newest in the
range; `truncated: true` says older ones were left out, so narrow `from` to read them. One week means seven calendar days and one month means the preceding
calendar month, clamping the day at month end. Non-session targets roll back to
the previous business session. The exact target session must have a close sample;
missing history is null, never a substitute from a more distant recorded date.
Close samples are withheld until the session has completed at the current
publication's market time. Comparisons also respect an explicitly requested range.

Each snapshot carries `as_of` (the actual observation), session date, capture slot,
spot, model `svi`, and expiry ids including AM/PM and exercise-family suffixes.
Each expiry retains its own forward, full-precision tenor, calibration range,
parameters, fit status/reason, butterfly check and `calendar_ok`. No sampled quote
points are stored. The terminal selects one listed expiry and samples its curves
in `lib/svi.ts`, using each snapshot's own forward and remaining tenor, clipped to
its calibrated range. This compares the same contract over time, not constant
maturity; expiry ageing and forward changes are part of the comparison. Failed
fits remain absent; grid violations remain labelled diagnostics.

Snapshots are versioned JSON lines in `SYMBOL-YYYY-MM.surfaces` beside the minute
CSVs in `--series-dir`, one line per session slot. Intraday slots keep the first
sample; the close slot keeps the latest eligible sample. New slots are appended; a
replaced slot (the close re-sampled until the bell, or a `--force` correction) rewrites
that month file through a temporary file, so a session never stores more than one close. `--force` can correct the retained sample
at its actual timestamp. Repeated identical backfills do not append fitting-time
noise. Restart restores the same records and full parameter precision. Torn or
invalid files report a series error and refuse further appends, preserving the
readable prefix. Only month files within 13 months of a symbol's newest file are
loaded, and only snapshots within 400 days of its newest snapshot stay in memory;
older files stay on disk untouched, and older snapshots are not accepted. Disk use
still grows with recorded sessions and expiries. `--no-series`
disables this collection too. `enabled: false` and empty arrays identify sources
without a series store; they do not imply that an old surface was observed.
