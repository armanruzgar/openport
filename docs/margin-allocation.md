# Exact strategy margin allocation

The rules are in [paper trading](paper-trading.md). Allocation is independent for
each underlying. Contract quantities are integers; monetary inputs are integer
micro-dollars, with exact rational arithmetic for proportional position values.

## Integer model

Start with all shorts naked (cash-secured puts for cash accounts and IRAs), all
longs paid for, and short shares holding their short-sale requirement. Maximize
the saving from this baseline using these nonnegative variables:

- For each short option, its integer quantity assigned to its expiry's pool.
- For each long option and each eligible earlier or equal short expiry, its
  integer quantity assigned to that pool.
- An integer quantity for each legal straddle or share-cover pair.
- A reserve for each pool, measured in tenths of a dollar (one strike mill times
  the standard 100 multiplier). It costs 100,000 micro-dollars per reserve unit.

Each position has a capacity constraint. A pair consumes the same number of
contracts from both positions. A short share lot has two separate roles, as in
the existing rules: a long call may protect its short-sale requirement while it
also covers a short put. These roles have separate capacities; the short-sale
requirement is charged only once. Odd share remainders retain their own charge.

Each pool's reserve is at least the negative sum of its allocated intrinsic
payoffs at zero and every eligible long strike. Its net call quantity must be
nonnegative. This describes the complete worst loss: between strikes the payoff
is linear, a short strike can only bend it downward (so cannot introduce a strict
minimum), and a nonnegative call tail excludes an unbounded loss. Reserves need
not be branched: integer allocations give integer reserve units automatically.

Cash accounts have no option pools or straddles. IRAs have pools but no straddles.
Share relations and naked house uplifts use the same account rules as before.

## Why this is the optimum

Every legal partition maps to the model. A vertical is a two-leg pool; leaving
its short naked represents the cap at naked cost. Multiple pools with the same
short expiry can be merged because the worst loss of their sum is no greater
than the sum of their worst losses. Naked residuals and the other pairs are
represented directly. Conversely, an integer feasible solution specifies a legal
partition with exactly its stated requirement. Thus minimizing the model is
equivalent to minimizing over all permitted whole-contract allocations.

The simplex dictionary supplies proposals only. For the savings LP `max c*x`
subject to `A*x <= b`, `x >= 0`, the implementation checks rational prices `y >= 0`
with `A^T*y >= c`. Their exact `b*y` is an upper bound on possible savings by weak
duality. Prices are repaired through capacity rows; pool reserve price sums are
capped first. Every repair preserves earlier constraints. Integer primal
allocations, capacities, payoff reserves, and costs are recomputed exactly.
If quantized prices leave a gap smaller than a dollar, the proposed basis's dual
equations are solved by rational elimination and checked again. This avoids
branching on a few micro-dollars of price error amplified by large quantities.
Rounding the resulting requirement lower bound upward to micro-dollars is valid
because published requirements are integral micro-dollars. When all saving
coefficients are integers, their greatest common divisor gives a stronger exact
lattice bound. A pool reserve's step is the gcd of its payoff coefficients times
100,000, since it is the maximum of those integer payoffs. Rational coefficients
use the ordinary micro-dollar bound instead.

A fractional allocation is split into `x <= floor(x)` and `x >= floor(x)+1`.
Both branches must be exhausted or certified unable to beat the incumbent.
Infeasible branches require an exact Farkas certificate (`y >= 0`, `A^T*y >= 0`,
`b*y < 0`). Floating-point infeasibility, equality, or near-integrality alone is
never accepted as proof. Pools are priced first without the dense straddle
columns and initially only the endpoint payoff constraints; their certificate is
still checked against every column. Unused constraint prices are zero. If this
does not prove optimality, all payoff constraints are priced, then all pair
columns are introduced. The final model includes every permitted allocation.

The union of allocations for two books is feasible for their combined book;
merging same-expiry pools cannot raise cost. Exact optimization therefore cannot
require more than the two separate requirements together. Upward rounding once
per underlying preserves this inequality. This argument concerns identical
underlying prices, account policies and compatible position values.

## Determinism and fallback

Symbols, expiries, columns, pivot ties and branch order are deterministic. The
budget allows 50,000,000 work units per underlying (dictionary cell visits and
cubic rational-basis elimination bounds), at most 128
allocation resources and 1,000,000 matrix cells. Rational arithmetic uses checked
signed 128-bit intermediates. The floating-point proposal dictionary scales both
payoff units and quantities; published costs and certificates remain unscaled
exact values. Exhausting
a limit or failing to certify a numerical proposal returns the retained bounded
allocator and reports `allocation: "bounded"`. Such a result is feasible and
conservative but carries no optimality or subadditivity claim. `exact` is emitted
only after proof, including the min-cost-flow shortcut for books of naked shorts
whose pools cannot improve their requirement. No cutoff uses the wall clock.

The exact requirement is rounded upward once per underlying. Individual parts
receive successive differences of the rounded cumulative rational cost, so their
integer micro-dollar amounts sum to that total. Older recorded runs without the
`exact_margin` driver flag keep their previous allocation and rounding behavior.

## Validation and cost

The tests compare 2,400 small books with an independent exhaustive partition
oracle, including cash, IRA and margin accounts and house uplifts. Separate
rational-value regressions check fractional micro-dollar buy-back values and
odd short-share remainders. Seed
`0xF490123` checks 20,736 margin pairs and 5,423 allowed IRA pairs: zero
subadditivity violations and zero fallbacks. Tests also cover integer branching,
forced resource-limit reporting, a large-quantity certificate, share covers,
deterministic journals, and verification/resumption of both driver choices.

Five Release benchmark repetitions on the development host measured these CPU
medians. Timings are measurements, not wall-clock limits; every listed result was
certified exact. `bench/bench_margin.cpp` publishes the `exact` counter.

| Book | CPU median |
| --- | ---: |
| 14 mixed-expiry legs | 0.20 ms |
| 80 mixed-expiry legs | 2.28 ms |
| 80 legs, quantities multiplied by 10,000 | 2.40 ms |
| 80 fragmented short legs | 0.32 ms |
| Adversarial 13-leg seeded book | 3.70 ms |

The prior pairing alone took 1.13 ms for the 80-leg mixed book and 0.39 ms for the
fragmented book in the same run. The operation cap remains necessary for harder
integer books; capped results are explicitly distinguished from certified ones.
