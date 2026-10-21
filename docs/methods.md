# How the numbers are made

- **Time to expiry** runs from the data's own market time, not the wall clock, to the
  settlement instant: 09:30 ET for AM-settled, 16:00 ET for PM-settled, 13:00 ET on
  early-close days, from a holiday and early-close calendar. Each product knows its
  sessions, including Cboe's overnight session for SPX, XSP, VIX and RUT options, so a
  delayed snapshot taken after the close keeps the closing time while one taken
  overnight uses the overnight quotes' time. An underlying is analysed once its first
  price arrives, so a delayed or replayed feed is never valued at the wall clock.
- **Spot** is the provider's underlying price while it is current. When there is none
  (Databento) or it is more than 30 minutes behind the options (the SPX index is frozen
  overnight while its options trade), spot is inferred from put-call parity and shown
  with ≈.
- **Forward and discount factor** come from a weighted put-call parity fit over the
  strikes nearest the money, per expiry, with capped weights and median-based outlier
  rejection so one bad quote cannot move it. No dividend or borrow assumptions are
  needed. Expiries under 30 days borrow the median rate of the longer ones: over a few
  days the discount factor is within a basis point of 1, so bid/ask noise swamps the
  slope.
- **Implied volatility** is Black-76 on that forward: Newton's method in log-price space
  from a Corrado-Miller initial guess, with a bisection safeguard. About 0.4 µs and 5.4
  iterations per option. Each strike's smile IV comes from its out-of-the-money side,
  and both sides' Greeks use it.
- **SVI surfaces** fit each expiry's OTM total variance with deterministic, constrained
  quasi-explicit calibration and capped bid/ask IV weights. Fits run lazily in the
  API, cached per analytics snapshot; butterfly and calendar grid violations remain
  visible alongside market points. [Model, checks and timings](svi.md).
- **Model-free IV and realized volatility** are computed lazily from the same snapshot
  and stored candles. Model-free IV uses OTM quote mids and our parity forward and
  discount, with known American exercise premiums removed. It is not Cboe's VIX.
  `openport-probe --compare-mfiv` compares SPX against published index minute bars;
  measured differences will be added here after a live session.
  [Metric definitions and comparison procedure](volatility.md).
- **American-style** equity and ETF options cannot fit a rate from their own parity:
  early exercise makes puts worth more at higher strikes, which reads as rates between
  -3% and +2% for SPY and QQQ. They take the zero-rate curve fitted on a European index
  (SPX when subscribed) or the flat `--rate`. Each option's early-exercise premium,
  American minus European value on the same Leisen-Reimer tree, is removed before the
  forward and IVs are fitted; displayed quotes stay as quoted.
  Known cash dividends from `--dividends` are escrowed: the tree starts at spot minus
  their present value, and exercise adds back the value of payments still to come.
  Ex-dates take effect at midnight New York time; only those after market time and
  before settlement enter each expiry. Residual continuous carry preserves its
  first-pass parity forward. Without eligible cash, the existing continuous-yield
  method is unchanged. Summary expiries report the cash amounts used in `dividends`.
  [Accuracy and cost](american-analytics.md).
- **Greeks**: delta and gamma with respect to spot, vega per vol point, and theta per
  calendar day with the forward held fixed, which is how Cboe quotes it. For American
  options they are European Greeks at the de-Americanised IV, accurate out of the money.
- **Exposure** is a derived OI-weighted model, with selectable positioning assumptions
  and expiry slices. It does not observe dealer inventory. See the formulas below.

## Dealer exposure

The default `convention=long_calls_short_puts` assigns dealers long calls (+1) and
short puts (−1). `open_interest` is accepted as a deprecated spelling of the same
convention, and responses always name it `long_calls_short_puts`. `short_all`
assigns −1 to both sides. These are assumptions about
positions, not a deduction from customers holding options.

`customer_flow` would require signed customer trade/volume direction (dealer weight
opposite the customer aggressor, buying +1 / selling −1), and would describe flow,
not outstanding OI inventory. Current normalized feeds retain unsigned cumulative
volume, with no aggressor side or signed-volume field. This mode is **unavailable**:
the API returns `exposure.available=false`, a reason, and null exposures and levels.
Volume alone, call/put identity, quote changes, and zero volume do not supply a sign.

For each option side let `w = convention_sign × received_OI × contract_multiplier`.
At each strike both sides use the same OTM smile IV (falling back to the ITM IV).
A side with received finite nonnegative OI contributes even when its own quote
cannot solve an IV. Missing OI is omitted; `oi_coverage` reports received OI among
sides with a finite smile IV. Zero OI is received data. Totals without OI remain
zero with zero/unknown coverage, not evidence that actual dealer exposure is zero.

With spot `S`, fitted forward `F`, discount `D`, smile volatility `σ`, actual years
`T`, and exposure years `τ = max(T, exposure_min_days / 365)` (default half a day):

