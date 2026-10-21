# American rates and de-Americanisation (A3)

American call/put parity does not identify a discount rate: the strike-dependent
put early-exercise premium changes its slope. European expiries retain the
existing robust parity fit. Expiries shorter than 30 days borrow the median of
that underlying's fitted longer rates; absent those, they use `fallback_rate`.

After European analytics, the engine constructs zero-rate curves from the
expiries with their own successful rate fit. At least two distinct positive
tenors are required. Zero rates interpolate linearly in time and extrapolate
flat. The latest eligible curve supplies American discounts; SPX takes priority.
All European books are processed before American books in each pass. Changing
the selected curve also refreshes American analytics with unchanged quotes.
Replacing the engine's cash-dividend schedule does the same, including when
payments are revised or removed. The engine converts each symbol's trading
dividends to plain dates and dollar amounts; analytics has no trading dependency.
Mixed exercise-style books use only European slices to construct curves.

An American expiry never fits a parity slope, including with de-Americanisation
disabled. It takes its rate from the supplied curve, otherwise the flat assumed
rate (`openportd --rate R`, default `0.04`, permitted range `[-0.05, 0.25]`).

## Correction

1. With the fixed discount, estimate F0 from the weighted median of
   `K + (C-P)/D`, using the existing spread-weight caps and MAD rejection.
2. Solve first-pass Black-76 mid IVs. Set `r = -log(D)/T` and
   `q = r - log(F0/S)/T`, using the common reference spot. Below
   `min_days_for_rate` (30 days), use the median implied q from this underlying's
   longer live expiries, falling back to own q if no longer tenor is available.
   With no known cash before expiry, clamp tree q to **[-5%, 20%]**: a modelling
   safeguard covering negative carry and high dividend/borrow yields while
   limiting amplified timing noise.
   With known cash, use the escrowed spot and residual carry described below
   instead of borrowing or clamping q.
3. Evaluate American minus European prices on the **same 31-step Leisen-Reimer
   lattice**, at the same volatility and carry. American value uses rollback;
   European value uses the binomial terminal probabilities and the same terminal
   payoffs. Sum weights outward from their mode and normalize, avoiding tail
   underflow and the cost of a second rollback. No analytic BSM price is
   subtracted from a lattice price.
4. Refit F from premium-adjusted near-ATM parity with the existing robust
   fixed-discount estimator. Solve final Black-76 mid/bid/ask IVs from adjusted
   prices, while keeping **displayed bid/ask/mid as the raw market quotes** and
   preserving receipt flags. The API exposes `eep` separately: null when not
   computed, zero when the computed premium is zero. Positive EEP is explained
   in the web IV-cell tooltip.

The implementation corrects all two-sided options with a usable first-pass IV,
including both parity legs, all OTM smile inputs, and ITM fallback smile inputs.
If an ITM quote cannot imply its own first-pass IV, its partner's valid first-pass
IV supplies the premium estimate. It skips a tree exactly when exercise cannot
help when no cash payments are present: calls with q <= 0 and r >= 0, or puts with
r <= 0 and q >= 0. A cash payment can make early call exercise valuable even at
q = 0, so cash schedules do not take that shortcut. Premiums are
reused for parity and all three quote prices within the pass; there is no cache
across changing market inputs. A negative premium-adjusted bid has no bid IV,
but does not prevent a positive adjusted mid from having an IV when the original
quote was two-sided. A received zero bid retains the no-mid-IV behavior.

