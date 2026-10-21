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
  live measured differences are pending. No live MFIV-versus-VIX accuracy result
  is claimed. The terminal tracking line uses same-session daily observations;
  the probe requires exact matched market minutes.
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
- **Exposure** uses the common open-interest convention: dealers are assumed long the
  calls and short the puts customers hold. That is a modelling convention, not knowledge
  of anyone's positions. GEX per strike is gamma × OI × multiplier × S² × 1%, dollars of
  hedging per 1% move; VEX is vanna × OI × multiplier × S per vol point. Exposure uses at
  least half a day to expiry so the local gamma of an option minutes from expiry does not
  drown out everything else. The gamma flip is where total GEX changes sign, found by
  bisection over the same positions as the total.

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