- `a = F/S`, `b = log(a)/τ`, `q = −log(Da)/τ` are fitted carry and yield over the
  exposure horizon.
- `d1 = log(F/K)/(σ√τ) + σ√τ/2`, `d2 = d1 − σ√τ`.
- Spot delta is `Δcall = Da N(d1)` and `Δput = −Da N(−d1)`.
- Spot gamma is `Γ = Da φ(d1)/(S σ√τ)`.
- Vanna per vol point is `Vanna = −Da φ(d1) d2/(100 σ)`.
- Charm per calendar day is
  `Charm = [q Δ − Da φ(d1) (2bτ − d2 σ√τ)/(2τ σ√τ)] / 365`.
- Vomma per squared vol point is
  `Vomma = D F φ(d1) √τ d1 d2/(10000 σ)`.

`N` and `φ` are the standard normal CDF and density. Charm is the instantaneous
change in **spot delta as calendar time passes**, holding spot, IV and fitted
continuous carry/rates fixed. Its sign is the negative of differentiation with
respect to remaining time. It is not a one-day finite roll through settlement or
an American exercise forecast. The floor regularizes the local sensitivity at
`τ`; `F` and `D` remain anchored to the actual snapshot, and `b` and `q` spread
their log-carry over the same `τ`. Dividing by the true `T` instead would turn a
1-point forward/spot gap 30 minutes before the close into a carry of about 350%
a year and let it dominate 0DTE charm; over the half-day floor it is about 15%.
Displayed
option Greeks elsewhere continue to use the true remaining time.

| API metric | Sum over option sides | Units |
| --- | --- | --- |
| `gex` | `Σ w Γ S² × 0.01` | Dollar delta change per 1% spot move |
| `vex` | `Σ w Vanna S` | Dollar delta change per volatility point |
| `dex` | `Σ w Δ S` | Dollar delta |
| `cex` | `Σ w Charm S` | Dollar delta change per calendar day |
| `vomex` | `Σ w Vomma` | Dollars per volatility point squared |

One volatility point is 0.01 in decimal volatility. Vomma is price curvature,
not a delta-hedging flow; its quadratic price contribution is approximately
`0.5 × VOMEX × (vol-point change)²`. Delta and charm are evaluated separately for
calls and puts before applying weights; equal call/put OI does not cancel DEX.

`GET /api/underlyings/{symbol}/exposure?convention=short_all&slice=0dte` selects a
convention and time slice. `slice=all` is the default; `0dte` includes live expiries
whose expiry date equals the **snapshot's New York calendar date**, and `ex_0dte`
includes the remaining live expiries. AM-settled contracts disappear after their
settlement instant. UTC midnight, the next overnight trading date, and the local
wall clock do not redefine this date. Replay uses its own recorded snapshot time.
Filtering precedes the display expiry limit. Every metric has aligned expiry and
`total_*` strike arrays; those arrays sum the displayed expiries. The `exposure`
summary sums **every strike and expiry in the selected slice**, independently of
`window` and `expiries` display limits.

For each metric, `exposure.levels` reports `positive_wall` (largest positive strike
sum), `negative_wall` (most negative), and `flip`. Flip searches re-evaluate the
same selected positions at hypothetical spots, keeping strike IV, time, forward
per spot and fitted rates fixed. The nearest sign change within the configured
search range (default ±10% of spot, 81 grid points) is returned: brackets are
bisected nearest spot first, and only until no remaining bracket can be closer.
An absent sign, zero profile, or underflowed tail gives null; a coarse grid can
miss two roots between adjacent samples. Long calls / short puts give nonnegative
DEX, so a delta flip or negative delta wall is normally absent. Gamma's legacy
`gamma_flip`, `call_wall`, and `put_wall` alias its corresponding generic levels;
under other conventions the wall names describe signs, not observed call/put inventory.

## Accuracy

Checked against Cboe's own published IVs on 2026-09-24 during the session: across every
expiry, the median difference on out-of-the-money options within 10% of the forward is
0.012 vol points for SPX, 0.030 for QQQ and 0.028 for SPY. Theta matches Cboe's to a
median 0.9% for SPX, and 2% to 3% for QQQ and SPY, whose Greeks here are European ones
at the de-Americanised IV.

## Performance

On an Apple M2 Max, a full analytics pass over the SPX chain (30,182 options across 63
expiries) takes about 40 ms, and SPY with de-Americanisation (13,028 options across 33
expiries) about 29 ms. The engine recomputes at most once a second, and only for
underlyings whose data, rate curve or cash-dividend schedule changed; each pass
publishes an immutable snapshot, so HTTP readers never block the feed. While the
engine is busy, the queue from the providers keeps only the latest quote per contract.

```
provider thread ──events──▶ queue ──▶ engine thread: chain book ──▶ analytics
                                                                        │
                        web terminal ◀── JSON API + WebSocket ticks ◀── immutable snapshot
```