An underlying print more than 30 minutes older than the option data
(`max_spot_age_minutes`, above the 16:00/16:15 ETF close gap) is treated
as absent, regardless of provider: parity supplies the reference spot and
`spot_source` becomes `parity`. This prevents stale closing index levels from
anchoring live overnight Greeks. Session clocks are documented in
[runtime.md](runtime.md#product-sessions-and-cboe-clocks).

For a one-day synthetic American chain with a 0.05% spot mismatch, longer-tenor
carry reduces the maximum OTM IV error from 0.070276 to 0.002375 vol points for
an upward spot mismatch, and from 0.035256 to 0.004235 for a downward mismatch.
The test uses 1,001-step quotes at S=100, r=4.5%, q=1%, vol=20%, strikes 97..103
in increments of 0.25 and a one-year companion expiry.

One correction iteration suffices for the measured acceptance grid below. The
numerical tolerances below are measured on that grid, not guaranteed
for every volatility, dividend or maturity. Greeks remain European Black-76
Greeks at the final smile IV, accurate on the OTM side of American contracts.
Their existing spot scaling, `dF/dS = F/S`, is retained; these are not sensitivities
of the cash-dividend exercise model.

Exposure counts every side with received usable OI at a finite-smile-IV strike,
even when that side's own IV failed. `priced` still counts own-IV successes;
`oi_coverage` is receipt coverage over the exposure set. The web coverage warning
uses missing quotes or OI, not missing IVs.

## Known cash dividends

`openportd --dividends FILE` and `--dividends massive` supply the same known
payments to American analytics as to paper trading. No amounts or future
quarterly payments are forecast. For each expiry, include only positive finite
payments on valid ex-dates strictly after the data's market time and strictly
before settlement. A date takes effect at **00:00 New York time**, including
daylight saving time, so quotes on the ex-date exclude that payment. A payment
on the expiry date is included when valuation precedes that day's midnight.
This is a date-only convention, not an observed intraday ex-event timestamp.

Both CRR and Leisen-Reimer use the escrowed-dividend model. For cash amounts
`A_i` at year fractions `t_i`, define

```
PV = sum(A_i * exp(-r * t_i))
X0 = S - PV
q_residual = r - log(F0 / X0) / T
```

The recombining tree runs on `X0` with growth `r - q_residual`. At time `t`,
exercise uses `X_node + sum(A_i * exp(-r * (t_i - t)))` for payments still to
come. At an ex-time exactly on the lattice, exercise includes the payment just
before the jump; an off-grid ex-time is sampled at the last preceding node.
The terminal payoff has no remaining cash, so a European tree converges to
Black-Scholes on `X0` with the residual yield. American exercise can capture
the cash by exercising a call before it goes ex. Cash reserves are computed
once per level, outside the node rollback.

Residual q explains carry that the supplied payments do not explain, including
borrow and unknown distributions. It uses this expiry's first-pass parity
forward even below 30 days, without the continuous-only path's term borrowing
or clamp: `X0 * exp((r - q_residual) * T) = F0`. Applying the old full yield to
`X0` would count the known cash twice. The final forward is still refitted after
one EEP correction; the tree is not iterated to the final forward. Timing noise
can therefore affect short-expiry residual carry, and a coarse tree can miss
part of the value just before an off-grid dividend. More steps improve that
boundary approximation; they do not remove the escrowed model's assumption
that volatility acts on `X`, rather than the full spot.

Zero-volatility and numerical-fallback American valuations check exercise on
both sides of each payment and at interior stationary points. The positive-vol
fallback remains an approximation. The pricing API rejects nonfinite times,
negative or nonfinite amounts, and nonpositive escrowed spots. Analytics ignores
malformed schedule entries; if the selected cash consumes the whole spot, it
leaves that expiry uncorrected with no payments reported. With no eligible cash,
the existing continuous-yield prices, safeguards and analytics are unchanged.

Summary `expiries` and the chain response's `expiry` include `dividends`, sorted
by ex-date, with `ex_date` (`YYYY-MM-DD`) and `amount` (dollars per share without
display rounding). The array is empty when no cash was used, including European
expiries and disabled or unavailable EEP corrections. These are model inputs,
not a claim that all future dividends are known.

Regression tests cover both trees' convergence, European escrowed Black-Scholes
prices, immediate call exercise ahead of a large payment, deterministic exercise
on either side of a jump, and exact preservation of the no-cash path. Synthetic
American chains with cash payments and residual continuous carry also exercise
the full forward/IV correction and reporting path.

## Accuracy measured on this Mac

S=100, r=4.5%, q=1%, vol=20%, T in {0.25, 1.0}, K=80..120 in increments of 2.5,
both calls and puts. Compare EEP with the same-tree difference at 2,001 steps:

| LR steps | Max absolute EEP error, all sides | Max error, OTM sides |
| ---: | ---: | ---: |
| 15 | $0.07360427 | $0.02229692 |
| 21 | $0.06245326 | $0.01643721 |
| **31** | **$0.03776984** | **$0.01137375** |
| 51 | $0.02133947 | $0.00589251 |
| 71 | $0.01481348 | $0.00410468 |
| 101 | $0.01050770 | $0.00275754 |

For the end-to-end check, generate American quotes with the existing 1,001-step
LR pricer and small symmetric spreads, then supply a flat 4.5% SPX curve:

| T | D fixed, no correction: F error | Corrected F error | Max corrected OTM IV error |
| ---: | ---: | ---: | ---: |
| 0.25 | -0.050555% | +0.001102% | 0.016711 vol points |
| 1.00 | -0.419232% | -0.027016% | 0.086979 vol points |

Both forward residuals are inside 0.05% and both IV residuals inside 0.1 vol
points. The regression also verifies that raw American parity misestimates the
rate by more than 50 bp. The paired-pricer test independently checks the terminal
expectation against a full European rollback on the same 31-step tree.

## Timing

Release build on this Mac, 2026-09-22. Deterministic SPY-sized chain: 12,028
options, 31 expiries at 1+i² days (i=0..30), S=600, 194 strikes per expiry from
120 to 1085 in $5 increments. Quotes are generated once with 101-step American
LR at r=4.5%, q=1%, vol=20%. Timing includes the complete `analyze()` call,
including forward fits, bid/mid/ask IVs, Greeks, exposures and gamma flip. It
excludes fixture construction. Five warm-up passes precede 50 measured passes.
The new paths use a flat 4.5% SPX curve.

| Version | Median | p90 | Own-IV successes |
| --- | ---: | ---: | ---: |
| Before A3, original binary and defaults | 19.704 ms | 24.234 ms | 9,565 |
| A3, curve supplied, correction disabled | 22.333 ms | 28.825 ms | 11,850 |
| A3, curve supplied, 31-step correction | 36.292 ms | 43.007 ms | 10,811 |

The final median is **1.84×** the original pass, within the approximately 2×
budget. Correct discounts change which IV solves succeed, so the baseline and
new passes do not have identical successful-solve counts. A preliminary
51-step implementation with two rollbacks took 50.823 ms (assumed-rate path);
31 steps met the acceptance tolerances, and the terminal expectation removed
unnecessary European rollback work. No ITM-side omission or approximate EEP
interpolation was needed.

Reproduce the new timing and accuracy probe after building the core (the probe
is also the `openport_measure_american` CMake target, which adds `--models` and
`--model lr|bs2002|baw`; see [closed-form alternatives](#closed-form-alternatives)):

```sh
export DEVELOPER_DIR=/Library/Developer/CommandLineTools
cmake --build build -j12
c++ -std=c++20 -O3 -DNDEBUG -Iinclude bench/measure_american.cpp \
  build/src/libopenport_core.a -o /tmp/openport-american-measure
/tmp/openport-american-measure
/tmp/openport-american-measure --no-deamericanize
/tmp/openport-american-measure --accuracy
```

The original baseline executable was built before editing the analytics and
retained as `/tmp/openport-a3-before` for the paired timing run. The first baseline
measurement was 20.013 ms; the final paired run above was 19.704 ms. Both used the
same fixture and five-plus-fifty pass methodology.

Cash-dividend change, 2026-09-25, Release build on this Mac: the same 12,028-option
probe, with 31-step corrections and five warm-ups plus 50 measured passes. The
before executable was retained before editing. These runs were sequential with
the build and tests finished:

| Version | Median | p90 | Own-IV successes |
| --- | ---: | ---: | ---: |
| Before cash support, no cash | 29.832 ms | 30.809 ms | 10,999 |
| After cash support, no cash | 29.599 ms | 29.929 ms | 10,999 |
| After cash support, synthetic cash | 34.036 ms | 35.982 ms | 11,130 |

The cash fixture adds synthetic $1.50 payments at day 60 and every 90 days
thereafter, through day 870, to both quote generation and analytics. They are
timing inputs, not SPY's announced dividends. Residual yield in quote generation
is still 1%. Different quotes change the number of successful IV solves. The
no-cash path has no measured slowdown; this cash workload adds about 15% to it.
Run `/tmp/openport-american-measure --cash-dividends` with the build command
above to reproduce the cash case.

## Closed-form alternatives

`openportd --american-model bs2002` or `baw` replaces step 3's lattice EEP with a
closed form minus analytic BSM at the same inputs: Bjerksund and Stensland's
(2002) two-step flat exercise boundary (with the bivariate normal of Genz 2004),
or Barone-Adesi and Whaley's (1987) quadratic approximation, whose critical price
is solved by Newton's method from Haug's seed. Puts use the put-call
transformation. Both prices are floored at the BSM and intrinsic values. The tree
remains in use wherever the closed forms do not apply: an expiry with known cash
dividends before expiry, a nonpositive rate, zero vol or time, or a non-finite
result. Each expiry reports the model that actually removed its EEP as
`eep_model` (`leisen_reimer`, `bjerksund_stensland_2002` or `barone_adesi_whaley`;
null when not de-Americanised). Run records include the choice only when it is not
the default, so existing recordings keep their bytes and verify unchanged.

**Leisen-Reimer stays the default**: neither closed form is as accurate as the
31-step tree, and neither is meaningfully faster here, because the tree already
prices the European leg from its terminal probabilities instead of a second
rollback. Measured 2026-10-21 on this Mac (Release build, shared with other
builds, so absolute times are noisy; ratios are the useful part).

EEP against a converged 2001-step LR EEP over 5,400 contracts: S=100,
K=70..130 by 2.5, T ∈ {7d, 30d, .25, .5, 1, 2}, vol ∈ {10, 20, 40%},
r ∈ {1, 4.5%}, q ∈ {0, 1, 3%}, calls and puts (`openport_measure_american
--models`). "IV error" solves BSM IV from the converged American price minus each
model's EEP, as de-Americanisation does, and compares it to the generating vol.

| Model | Max EEP error | Max OTM EEP error | Max OTM IV error | OTM IV solves failed | Mean time per EEP |
| --- | ---: | ---: | ---: | ---: | ---: |
| Leisen-Reimer, 31 steps | $0.0921 | $0.0450 | 0.082 vp | 0 | 1.4 µs |
| Bjerksund-Stensland 2002 | $0.2604 | $0.0973 | 0.203 vp | 88 | 5.1 µs |
| Barone-Adesi-Whaley | $0.1530 | $0.1473 | 5.029 vp | 296 | 1.2 µs |

A failed OTM solve means the closed form's EEP exceeded the option's whole time
value, leaving a price below intrinsic. Google Benchmark
(`openport_bench --benchmark_filter=EarlyExercise`, median of three, CPU time over a
72-contract grid): LR 652 ns, BS2002 5,628 ns, BAW 848 ns per EEP. The
bivariate normals make BS2002 the slowest.

End to end on the regression chain above (1,001-step LR quotes, flat 4.5% curve,
`--accuracy`):

| Model | T | Corrected F error | Max corrected OTM IV error |
| --- | ---: | ---: | ---: |
| Leisen-Reimer | 0.25 | +0.001202% | 0.016712 vp |
| Leisen-Reimer | 1.00 | -0.026917% | 0.086981 vp |
| Bjerksund-Stensland 2002 | 0.25 | -0.020010% | 0.080009 vp |
| Bjerksund-Stensland 2002 | 1.00 | -0.084037% | 0.167337 vp |
| Barone-Adesi-Whaley | 0.25 | -0.001737% | 0.145610 vp |
| Barone-Adesi-Whaley | 1.00 | -0.011693% | 0.173486 vp |

Both closed forms exceed the 0.1 vol point IV acceptance tolerance at one year,
which the tree meets. On the 12,028-option SPY-sized timing chain (`--model lr|bs2002|baw`,
five warm-ups and 50 passes, median): LR 40.2 ms, BS2002 110.7 ms, BAW 46.8 ms,
with no correction 23.9 ms. These ran alongside other builds and are noisier than
the earlier table, but the ordering matched the per-EEP benchmark. The closed
forms are offered for comparison and for experiments where their known biases
are acceptable: BAW overprices long-dated options and BS2002, a lower bound,
underprices them (by over $1 per $100 at three years and 60% vol in the tests).

The approximations are validated in `tests/pricing/american_test.cpp`: the
bivariate normal against closed forms and numerical integration; both prices
against the 2001-step tree within 0.2% of strike up to a year and 40% vol;
BS2002 as a lower bound of the tree; European and intrinsic floors; exact zero
premium where exercise cannot pay; and every fallback to the lattice. Rebuild the
probes with `cmake --build build --target openport_measure_american openport_bench`.

References: Bjerksund and Stensland (2002), *Closed Form Valuation of American
Options*, NHH discussion paper; Barone-Adesi and Whaley (1987), *Efficient
Analytic Approximation of American Option Values*, Journal of Finance 42(2);
Haug (2007), *The Complete Guide to Option Pricing Formulas*, 2nd ed., 3.1.3–3.1.4;
Genz (2004), *Numerical computation of rectangular bivariate and trivariate normal
and t probabilities*, Statistics and Computing 14.
