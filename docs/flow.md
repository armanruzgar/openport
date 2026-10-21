# Time and sales and options flow

The Flow tab on Trade shows the option prints a feed delivers, newest first, and
sums their premium by expiry and strike. `GET /api/underlyings/{symbol}/flow`
serves the same data ([API](api.md#status-and-market-data)).

## Where prints come from

| Source | Prints |
| --- | --- |
| Databento | Every OPRA trade, with the printing exchange (publisher id) |
| Recordings and imported days | The trades they hold; ThetaData imports carry the exchange code when its rows have one |
| Cboe, Massive, ThetaData live, Tradier, tastytrade | None: these adapters deliver quotes, not option trades. The tab says so, and shows no tape |
| Demo market | Simulated prints, labelled on every row and in the response |

OpenPort never invents prints for a market-data feed. The demo market publishes
cumulative volume but no trades; each rise in a contract's volume becomes one
simulated print of that size at its bid, ask or midpoint, picked deterministically
from the contract and time. They exist so the tab can be tried; they say nothing
about real flow.

Each underlying keeps its last 5,000 prints in memory (`capacity`); `total` counts
every print seen since the engine started and `evicted` the ones dropped for space.
Each engine, live or replay, keeps its own tape. Under overload the event queue
may drop trades before quotes; `dropped_trades` counts them (the same number as
`dropped_events` in status). It is feed-wide, since the queue drops a trade before
its underlying is known, and those trades are not in `total`.

## Classification

A print is compared with the option's quote in the chain book when the print is
applied, so with the newest quote received before it. The queue coalesces quotes
under load, so that quote can be newer than the trade; when the quote is stamped
after the trade, the print is `unquoted` instead of classified against a quote it
never met.

| `side` | Price | `aggressor` |
| --- | --- | --- |
| `ask` | within half a cent of the ask | `buy` |
| `bid` | within half a cent of the bid | `sell` |
| `mid` | strictly between them | null |
| `outside` | above the ask or below the bid | `buy` above, `sell` below |
| `unquoted` | no two-sided quote, or only one stamped after the trade | null |

Net premium is price × size × multiplier, added for buys and subtracted for sells,
apart for calls and puts. Mid and unquoted premium is reported as `unclassified`.
Aggressor is a quote-rule inference, not the exchange's report.

## Sweeps, blocks and multi-leg prints

These are inferred from the tape, not reported by the feed:

- **Sweep**: two or more prints of one contract, in one direction, on at least two
  known venues within one second. Prints without a venue are never sweeps.
- **Block**: a single print of at least 100 contracts (`block_size`).
- **Multi-leg**: prints of different contracts of one underlying, on the same venue
  (or none known), within one millisecond, whose sizes are in a whole ratio of at
  most 4:1. This can join unrelated prints and miss real spreads; the feed's trade
  conditions are not read. Simulated prints are never grouped.

## Filters

`type=call|put`, `side=` (comma-separated sides), `expiry=` (an expiry id),
`min_size=` (contracts), `min_premium=` (dollars) and `only=sweep|block|multi_leg`
filter both the tape and the sums; `limit` (1–1000, default 200) caps the prints
returned. `matched` counts the filtered prints, `retained` the kept ones.
