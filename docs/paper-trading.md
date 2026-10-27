# Paper trading (v1)

`openport::trading` is a C++20 library linked as `openport::trading` / `openport_trading`.
It depends on `openport_core`; JSON and OpenSSL Crypto are private dependencies.
There are no threads, network calls or wall-clock reads. Only `FileJournal` accesses
files. Caller timestamps are **effective market time**, UTC nanoseconds, nonnegative
and monotone. Delayed feeds must pass their delayed market time, not receipt time.

## Public API and integration

| Header under `include/openport/trading/` | API |
| --- | --- |
| `money.hpp` | `Money::parse`, `from_micros`, `from_double`, `str`, checked arithmetic and `prorate` |
| `types.hpp` | `Reason`, `TradingError`, `Decision`, contracts/quotes/valuation checks, orders, fills, limits, configuration |
| `ledger.hpp` | `Account`, `Position`, `Ledger::fill`, `settle`, `restore` |
| `risk.hpp` | `portfolio_risk`, `check_exposure`, `scenario_grid`, risk buckets and scenario cells |
| `journal.hpp` | `Journal`, `FileJournal::create/read/resume/flush`, `verify_journal`, `JournalRecovery` |
| `session.hpp` | `TradingSession`, `CommandResult`, immutable `TradingSnapshot`, `PayoutQuote`, `payout_quote` |
| `evaluation.hpp` | `Evaluation`, `EvaluationDay`, `Payout`, `AttemptSummary`, `Closure`, `BuyingPower`, `naked_requirement`, `MarginLeg`, `margin_requirement` |
| `history.hpp` | `Lifecycle`, `lifecycles` (round trips rebuilt from fills and closures) |

Typical ingress:

```cpp
using namespace openport::trading;
auto journal = FileJournal::create("paper.jsonl");
TradingSession session(SessionConfig{}, market_time, journal);
session.define(resolved_contract, market_time);
session.on_quotes(quote_batch, valuation_batch, market_time);
auto result = session.submit(order_request, market_time);
auto published = session.snapshot(); // shared_ptr<const TradingSnapshot>
```

`define` accepts an already resolved `md::OptionContract`. Parsing an OSI string
alone is not proof of a known listed definition; that responsibility belongs to
the caller. All subsequent references use its **canonical padded OSI**, never a
provider's dense instrument ID. Re-registering identical terms is harmless;
conflicting terms under one OSI reject.

Reducer journal commands are `session_start`, `run_input`, `definition`, `submit`,
`modify`, `cancel`, `cancel_all`, `cancel_orders`, `close_positions`, `market`,
`limit_change`, `guardrail_change`, `kill_trip`, `kill_reset`, `settlement`,
`closing_print`, `day_rollover`, `account_reset`, `payout`, `annotate`, `group`,
`ungroup`, `day_note`, `exercise`, `abandon`, `exercise_instruction`, `stock_trade`
and `alert`. Every completed command, including a business rejection, increments `account_version`. Business failures return a
`Decision` with code/message and numeric actual/limit/scope where applicable.
Invalid command batches, arithmetic overflow, invalid configuration and persistence
failures throw `TradingError` with an explicit code. They publish no partial state.
`submit` returns the assigned order ID even for rejection. Accepted orders can
immediately finish or cancel; inspect their resulting status and filled quantity.

Supply at most one quote and one valuation per OSI per `on_quotes` batch. The whole
batch is installed before any matching or risk decision. Unknown symbols, future
or negative data times, and duplicate symbols in one batch reject the whole batch.
An empty batch advances time, expiry, DAY cancellation, freshness and loss checks.
On an idle account (no positions, no open orders, the attempt started) time drives
none of these, so an empty batch is not a transaction: nothing is journaled, the
account version and snapshot time stay put, and the next transaction advances the
clock. Rollover is its own command and still happens on a new trading day. A thrown
invalid input does not advance time; the caller should send a valid clock batch when
it still needs boundaries processed.

Each quote has a strictly increasing **per-contract observation number**, independent
of price changes. Repeated or older IDs and backward quote times are ignored;
they cannot replenish liquidity or marks. Same prices with a genuinely new ID can
replenish liquidity. A locally repeated cached poll must keep its old ID. Older
valuation timestamps are ignored; same-time valuation revisions are allowed.
Invalid/nonfinite valuation frames are stored as explicitly invalid, not zero risk.

Snapshots own account, marked positions, open orders, order/fill history, risk and
utilisation, scenarios, quality flags and account version. Previous publications
remain unchanged. `snapshot_json()` is a stable diagnostic serialization: money is
integer micro-dollars and enum ordinals (except string reason codes) are internal
schema details, **not a proposed HTTP wire format**. A session without a journal is
an explicitly ephemeral simulation. Production integration should provide a sink.

## Instruments and prices

Standard contracts with multiplier 100 are accepted in two families. European
cash-settled index options: SPX/SPXW, XSP, NDX/NDXP, RUT/RUTW, XND, MRUT, DJX,
VIX/VIXW. American equity, ETF and OEX options on any other root, e.g. SPY, QQQ, IWM, DIA,
AAPL. The underlying and exercise style must match the root's conventions: American
exercise on a European index root is `AMERICAN_UNSUPPORTED`, a European contract on any
other root is `ROOT_UNSUPPORTED`. A listed adjusted series (`standard` false) is
`NONSTANDARD_UNSUPPORTED` unless its deliverable is known: `define(contract, time,
deliverable)` admits a share-settled American series with its whole shares and cash in
lieu a contract, and the series a split adjusts keep theirs (see
[Splits and adjusted series](#splits-and-adjusted-series)). The feed's own nonstandard
listings carry no deliverable, so the desk still leaves them out.

American equity and ETF options deliver shares; OEX, like the European index roots,
settles in cash. By default, held into expiry, an equity or ETF option a cent or more in the money
at the settlement reference is exercised or assigned: it settles at intrinsic value,
and 100 shares a contract change hands at the settlement reference, bought by long
calls and short puts, sold by long puts and short calls. Together that costs the
strike, while the option's own trade records its real result and the shares' P&L is
their own move from there. `exercise(symbol, contracts, time)` exercises long, in-the-
money contracts early in the same way against the underlying's fresh price, so any
time value left is a cost; it takes the account's checks and, with the
`buying_power` rule, must fit within it. Shares an exercise opens, or adds to, enter
the plan as a share trade would: its entry windows and allowed underlyings,
`require_stop_loss` (no bracket can protect delivered shares, so sell the option
instead), hedging, `max_trade_risk` (a long's whole value, a short's unbounded loss)
and the exposure limits all apply. Shares that only reduce a holding, as a put
exercised against long shares, take none of them.

### Simulated expiry pin risk

`pin_risk` defaults to false on every existing plan. Enable it when creating or
resetting an account (also in the terminal's margin settings); like the financing
rates, it is a program rule, so an account that changes it no longer reports the
preset's `plan_id`. For a physical short
option whose actual settlement reference is within $0.10 of its strike, each
contract has a 25% chance of the contrary exercise decision: an otherwise exercised
short may lapse, and an otherwise lapsed short may be assigned. Calls and puts use
the same rule. Longs, cash-settled indices and shorts outside the band keep their
normal exercise rules.

This is a deliberately simple simulation of after-hours uncertainty before the
OCC 17:30 ET exercise cutoff, not observed after-hours trading or an estimated
market probability. Settlement still posts at its ordinary simulated settlement
moment. No later market timestamp or price is invented. The splitmix64 draw mixes
`pin_risk_seed` (0–2147483647, default 0), attempt start, padded OSI and expiry date;
recovery and replay retain the same outcomes. A `PIN_RISK` warning appears on expiry
day before expiry when a held short is within the band, even if out of the money.

Pin-model settlements close the option at zero and deliver only the assigned
contracts' shares at the strike, marked using the actual settlement reference.
This records the exact strike cash flow, including out-of-money assignments.
Exercise fees apply only to assigned contracts. Settlement events record
`pin_risk`, `simulated`, `assigned_contracts` and `seed`; ordinary settlement sources
keep their original provenance. The warning's ordinary delivery estimate remains
the default exercise outcome; pin risk can change the actual number delivered.

A long held into expiry can take a **do-not-exercise instruction**:
`instruct_exercise(symbol, true, time)` on a long option makes
it expire worthless at its settlement, however far in the money, with no shares or cash
delivered (its settlement closure is at 0.00), as a broker's contrary exercise advice
does. `false` withdraws it. The instruction ends with the position: selling the
contracts, exercising them early, abandoning them or a reset clears it. It also
forfeits the cash settlement of an index long; a short cannot take an instruction
(`INVALID_ORDER`). Both disposal commands remain allowed under the kill switch
and after an evaluation passes or fails.

**Delivery and buying-power shortfalls.** Delivery is mandatory for held in-the-money
contracts unless a long has a do-not-exercise instruction; insufficient cash or
buying power does not cancel it. The held-book `EXPIRY_DELIVERY` warning names the
strike delivery cost and projected buying power for today's in-the-money physical
expiries (including exercise fees and working-order reservations). Negative projected
buying power is a warning even before a configured pre-expiry cutoff: an unfilled
cutoff close can still deliver. Close or reduce the position before expiry to avoid it.

On practice and evaluation plans, automatic delivery records `buying_power_after`
on its share fill. If negative, the terminal delivery announcement and server `alert`
notification name the shortfall and the next steps. This is a margin-call-style
buying-power restriction, not a new kill latch or an automatic liquidation: openings
that consume unavailable buying power reject with `BUYING_POWER`; closes that free
buying power remain allowed, even if a partial close leaves it negative. Selling
shares that cover a short option can still be refused if it worsens the requirement;
close that short first or close the covered position together. Share closes require
an open share session and a fresh price. Restore enough buying power by reducing
positions before opening again; rollover alone does not clear the deficit. Other
plan limits and evaluation failures still apply independently.

**Early assignment** follows the market rather than a model: at each day rollover
(`roll_day`, overnight), holders exercise a short American equity or ETF option when
they did better exercising than holding it at the close, as exercise notices are due
that evening. That is when its closing mark is below its intrinsic value at the
underlying's close (a deep put whose time value is gone), or, for a call, when its
time value is less than a dividend going ex on the new day in the account calendar.
The OCC allocates exercises to short positions at random, and
openport cannot know how many holders exercise, so each such contract is assigned
with even odds, drawn from the account's attempt, the contract and the date: a
position can be assigned in part, or not that night, and a replay assigns the same
contracts. Assigned contracts are bought back at intrinsic value
(`ClosureKind::Assignment`) and deliver 100 shares a contract at the underlying's
close (`StockSource::Assignment`), together the strike; the new day takes the
difference from the marks, and the shares' move from the close. Options expiring
that day settle instead.

Revision 4 scenarios generate American SPY/QQQ option prices using known cash
dividends and an early-exercise premium; index prices remain European. Simulated
ETF prices drop on their generated or explicit ex-dates, keeping the preceding
close cum-dividend. See the [calendar and pricing rules](scenarios.md).

**Dividends** come from a file, from Massive or from the simulated scenario calendar.
`openportd --dividends FILE` reads
`SYMBOL,YYYY-MM-DD,AMOUNT` lines (the ex-date and dollars a share; blank lines, `#`
comments and a `symbol,...` header are skipped) with `parse_dividends`. `--dividends
massive` reads the stock and ETF symbols' cash dividends in US dollars from Massive's
dividends endpoint (`GET /stocks/v1/dividends`, in every stocks plan) with
`MASSIVE_API_KEY`, whichever provider supplies the quotes: those going ex from a month
back on, when the server starts and every six hours after, hourly while a symbol fails
(`providers::MassiveDividends`), and two distributions going ex together are paid as
one. New ones reach the live accounts, and replays started after them. The same
schedule supplies known cash payments to American option analytics, with revised
or removed payments refreshing analytics even without new quotes; see
[American analytics](american-analytics.md#known-cash-dividends).
Revision 4 Replay runs merge their generated calendar over matching server payments,
then explicit session entries over matching generated payments. The merged schedule
is saved in the start record, so resume and verification use the same payments.
The revision 4 demo feed uses its generated calendar when no `--dividends` source
was supplied, including American analytics and dividend/assignment warnings. An
explicit source replaces the demo account calendar, without changing its prices.
### Splits and adjusted series

`openportd --corporate-actions FILE` reads `SYMBOL,YYYY-MM-DD,split,NUMERATOR:DENOMINATOR`
lines (`2:1` doubles the shares, `3:2` makes 100 shares 150, `1:10` is a reverse split;
blank lines, `#` comments and a `symbol,...` header are skipped) with
`parse_corporate_actions`. `roll_day(time, dividends, actions)` applies those going ex
after the last trading date and on or before the new one, once each, after the night's
assignments and before the day's dividends:

- Held shares split toward zero, keeping their basis. A fraction of a share left over is
  paid in cash at the split mark (the last mark times denominator over numerator; the
  average cost without a mark), with the basis it carried released as realised P&L, as a
  broker's cash in lieu is. The stock fill records the split (`source: "split"`, no price)
  and the round trip continues.
- The mark splits with the shares, so equity does not move.
- Every share-settled series on the underlying that has not expired keeps its symbol,
  strike, contract count and 100 premium multiplier, as OCC adjusts a series, and takes
  the split deliverable: 100 shares a contract become 200 for 2:1, 150 for 3:2 and 10 for
  1:10. A fraction of a share in the deliverable becomes cash in lieu at the split mark (a
  simulated estimate of the amount OCC later fixes). Once a contract delivers less than
  one whole share (100 shares after 1:200 or 1:1000), it delivers cash only, all of it in
  lieu: such a series pays a fixed amount whatever the stock does, is margined at what
  buying its shorts back costs, reprices with no move and delivers no shares when it
  settles, is exercised or is assigned. OCC instead re-strikes whole-number
  forward splits (twice the contracts at half the strike); the adjusted series pays
  exactly the same.
- Working share orders on the stock are cancelled with `CORPORATE_ACTION`; working option
  orders stay, their premium being per contract either way.

An adjusted series settles, is exercised and is assigned on its deliverable: one unit is
worth its shares (and cash) over 100 against the strike, so a 2:1-adjusted 500 call with
the stock at 260 is worth 200 × 260 / 100 − 500 = 20, and delivers 200 shares at the
settlement price, together the $50,000 its strike was worth. Strategy margin takes each
underlying's series of one deliverable as the standard series that pay the same (each
contract shares/100 standard contracts at its effective strike, 100 × strike less the
cash over the shares) and covers them with lots of their own deliverable; shares go to
an adjusted group's shorts first. Cash accounts and IRAs cover an adjusted call with its
deliverable's shares. Scenario grids, risk profiles, breach risk and historical stress
reprice it the same way. Portfolio margin scans the standard series and shares and adds
strategy margin for adjusted series, which its scan does not reprice. The desk quotes,
values and fills an adjusted series from the standard series that pays the same (the 250
call for a 2:1-adjusted 500 call): prices times shares/100, displayed sizes over it and
Greeks scaled the same, once the feed lists that series. When none is listed (a 3:2 or a
reverse split leaves an effective strike such as 333.33 no exchange lists), the listed
one has never quoted, or the series delivers cash only, the desk values it by model: Black at its effective strike, at the
expiry's forward and discount from the analytics, with the smile IV interpolated between
the listed strikes either side of it (the nearest one beyond the listed range), scaled
to its shares (a cash-only series is worth its intrinsic value, undiscounted for an
American series, which can be exercised now, and discounted for a European one, the
discounted value being what historical stress margins a short one at, whatever the
shock). It is a
European model of an American series, floored at the deliverable's exercise value at
the analytics' spot (shares times its distance from the effective strike, over 100),
worked in the same decimal arithmetic as the overnight assignment check so the floored
mark never rounds below it, as the American series would be: a deep in-the-money put's European value falls below
it, which would show a holder a false loss and give a short's holders a reason to
exercise overnight. On the floor it moves one for one with the deliverable's shares
(delta of shares/100, no gamma, vega or theta). The value marks the position as a quote with no
size, `modelled_mark: true` on the position, so the account stays valued, rollovers
continue and margin and risk see it; it never fills an order, so the series cannot be
traded until a listed series quotes it (close it by exercise, or wait for expiry). An
underlying without IVs around the strike leaves it unmarked, which holds the rollover
as any missing mark does. Positions report `deliverable` (`{shares, cash}`, null for 100 shares) and
`GET /api/account` the applied `corporate_actions`. Replays and scenario runs given a
corporate-actions file keep it in their start record, so resume and verification apply
the same splits; generated scenario prices do not split themselves.

`roll_day(time, dividends)` takes those going ex after the last trading date and
on or before the new one (`dividends_due`, so a server that was down
across an ex-date still pays it), after the night's assignments: shares held into the
ex-date receive `per_share * shares`, rounded to the nearest cent (ties away from
zero) as brokers pay it, in cash and realised P&L, and short shares pay it, once per
symbol and date. The record moment is that rollover, not the ex-date's midnight:
17:00 ET the business day before the ex-date on a live feed (or the first batch after
it), and the ex-date's 09:30 open on the demo feed. Shares held at the rollover are
paid; shares an exercise delivers after it are not, although OCC's 17:30 exercise
cutoff would capture them. Analytics, by contrast, treat the ex-date as effective from
00:00 New York. Each is a `DividendPayment` in
`TradingSnapshot::dividends`, today's P&L by Greek counts it as other, and the share
round trip holding the shares adds it to its net. The payment keeps the number of
stock fills before it (`after_stock_fill`), so the round trip takes it even when the
shares are sold at the rollover's own market time; a payment recorded before that
comes after the night's assignments and deliveries at its time and before any trade.
Without a dividend source (or a revision 4 simulated calendar), shares held through
an ex-date lose the dividend in price without the cash. Evaluation plans close
positions five minutes before their last trade, so expiry delivery only reaches
accounts without an expiry cutoff. Greeks and
scenarios use the analytics' European Black-76 values at the de-Americanised smile IV.

Shares opened by trading or delivered (`TradingSnapshot::stocks`) are marked at the underlying's price,
which `on_quotes` takes in `stocks` (the engine sends it for every underlying whose
equity options or shares the account holds). The price is fresh within
`max_quote_age` of the market time while the stock market is open, or of its last
close (16:00 ET, 13:00 early; `md::stock_session`) while it is not, so the close
stays current while the options trade on to 16:15 and overnight. After a business
day's regular close, Cboe supplies its positive finite `data.close` as the underlying
price, including revisions, so delivered shares use that close rather than
after-hours trades; without a valid close it keeps the current quote
([Cboe clocks](runtime.md#product-sessions-and-cboe-clocks)).
Generated scenarios from revision 8 likewise hold ETF prices at the 16:00 stock close
(13:00 early) while options trade to 16:15 (13:15 early), so exercise and delivery
use that close. Saved revisions 1–7 preserve their historical price movement.
They count in equity, daily loss and the rules, at their dollar
delta in risk limits and scenarios, and in the P&L by Greek (all delta). Short shares
hold 150% of their value in buying power, and under strategy margin every 100 shares
can cover an option (see buying power under Account rules). `trade_stock(symbol, shares, time, price)`
opens, adds to, reduces or reverses a stock or ETF share position, at the underlying's
fresh price in the stock regular session (09:30–16:00 ET, 13:00 on early closes),
without a fee. The optional feed `price` seeds an underlying the account does not yet
hold; the desk captures it in the command before journaling, so replay uses the same
price and time. Each trade takes 1–10,000,000 whole shares. Index underlyings (SPX,
XSP, VIX, RUT, NDX and the other supported cash-settled roots) have no tradable shares.

An opening or adding trade takes the account's checks: active attempt, kill switch,
personal guardrails, fresh marks and valuations on the book, daily loss, projected
exposure (delta one per share, dollar delta shares times price), and projected buying
power when its rule is enabled. Buy-only and defined-risk plans refuse short shares.
Opening counts once against the personal opening-trade limit; a reversal counts once
too, closing the old round trip and opening the excess with a new basis. A refusal
changes no holdings. Reductions remain possible under the kill switch and guardrails.
New share trades use the same equity, risk, scenarios, P&L attribution, journal notes
and tags, round trips and dividend accounting as delivered shares. Exercise and
assignment add to or net against them. Long shares are paid in full under strategy
margin; every 100 cover a short call, including in a collar with a long put. Portfolio
margin instead scans the whole book.

### Working share orders

`submit_stock_order` (`POST /api/stocks/orders`) places a working order for a stock's or
ETF's shares instead of trading at once: `market`, `limit`, `stop` or `stop_limit`, DAY
or GTC, 1–10,000,000 shares, prices in whole cents. Shares trade at the underlying's
single price, which has no displayed size, so a share order fills whole, at that price,
or waits; partial share fills are not modelled. A limit buy fills on a print at or
below its limit and a sell at or above it (at the print, so a better price improves
it); a stop activates once a print reaches its level (at or above for a buy, at or below
for a sell) and then executes at the print (`stop`, through any gap) or rests as a limit
(`stop_limit`). One the current fresh price already crosses executes when accepted;
otherwise it takes prints stamped at or after its acceptance. Market and stop orders
work in the regular session; `extended_hours: true` lets a limit also work premarket
(04:00 ET to the open) and after hours (to 20:00 ET, 17:00 on an early close), sessions
`md::stock_extended_session` models on the stock market's business days. The prices
are the feed's underlying prints in those hours; the simulator adds no extended-hours
spread or liquidity of its own, so an extended-hours limit can fill only on a feed that
delivers those prints. `/api/status` reports it per provider as
`provider.extended_hours_prints`: false for Cboe, which outside the regular session
publishes only the close, and for the demo market, which prints only through the
regular session; no adapter reports it true yet, and replays report false. The
terminal's share ticket disables its Extended hours box, with a caption saying why,
while it is false; the API still accepts the flag. DAY ends at its date's regular close (after hours' end
with extended hours) with `DAY_END`, and an order placed after it works the next
business day; GTC works until it fills or is cancelled.

An extended-hours limit fills only on a print fresh within the session it fills in
(`max_quote_age` of the market time in premarket and after hours as in the regular
session): at 18:30 neither the 16:00 close nor a 16:05 print fills it. Outside the
regular session a share price keeps its own print time (the close's at the latest)
even while its feed's snapshots are current, since a snapshot feed such as Cboe's
goes on sending the last close then: after earnings, or premarket while SPX options
trade overnight, the close is never taken for a fresh print.

Opening orders take, when accepted, the account checks (kill switch, decided attempt,
plan entry rules), buy-only, defined-risk and cash/IRA short-sale checks that count the
working sells before them, and the plan's stop requirement. While they work they
reserve buying power from acceptance: each, after those accepted before it, what filling
it at its limit (a stop at least at its stop, a market order at the last price) would
take; with buying power enforced, a market order is refused with `STALE_QUOTE` until
the stock has a price to reserve against. So a cash account cannot rest
two buys that together exceed its cash; an order that would leave buying power negative
is refused with `BUYING_POWER`. Each execution then takes every check
`POST /api/stocks/trade` does at its price, including `no_counter_positions` against the
other accounts' held and working dollar delta as the server supplies it for that batch,
as an option order's fill does; a refusal there rejects the order with its reason, while
a stale price, a closed session, a halt or another account's missing valuation only
makes it wait. Fills record
the order on their stock fill (`stock_order`).

`bracket: {stop_loss?, take_profit?}` places, once the entry fills, a GTC stop at
`stop_loss` and a limit at `take_profit` for the shares it traded, each the other's
one-cancels-other; they reserve nothing, only ever reduce, bypass the minimum hold as
option exits do, shrink to the shares still held and end with `POSITION_CLOSED` once
those are closed another way. A bracket stop satisfies `require_stop_loss`, so such
plans can open shares through a bracketed working order, and the plan's trade-risk
limit then measures the move to the stop rather than the shares' value; that stop
cannot be cancelled while the shares are held (`STOP_REQUIRED`). `oco` places
a partner order on the same symbol, as `<client_order_id>:oco`; the first fill of either
cancels the other with `OCO_FILLED`. A retry with the same `client_order_id` and terms is
answered as first accepted (200); other terms under it are 409 `DUPLICATE_CLIENT_ID`.
`POST /api/stocks/orders/preview` runs the order on a private copy and reports the
account after, its reservation included. Cancel-all (for all or the symbol) and
account resets cancel share orders; flatten leaves them. A buy-write (shares and a short call at one net price) is
not offered: the order book has no mixed share-and-option instrument, so the
covered-call template still buys shares first and sells the call as a separate order.

The `defined_risk` plan keeps its stricter option-coverage rule: a short call needs
a long call expiring with it or later. Shares cover calls for margin but do not
satisfy this plan rule; a put in a collar does not substitute for the required call.
This preserves the plan's option-only coverage invariant, including pending option
sales and exercises, and existing journal decisions. Use a plan without `buy_only`
or `defined_risk` for the covered-call and collar shortcuts.

Flattening closes opened and delivered shares while the stock market is open (after its close they
stay, and the flatten lists them in `kept_stocks`), and a decided attempt liquidates them, and a reset
drops them at their mark. With the `buying_power` rule, a sale of shares that a short
call is written against, which leaves the call naked, must leave available buying power
nonnegative when it reduces free buying power, otherwise `BUYING_POWER`; buy the call
back first. Every change is a `StockFill` in `TradingSnapshot::stock_fills`,
with how it came about (`StockSource`: delivery at settlement, early exercise, a trade,
the rule, a reset) and the delivering option, so the trade history follows the shares
too.
Strikes must be positive, representable in OSI's thousandths and eight digits;
expiry dates must be valid in OSI's 2000–2099 range. Explicit AM/PM terms from the
known definition determine expiry; conflicting terms cannot replace a definition.

All booked money is signed int64 **micro-dollars**. `Money::parse("4.60")` is exactly
4,600,000. Input permits a sign, integral digits, and up to six fractional digits;
it rejects whitespace, exponent notation, empty fractions and excess precision.
Money strings preserve every meaningful micro-dollar with 2 to 6 decimal places,
omitting trailing zeros beyond cents. Computed requirements, buying power under
portfolio margin and prorated values can carry more than cents (for example,
`"2033.725875"`). Clients should parse them as exact decimals and round only for
display; the terminal shows buying power and requirements at two decimals without
changing booked values.
`from_double` is an adapter boundary conversion, nearest micro-dollar, ties away
from zero. Prefer exact decimal parsing whenever a source provides decimal text.
Every addition, subtraction, multiplication, division allocation and conversion is
checked; overflow raises `ARITHMETIC_OVERFLOW`, never wraps.

Limit prices are positive multiples of this **v1 simulation tick policy**:

| Root | Price below $3.00 | Price at or above $3.00 |
| --- | ---: | ---: |
| SPX, SPXW, NDX, NDXP, RUT, RUTW | $0.05 | $0.10 |
| XSP, MRUT | $0.01 | $0.05 |
| XND, DJX, VIX, VIXW | $0.01 | $0.01 |
| SPY, QQQ, IWM | $0.01 | $0.01 |
| OEX | $0.05 | $0.10 |
| Other equity and ETF roots | $0.01 | $0.05 |

This table implements the requested policy; it is not a full exchange order-routing
specification. Observed fill prices need not themselves be on the limit-order tick.

## Orders and quote matching

Orders have buy/sell side, a positive integer contract count, a client ID and one
of market/IOC or limit/DAY/GTC/IOC/EXTO/GTC_EXTO/GTD. A triggered market also takes
EXTO, GTC_EXTO or GTD: its TIF controls arming, and execution is one attempt when reached.
Market/DAY or market/GTC, market with a limit, and limit without
a positive price reject. A client ID is 1 to 128 bytes of text without control
characters, and cannot be reused within an attempt, even after a rejected order.
Submitting the same terms again under a used client ID is a retry, not a new order: it
gets the first answer (the order as it now stands, even after a change, or the original
rejection) and records nothing. Other terms under that ID are refused with
`DUPLICATE_CLIENT_ID`, naming the order that holds it, and record nothing either, so
an ID names one order however often a client retries. Client IDs are scoped to the
evaluation attempt: after an account reset, an ID an earlier attempt used is free and
names a new order, and each order reports the `attempt` it belongs to.

Tickets show liquidity warnings for thin or absent two-sided quotes, including
spread percentage, session volume and OI, without blocking submission. Strategy
tickets check each leg. Market tickets show each executable side's displayed size
and requested contracts. The chain marks a quote paper orders cannot fill on (one-sided,
crossed or without displayed size) and, on the account's quotes, the displayed size
its orders have already taken; tickets show both, and say from the server preview how
much of an order can fill on the current quote and what happens to the rest. Optional latency and impact can change the simulated fill,
as described below.
The [liquidity rules](runtime.md#chain-volume-and-liquidity) are browser cues, not
new reducer checks or journal fields.

By default, buy execution uses ask; sell execution uses bid. With `slippage_ticks` (0 by default,
an integer from 0 to 10), buys add that many ticks to the ask and sells subtract them
from the bid, floored at zero. The tick is `tick_size(root, displayed_price)`, using
the displayed far side's tier even if slippage crosses $3. Without inside fills, a limit executes only if
the displayed far side is no worse than its limit, including equality: buys fill at
`min(limit, ask + slippage)` and sells at `max(limit, max(0, bid - slippage))`.
Bracket exits, liquidation and expiry auto-close use the same slippage. Exercise,
settlement and share trades keep their existing prices. Without impact, market orders
never sweep undisplayed depth.
Unfilled DAY and GTC limits rest; unfilled IOC quantity cancels with `IOC_REMAINDER`,
whose message says why nothing more filled: a limit that did not reach the executable
price (named, after slippage when it applies), no fresh two-sided quote, or used-up
displayed (or, with latency, eligible) liquidity.
`filled_quantity` remains separate from terminal state: cancelled orders may have
fills. Each partial fill charges `quantity * fee_per_contract` (default $0.65), or
the account’s optional [itemized fee schedule](#fees).

Both positive prices and both positive integer sizes are required; crossed,
one-sided, missing and zero-size books supply **no liquidity**, except to a combo
exit closing a leg that shows only an ask (see Multi-leg orders), and to a buy that
only closes a short, which can take a fresh book that shows only an ask, as a far
option nobody bids for does: a short quoted 0.00/0.05 is bought back at 0.05 within
the displayed size, its price band measured from the mark halfway to the ask. Buying
more than the short, or selling, still needs a two-sided quote. Locked positive
books are accepted. [Stress scenarios](scenarios.md#events) can author each book
state, stall quotes until marks become stale, and halt/resume the market. During a
stall, marks are retained with `valuation_complete: false` and `STALE_QUOTE` in
quality flags; orders on that feed fail `FEED_STALLED`. Displayed size is an independent bid/ask budget per
(contract, observation), consumed across all paper orders. The same observation
never refills that budget, including after recovery. Each new observation refreshes
it once. The same observation offered again with a later time confirms the quote is
still current: its time and its mark's advance, and its budget does not.
Fractional provider sizes must be converted conservatively by the caller.

Every fill keeps the book it traded against (`Fill::quote`): both sides and their
displayed sizes, what was left of the taken side's displayed size for paper orders
before it (below zero once impact depth is in use), and when the quote was first
given. `quote_time` is when the quote was last confirmed current. A confirmation of
the same observation at a later time keeps the first time, and so does a quote offered
later than the provider gave it (`QuoteObservation::quoted`: the engine supplies the
provider's own quote time, which a snapshot feed can vouch for long after). A fill on
a quote that stayed unchanged for minutes, or one that latency held back, therefore
shows how old its quote was. The book is written only with fills that have it, so
fills recorded by earlier builds recover without one.

By default, resting buy limits cross when ask <= limit; resting sell limits cross when bid >=
limit, on a new valid observation timestamped at/after acceptance. A newly
submitted order may execute against the latest cached quote if it is still fresh.
An observation offered again in a later batch refills nothing, but an order that a
data gap held back when it was new (see below) can take what is left of its budget
while it is still fresh.
For each OSI and side, better limit price wins,
then acceptance sequence (order ID). Buy and sell budgets are separate; buys are
processed first for deterministic cross-side risk effects. OSIs are processed in
lexical order after atomic installation of the entire batch. Submissions also
respect existing better orders when sharing the current budget. By default there is
no queue position, trade-through or hidden-liquidity simulation; the optional
[queue fills](#queue-fills) model adds a seeded queue estimate for resting limits.

### Walking limits

A single-leg or multi-leg DAY/GTC limit may carry
`"walk": {"step": "0.10", "seconds": 10, "limit": "4.20"}`. Each elapsed interval
of market time moves its current limit toward the cap: up for a buy, down for a
sell. Combo prices are signed net debits, so both debit and credit combos move
**up** (a credit of -0.50 walks through -0.40 to -0.30, accepting less credit).
At the cap it rests. Cancel or completion stops it. Ordinary closing limits can
walk; stops, triggered orders, managed bracket exits, held exits, flatten orders,
market orders and other times in force cannot. A walking entry's bracket remains
at its separately submitted exit prices.

The positive step must be on the product's lower-tier tick; the cap must be on its
own price tier's tick (combos use their smallest lower-tier leg tick). Each
single-leg step rounds toward the cap to land on the tick at its new price.
`seconds` is an integer from 1 to 3600; the cap must be at least as aggressive as
the current limit and no more than 1000 steps away. The cap passes the same price
band as an initial limit. Invalid terms reject with `INVALID_ORDER`, off-tick
prices with `INVALID_TICK`, and an out-of-band cap with `PRICE_BAND`.

Intervals count from acceptance or the last scheduled step. Every accepted manual
change restarts the interval from its market time. PUT may replace `walk` with a
new object or remove it with `null`; omission keeps it. Rejected changes leave
both terms and schedule intact. Buying power and full-fill loss projections reserve
at the cap, and execution rechecks the usual current risk and fees.

Clock advances and quote batches drive steps inside the deterministic reducer.
A jump catches up every elapsed step, each with its scheduled time, previous and
new limit, actor `walk` in `changes`, and an `order_walked` event. It matches only
on the current fresh book after the batch installs quotes; it does not invent
intermediate quotes or retroactive fills. Elapsed market time includes gaps and
closed sessions; normal session and expiry gates still control fills and cancellation.
Cancellation at the order's deadline takes precedence over catching up missed steps.
Orders return the current `limit_price`, `walk` and `next_walk` (`time`,
`limit_price`). `next_walk` is null at the cap, after completion or when the next
interval would reach the order's deadline. New and change previews show
`next_walk` when the order would still work. Both terminal tickets offer Walk;
Orders shows its current price and next step, and Edit can change or remove it.

### Optional fill models

**As displayed** is the default: no latency, no impact, no slippage and no inside fills. Existing
plans, account responses and journal records keep their previous bytes when the new
settings are zero. The Rules page offers **Conservative** when starting a plan for
an account's new attempt: 1,000 ms latency, 1 slippage tick and 1 impact tick. This
adds friction for practice on a delayed feed; it does not reconstruct a live market.
The choice does not change the plan's evaluation or margin rules. Custom account
rules can set each value separately. Changing a preset starts a new attempt, with
the usual reset of positions and cash; it does not change a running attempt.

**Inside at midpoint** (`fill_model: "midpoint"`) enables `inside_fill_percent: 50`
with latency, impact and slippage at zero. Custom rules accept any integer from
0 to 100; zero disables inside fills and is omitted from journals and responses.
An untriggered limit strictly inside a valid spread fills at its limit when at
least that percentage of the way from its own side to the far side. On a
4.00/4.40 book, 50 fills a buy at 4.20 or higher and a sell at 4.20 or lower;
4.10 buys and 4.30 sells still wait. At 100 only the usual far-side fills remain.
Marketable limits use the ordinary far-side model (and may get a better price),
and markets and stops are unaffected. Matching runs on submission and each
eligible observation, sharing price/time priority and the far side's displayed
size left with other orders. Reconfirmed observations never restore spent size.

Inside fills retain latency, fees and all ordinary execution checks. They execute
at the limit without added slippage, and **never** use synthetic impact depth:
each leg is bounded by its remaining displayed size even with impact enabled.
For a combo, measure from the near net (buys at bids, sells at asks) to the natural
net (buys at asks, sells at bids). Allocate improvement over natural proportionally
to each leg's spread, weighted by ratio. Leg prices stay inside their quotes;
micro-dollar rounding favors the trader. Ratio-one legs absorb rounding excess,
so the net is exactly the limit when possible, otherwise a few micro-dollars
better per unit. `Fill::quote` keeps the bid and ask that demonstrate the inside
price; no separate fill flag is necessary. This is a deterministic training model,
not an inference that someone actually traded inside that quote.

`impact_ticks` is an integer from 0 to 10. Zero keeps the displayed-size cap above.
A positive value supplies **simulated depth** in blocks as large as the displayed
size on that side. Block zero fills at the displayed price with the configured
adverse slippage. Block one adds `impact_ticks` adverse ticks, block two twice that, and so
on. The tick tier stays that of the displayed price; sell prices stop at zero.
For example, with an ask of $4.20, size 2, one impact tick and no slippage, a buy of
5 SPXW contracts fills 2 at $4.20, 2 at $4.30 and 1 at $4.40. With one slippage tick,
those prices are $4.30, $4.40 and $4.50.

Depth consumed by earlier orders on that side counts toward the same blocks.
Bid and ask remain independent. A new observation resets the budget; reconfirming
the same observation does not. There is no inferred depth when either displayed
size or price is missing, nonpositive or invalid. With impact enabled, the whole
price including slippage must fit a single-leg limit; slippage is not clipped to
make a deeper block fill. DAY/GTC remainders rest, and IOC remainders cancel.
Normal price-band, loss, buying-power and exposure checks still apply at each fill.

Combos take one whole unit at a time with impact enabled. Each leg consumes its
own side's blocks; a ratio spanning blocks records separate prices, using exact
fixed-point money. All legs of that unit pass checks and fill together only if its
entire net fits the limit. Fees use the account’s flat or itemized schedule.
Brackets, manual closes and account-owned liquidation/auto-close orders use the
same impact model.

`fill_latency_ms` is an integer from 0 to 60,000. A positive value holds an accepted
order until every leg has a fresh, valid quote timestamped at or after acceptance
plus that delay. Advancing transaction time alone cannot release it. A later
snapshot of the leg's underlying confirming an unchanged quote can qualify without
replenishing its size; another underlying's snapshot cannot.
On a sparse or 15-minute-delayed feed, the next quote can arrive much later than
the configured delay. There is no wall clock or random timer in this rule.

Stops start their delay when triggered, even if they were armed long before.
Bracket targets start when created by the entry's first fill. In-place modifications
keep the original acceptance time and delay. A delayed IOC stays working until its
first eligible quote, attempts execution once, then cancels any remainder; a limit
can continue waiting for its price. DAY ends, earliest-leg expiry and auto-close
cutoffs run before matching. GTC and legacy bracket exits wait for the regular session;
EXTO/GTC_EXTO use every product session.
Cancels and the kill switch still act immediately on pending orders; reduce-only
closes remain eligible. Account-owned closes also wait, without creating duplicate
pending closes. Normal session, halt and risk checks still apply.

These models are simulations. None knows queue position, hidden liquidity, or
whether the market would have traded at all. Impact invents a price schedule, not
observed market depth; latency selects a later supplied quote, not a future trade.
Neither removes the hindsight advantage of a delayed feed.

### Queue fills

**Queue position** (`fill_model: "queue"`, or `queue_fills: true` in custom rules)
turns on a probabilistic queue model for resting limits, with latency, impact,
slippage and inside fills at zero. It is off by default: an account without it
receives no trade data, journals nothing new and keeps every byte of its journal
and replays. Unlike the execution settings above, queue fills and `queue_seed` are
**program rules**: an attempt that turns them on no longer follows its preset
(`plan_id` is empty) and counts as a new purchase for program costs.

A resting single-leg limit (DAY/GTC, or a triggered stop-limit once working) that
is not marketable **joins the queue** at its price:

- At its own side's touch (a buy limit equal to the bid, a sell equal to the ask),
  the estimated queue ahead is the size displayed there when it joins, plus the
  account's own orders that reached the same price earlier and have not filled.
- Strictly inside the spread, nothing displayed is ahead; only the account's own
  orders that reached that price earlier are.
- Behind the touch (a buy below the bid), it has no queue yet. It joins when the
  touch comes to its price, behind what is displayed then.

An order reaches its price when it is accepted (or its stop triggers), once the
account's fill latency has passed, or when a later change sends it to the back (see
below). **Time priority** among the account's orders at one price follows that
moment, then the order ID: an old order repriced to a level where a newer one
already rests queues behind it, and a print through the price goes to the newer
one first.

Each market batch then **uses the queue up**, in price/time priority across the
account's orders:

1. **Trade prints.** When the feed supplies trades (its `trades` capability, as
   recordings with prints and OPRA-style feeds do), contracts printed at the
   order's price after it joined consume the queue ahead. Prints are counted from
   when the order reached its price, never from before its latency passed or
   before the change that sent it there. What prints past the
   queue the order may fill, at its limit. A print **through** the price (below a
   buy limit, above a sell limit) fills it whatever is ahead: any order there would
   have met it. Prints reported in the batch an order joins in came before it
   rested at the touch, so only those through its price count. A print's optional
   venue is kept with it but never required.
2. **Volume increases.** Without prints, the increase in the contract's cumulative
   day volume from one batch to the next counts as traded at the order's price,
   half of it on this side (volume carries no side or price), only when the order's
   own side of the quote was at its price in **both** batches. An increase across
   a batch where the touch was elsewhere is not counted, even after it comes back;
   a batch without a volume reading for the contract starts the count again. The
   first reading is only a baseline. Volume reported between two batches is
   assumed to have traded at the touch; it may have traded at other prices.
3. **The volume-rate heuristic.** With neither, the displayed size the order last
   saw at its price is assumed to trade once every **300 seconds** of market time,
   pro rata between batches (each step at most the freshness window, 60 seconds by
   default). Without a displayed level at the price, nothing is assumed to trade.
   **Warning:** this fallback fills a resting order at the touch within about five
   minutes even when nothing at all traded at its price; on a feed without trades
   or volume, queued fills are an assumption, not evidence. Prefer a feed or
   recording with prints, and treat `source: "rate"` estimates accordingly.

Displayed size that **leaves the level without trading** (the size shrinking by more
than the contracts counted as traded) is treated as cancellations spread evenly
through the queue: the share of them ahead of the order is removed. The estimate
never exceeds what is displayed at the price (plus the account's own earlier
orders), and it is zero once the level clears (the touch moves past the price
without crossing it).

Fractions — half a volume increase, part of a turnover, a share of cancellations —
are **rounded at random**, up with the probability of the fraction, from a hash of
`queue_seed` (0–2,147,483,647, default 0), the attempt's start, the order and the
market time. The same seed and the same feed therefore always give the same fills;
replays and reproducible runs verify byte for byte.

A queued fill trades at the order's **limit**, without slippage or impact, and takes
nothing from the far side's displayed size; the fill records `queued: true` and the
book it saw. It is bounded by the contracts that traded past the queue, and passes
the usual fill-time checks. The far side reaching the limit still fills the order
at once as before (`queued: false`).

Only a **smaller size** keeps an order's place in the queue. A larger size, a new
price (by hand, by a walking step or by a flatten close following the touch), a new
time in force or a new walk sends it to the **back**: it joins again behind what is
displayed then, and behind the account's orders already at that price, as an
exchange treats a replaced order. A bracket exit that grows with its entry's fills
goes to the back too. Orders report the estimate as `queue` (`price`, `ahead`, `earned`,
`displayed`, `joined_at`, `updated_at`, `source` and `simulated: true`) and wait with
`QUEUE` while contracts are ahead.

Limits of the model: displayed size is the whole NBBO across venues, so the queue
ahead is an upper estimate, not an exchange's queue position; it knows no hidden or
reserve size, no pro-rata allocation and no orders that join ahead; prints may lag
or be filtered by the provider; volume increases are halved without knowing their
side; and the 300-second turnover is an assumption, not a measurement. Multi-leg
orders, markets, stops and extended-hours touch stops are unaffected.

### Stop stress

**Stop stress** (`stop_stress_ticks` 0–10, `stop_stress_percent` 101–1000, default
200) adds extra adverse ticks to a **price-triggered** order (a stop, stop-limit,
trailing stop or bracket stop-loss on its option or its own underlying) that is
reached in a **fast market**. Conditional triggers on another symbol, a study or the
clock are not stops. It is off at zero; while it is off nothing is measured or
journaled. Like queue fills it is a program rule.

For each contract the account measures what is normal: exponential averages
(weight 1/8) of the spread width and of the market time between new observations.
After eight valid observations, a new one is **fast** when its spread is at least
`stop_stress_percent` of the average, or it came at most 100/`stop_stress_percent`
of the average gap after the one before (quotes changing that much faster than
normal), each measured against the averages before it. A stop reached on a fast
quote of any of its contracts records `stress_ticks`, and its **first fill attempt**
after the trigger (once any fill latency has passed) slips that many more ticks in
the displayed price's tick tier, on top of the account's slippage and impact; fills
report `stress_ticks`. A stop-limit's limit always caps its price: stress never
moves a fill past it, and with impact the next block's unstressed price decides
whether it may fill. A stop-limit still resting after that attempt drops its stress
(`stress_ticks` returns to zero) and fills later like any other limit. Extended-hours stops, which send a limit at the touch, are unaffected.

Spread and quote rate stand in for what a live fast market does to stops; they are
measured from the supplied quotes only, so a sparse or snapshot feed shows less of
the rate signal than a streaming one.

### Fees

The default remains the flat `fee_per_contract` ($0.65, or `--paper-fee`), opening
or closing, with exercise, assignment and settlement free. Optional `rules.fees`
replaces that fee with an itemized schedule:

| Field | Charge |
| --- | --- |
| `open` | Commission per contract that opens or adds to a position |
| `close` | Commission per contract that reduces a position; a reversal splits opening and closing quantities |
| `leg_cap` | Maximum commission per leg per order, across partial fills and order changes; zero means uncapped |
| `clearing` | Per contract, opening or closing |
| `regulatory` | Per contract, opening or closing (ORF) |
| `index` | Per contract by option root; `SPX` and `SPXW` are separate keys, and a missing root pays zero |
| `exercise` | Per contract exercised, assigned early or delivered at expiry; cash settlement and worthless expiry are free |

Amounts are exact decimal strings between $0 and $1,000 inclusive. The index map
has at most 16 keys, each one to six uppercase letters or digits. Omitted amounts
are zero and an omitted index map is empty. Absent or null `rules.fees` selects
flat fees; `{}` selects a zero-cost itemized schedule. Malformed fields return
400 `INVALID_REQUEST`; out-of-range amounts or invalid roots return 422
`INVALID_RULES`, as other account rules do.

Create or reset an account with `fee_model: "itemized"` for an illustrative
schedule: $1.00 to open, $0 to close, $10 commission cap per leg per order,
$0.10 clearing, $0.02 regulatory, $0.60 on SPX and SPXW, and $5 per exercised or
assigned contract. `fee_model: "flat"` restores the server’s flat fee. Either
preset overrides `rules.fees`; without a preset, custom rules keep their schedule.
These are simulation settings, not a current broker quote. For example, before
the cap, one SPXW contract costs $1.72 to open and $0.72 to close: $2.44 for a
round trip, versus $1.30 at the default flat fee.

Fills record `fees: {commission, clearing, regulatory, index}`, which sums exactly
to `fee`. HTTP fills return null `fees` under flat fees. Exercise, assignment and
physical settlement record a nonzero `fee` on the closure, charged to that option
trade and account fees. Delivered shares themselves remain free to trade.
Buying-power reservations, loss checks, previews, what-if and flatten dry runs
use the schedule, including the commission already paid toward a working order’s
cap. Fees reduce equity, target progress and loss room.

New journal fields are optional: schedule and fill breakdowns are written only
under itemized rules; closure fees only when nonzero. Flat-fee journal bytes stay
unchanged, and old journals load without a schedule. Recovery preserves each
order’s paid commission. The terminal offers flat or itemized fees in New account
and Start a new attempt, describes the active schedule in Rules, and shows
expandable breakdowns on fills and ticket previews.

### Sessions

Orders trade in the sessions `md::trading_session(root, time)` gives each product.
Every product has its **regular** session: 09:30 to 16:15 ET for index roots and the
ETFs whose options trade until then (SPY, QQQ, IWM, DIA, GLD, TLT, the sector SPDRs and
others, `md::is_late_close_underlying`), 16:00 for other equity options, or 13:15 and
13:00 on early-close days. On expiry day, expiring index series stop at 16:00 while
expiring ETF options trade on to 16:15. SPX/SPXW, XSP, VIX/VIXW and RUT/RUTW options also trade in Cboe's **overnight**
(global trading hours) session, 20:15 to 09:25 ET, which belongs to the next trading
date, and in the **curb** session, 16:15 to 17:00 ET after a full day (see
[runtime notes](runtime.md#product-sessions-and-cboe-clocks)). As on Cboe, the overnight and curb
sessions accept limit executions only. Plain DAY/IOC limits trade there; DAY ends with
the session it entered (`DAY_END`). Legacy DAY/GTC triggers and bracket exits still
wait for regular hours. GTC limits and held exits can be placed during a product's
curb and overnight sessions with fresh data, but rest until regular hours.

**EXTO** trades all product sessions of its trading date, ending at the last session
close (normally 17:00 ET for these index products, the regular close on early-close
days). An EXTO placed at 21:00 ET belongs to the following trading date and survives
its 09:25 overnight close. **GTC_EXTO** works every product session until contract
last trade or account auto-close. **GTD** works regular hours until an explicit
`good_till` ISO timestamp with `Z` or a UTC offset, strictly in the future and at
most 366 days from acceptance, measured on market time. It cancels with `GTD_END`
on the first command at or after that instant, before matching. Only GTD takes
`good_till`. All three survive restart, reserve risk like GTC and remain bounded by
the earliest leg's last trade and account cutoff; `day_end` reports that deadline.

EXTO/GTC_EXTO stops and brackets are **simulator-managed protection**, not native
exchange stops: [Cboe does not accept market or stop orders in GTH/curb](https://cdn.cboe.com/resources/membership/24x5-Trading-FAQ.pdf).
Their triggers monitor fresh quotes or valuations in every eligible product session.
A reached stop-limit sends its chosen limit and rests if the gap is past it; this is
the safer default for controlling price. A reached stop-market sends an IOC limit
at the current touch (ask to buy, bid to sell), using only remaining displayed size,
without slippage or synthetic impact depth outside regular hours. Combo stops use
each leg's touch and the smallest available whole-unit size. A standalone stop's
remainder cancels; bracket stops re-arm the protected remainder as before. In regular
hours the normal fill model applies. Bracket exits of EXTO/GTC_EXTO entries use
GTC_EXTO, so they continue protecting after the entry's trading date ends. GTD
bracket exits use the existing GTC/IOC policy and outlive the entry deadline.

Unconditional market entry orders are still refused outside regular hours with
`LIMIT_ONLY`; closed sessions cannot trigger or fill. Plain market flatten remains
regular-only; `type: "limit"` flatten works overnight and curb (see below).
AM-settled series stop trading at the regular close of the business day before their
expiry, so no curb or overnight session trades them then (`SESSION_CLOSED`); PM
series trade the overnight session of their expiry date. Contract expiry can be
earlier than a session end and takes precedence (e.g. PM expiry 16:00). Boundaries
process on the first command at/after them, before possible fills.

GTC limits survive session ends, daily rollover and journal recovery. They wait
outside the regular session, even on products with overnight trading. They reserve
risk and buying power on every day they rest, just like working DAY limits. Checks
run again before a fill and on limit changes: a failed check cancels with
`RISK_CHANGED`; missing or stale data waits. They cancel with `EXPIRED` at the
earliest leg's last trade, or with `EXPIRY_CUTOFF` at the account's earlier auto-close
deadline, even when no position is held; their `day_end` is that deadline. Evaluation
decisions and account resets cancel them too.
When none of a product's sessions is open (between sessions, weekends or holidays),
new orders, GTC included, are refused with `SESSION_CLOSED`; GTC orders placed
earlier remain working through the gap and wait for regular hours. Holidays and
early closes follow NYSE's rules from 2022 on, and the overnight session runs into
most holidays until 11:30 ET, as Cboe schedules it (see the runtime notes).

The engine also compares each underlying's market-data time with wall-clock time. A
healthy feed shows the market as it was the provider's stated delay ago, stopping at
the end of a session. When that moment is in one of the product's sessions, or data
stopped inside a session that should since have ended, a lag **greater than
`max_quote_age`** beyond it (for a delayed feed, the larger of that and three
minutes, since delayed snapshots can trail their stated delay by a minute or two)
rejects new orders with `FEED_STALLED`, including the lag
behind the wall clock in the message (for example, "SPX quotes are 10h 20m behind the
market; the feed appears to have stalled"). A feed that rightly shows a closed market
returns `SESSION_CLOSED`, naming why (between sessions, a weekend or a holiday): for
the first 15 minutes of a session, a healthy 15-minute delayed feed still shows the
market before it. The simulation clock stays at market time. Resting orders do not
fill from stale data and are not cancelled merely because the feed stalls; they
remain subject to normal market-time DAY/expiry, risk and explicit cancellation rules.

Snapshot providers (Cboe, Massive, ThetaData and the demo market) send only the
quotes that changed, and end each poll of an underlying with `md::SnapshotComplete`.
While an underlying's last complete snapshot passes that stall check, the engine
offers every account its quotes, valuations and share price as current: valuations
and the share price at the market time, option quotes as described next. So a
quote that sits unchanged, such as a quiet far wing's, stays current, and so does an
underlying whose source runs a minute or two behind the others', as Cboe's quote pages
do. Past the tolerance they are no longer offered, age, and new orders reject; after
a gap, such as overnight, the account waits for the next complete snapshot.
Streaming feeds mark no snapshots, so their quotes keep the time they last changed.

With a [fallback feed](runtime.md#feed-failover), these checks apply to whichever feed
supplies the underlying. The backup's quotes keep their own market times: a delayed
backup behind a real-time primary shows quotes the freshness window refuses, so orders
wait rather than fill against data minutes old. Each fill names the feed whose quote
priced it in `feed` (null from a single feed), and the journal keeps it.

Current is not new. An option quote carries the market time its own underlying's
latest snapshot arrived (or its own later timestamp), not a newer time another
underlying's data has moved the clock to. So fill latency waits for that
underlying's next snapshot: a QQQ poll cannot release an SPX order on the SPX quote it
was sent against. Only once that time falls `max_quote_age` behind the market time
does the quote follow the clock, which keeps it current until its own feed stalls.
Fills record the quote time they used. Replays, backtests and the demo market apply
every underlying's snapshot of one market time together (see
[reproducible runs](architecture.md#reproducible-runs)), so triggers, fills, plan
rules, playbooks and equity samples never see an instant half applied. A live polling
feed's underlyings arrive apart and each is applied as it comes, so there a rule
decided between two polls, such as the expiry cutoff or an intraday floor, values the
underlying still waiting at its last snapshot.

**Circuit breakers** halt the whole market as the exchanges' market-wide rule does
(NYSE Rule 7.12, which the options exchanges follow), measured on the S&P 500 (SPX, or
SPY when SPX is not subscribed) against its previous close: Cboe's published
previous-day close (`md::UnderlyingClose`), a generated scenario's previous close, or
the closing print the engine saw. In the
regular session a fall of 7% (level 1) or 13% (level 2) halts trading for 15 minutes,
each once a day and only until 35 minutes before the close (15:25 ET, 12:25 on an
early-close day); a fall of 20% (level 3) halts it for the rest of the day. While a halt
covers an underlying's market time, new orders reject with `MARKET_HALTED`, whose
message gives the fall and when trading resumes, status reports it, and no resting
order, trigger or bracket exit fills. The terminal shows the fall, the level and the
resume time in a banner while the latest market time is inside a recorded halt;
level 3 says trading is halted for the rest of the day. Authored scenario halts
identify the scenario and give the resume time in New York time, for example
"Trading is halted market-wide by the scenario; it resumes at 10:30 ET". A halt
ending at or after the regular close (including an early close) says "for the rest
of the day"; the terminal uses the same calendar-aware decision. SPY is named when it stands
in for the index. `server::circuit_breaker` holds the rule. Halts of a single stock or
ETF are not modelled.

With a paper journal, the engine keeps the day's breaker state (the level tripped,
the previous close and the halts) in `market-halts.json` beside the main journal,
replaced atomically on each change, so a restart during a halt keeps its original end
and cannot trip the same level again that day. A new trading day resets the level and
looks up its own previous close; the last day's halts stay listed until another day
trips one, and prints from an earlier trading day cannot reset the breaker. Replays
and engines without a journal keep the state in memory. A missing file starts empty;
an unreadable or invalid file, or a failed write, is reported in status and the
daemon log, and the engine carries on without it.

## Conditional and bracket orders

A single-leg order, or a closing combo, may carry a **trigger** `{source, direction, level}`. It is accepted with the
normal pre-trade checks, then rests as `Armed`: open, cancellable, reserving exposure and
buying power, but never matched. Option triggers compare the order's executable side
(ask for buys, bid for sells) from a fresh book; underlying triggers compare spot from
the contract's fresh valuation. Levels are inclusive (`at_or_below`, `at_or_above`), and
missing or stale data never triggers. Triggers are checked after each market batch's
resting orders match, and at submission, only during the contract's regular session.
A reached order is activated: entries rerun every pre-trade check, while bracket exits
need only an executable book. Stale data or a closed session keeps it armed; any other
failure cancels it with `RISK_CHANGED`. It then trades like any order (market orders
IOC). Armed orders last until the nearest contract’s last trade or auto-close, which
their `day_end` reports (EXTO/GTD instead keep their earlier TIF deadline). Once triggered, a DAY order lasts the session it activated in,
like any DAY order; a GTC order stays good until that deadline.

An option or combo trigger can read another **reference** than the executable side
(`reference: "bid_ask"`, the default): `"mid"` reads the mid of a fresh two-sided quote,
and `"mark"` the mark (the mid, or half the ask when nobody bids). A combo sums its
legs' mids or marks, signed and weighted by ratio, as it does their far sides.
Underlying triggers read spot and take only the default. So a stop on a wide quote need
not fire on a bid that dips for a moment.

A trigger with a **trail** `{unit, value}` is a **trailing stop**: while it is armed and
its contract is in the regular session, whenever the value it reads less the trail
(plus the trail, for `at_or_above`) passes its level, the level moves there, in whole
cents, and it never moves back. The trail is a dollar `amount`, a `percent` of the
value read (above 0 and below 100), or 1 to 1,000 `ticks`: the tier tick at that value,
or the combo tick; underlying triggers trail by amount or percent only. The submitted
level is where it starts; the first fresh reading moves it if that is tighter, so a
sell stop trailing 0.50 behind a 4.00 bid starts at 3.50 at the latest. A stop-limit's
limit moves with its level: the move is shortened to keep the limit on its tick, so
the two keep their distance, and the level only waits for the next whole tick. Moves
are journaled with event `order_trailed`, orders report the level where it is now, and
`PUT /api/orders/{id}` can still set it; it trails on from there. Bracket stops and
held exits take a trail and a reference too. `reference` and `trail` apply only to
option, combo and own-underlying triggers; a trigger on another underlying, a study
or the clock refuses either with 400. EXTO/GTC_EXTO stops keep activating in extended
sessions, but trail only in the regular session.

A trigger can also watch something other than the order's own market, so a rule such as
"buy if VIX reaches 20" or "close at 15:30" runs unattended on any account:

| Trigger | Compares |
| --- | --- |
| `{"source": "underlying", "symbol": "VIX", "direction": "at_or_above", "level": "20"}` | Another underlying's price |
| `{"source": "study", "study": "iv30", "direction": "at_or_below", "level": "15"}` | The order's underlying's 30-day implied volatility, in vol points; `symbol` names another underlying |
| `{"source": "study", "study": "iv7", "direction": "at_or_above", "level": "20"}` | Its 7-day at-the-money implied volatility, in vol points |
| `{"source": "study", "study": "term_ratio", "direction": "at_or_above", "level": "1"}` | Its 9-day over 30-day implied volatility (above one is inverted) |
| `{"source": "time", "at": "15:30"}` | The New York time of day: from that minute on, or up to it with `"direction": "at_or_below"` |

Prices and studies come from the same analytics that value the contracts: the
underlying's spot (quoted, or implied by parity), the model-free 30-day volatility
(the 30-day at-the-money volatility when the strip cannot give one), the 7-day
at-the-money volatility and the ratio of the 9-day and 30-day model-free volatilities, as
on the Volatility page. Demo and scenario days cover the 7-day maturity for `iv7`,
but `iv30` and `term_ratio` are available only while usable listed expiries bracket
30 days (or land exactly on it); the analytics do not extrapolate beyond the last
expiry. This can be the ETFs' next-month PM monthly while the index AM monthly
falls just short of 30 days. `iv30` needs usable model-free or at-the-money inputs;
`term_ratio` needs both the 9-day and 30-day model-free volatilities. The server
supplies them with each market batch to the accounts whose armed orders watch them, and
the account keeps the latest of each; one older than the valuation age, or missing
because the feed does not carry that underlying, never triggers. Like any trigger they
are checked after each market batch and at submission, in the order's contract's regular
session, so a time trigger fires at the first batch at or after its minute (in a replay,
the first step that reaches it) and an order placed after its time is reached at once. A
GTC order waiting for 10:00 with `at_or_below` therefore activates the next morning.
They read only recorded inputs, so a replay verifies like any other run. Bracket exits
keep watching their own option, net or underlying, and a time trigger has no level to
change: cancel and place it again. A trigger watches a fixed quantity: unlike a bracket
exit, a conditional closing order is not resized when the position changes, so cancel
it when you close the position another way.

A limit order with a trigger is a **stop-limit**. Its limit is set against its stop, not
against today's market, so when it is accepted or changed the price band is measured
around an option (or combo) trigger's level instead of the mid: a protective sell at
2.90 with its stop at bid 3.00 is accepted while the market is at 4.10, and one at 1.00
is not. An underlying trigger's limit is still banded around the mid. Once triggered,
the band applies only to the price it fills at, so a market that gaps through the
limit leaves the order working at its limit rather than cancelling it: it never fills
worse.

A **bracket** `{stop_loss, take_profit}` on an entry creates exits as the entry fills.
Each exit takes a trigger (a stop: market IOC when reached), a limit price (a resting
take-profit), or both (a stop-limit: armed until its trigger is reached, then a GTC
limit at its price, which a gap through it leaves waiting rather than filled at a worse
price). Exits take the opposite side and are sized to the entry's
filled quantity; later entry fills grow them. Client IDs are the entry's with `:stop` or
`:target`. EXTO/GTC_EXTO entries give both exits GTC_EXTO. Otherwise a stop is market IOC, and a take-profit or a triggered stop-limit a GTC limit
on the tier tick; all last, and report
in `day_end`, until the nearest contract's last trade or auto-close, and a stop already
reached when the entry fills fires at once. The two exits are linked: when one fills
completely it cancels the other with `OCO_FILLED`, unless it was resized to leave
part of the position protected by that sibling. A partial fill also leaves the other
in place, shrunk to what this bracket still protects even when another entry holds
the same contract. A stop that fills only in part, because the
bid shows fewer contracts than it sells or other orders have used them, does not cancel
its remainder: it re-arms for it (`order_rearmed`) and fires again on the next quote that
still reaches its level, so a thin book never leaves part of the position bare. If the
entry still has an unfilled remainder, the first exit fill cancels it with `OCO_FILLED`
so it cannot reopen after protection has fired. Exits never exceed the position they protect: they shrink when it shrinks
and are cancelled with `POSITION_CLOSED` once it is flat, so they never open a
position. Because they only reduce risk, they execute like system orders, without the
price band or loss projection, and good-until-expiry exits wait for the next regular
session. A bracket pair counts once in reachable exposure and buying power (fees only);
exits never count against buy-only sells, so a manual close is always possible. Plain
stop sells do count, but a manual close supersedes them on a buy-only plan (see
`buy_only` under plan rules).

A contract already held, opened without a bracket, takes the same pair: submit the
order that closes it (`symbol` and the closing side, `quantity` at most the holding
less what other working closing orders claim) with `exits_only: true` and a `bracket`.
No entry fill is generated. The order itself is the take-profit, or the stop when
there is no target, and its terms must match that exit: a GTC, EXTO, GTC_EXTO or GTD
limit at the target (or at a stop-limit's price, with its trigger), or a market IOC
with the stop's trigger. Like held spread exits, EXTO/GTC_EXTO extend both exits to
every product session, and GTD gives both its explicit deadline; triggered market
exits also take EXTO, GTC_EXTO and GTD.
The exits are those a bracket creates (`oco`, `stop_loss_order` and
`take_profit_order` name them, client IDs `:stop` for the second), linked and
re-armed alike, and they count once in exposure and buying power. They take an
exit's checks, not an entry's, so they are accepted under the kill switch and in
any session, waiting outside the regular one unless EXTO/GTC_EXTO. Positions'
**Exits…** dialog sets, changes and cancels them; `POST /api/orders/cancel` with
`orders` cancels both in one transaction.

### Order chains: one-cancels-other and one-triggers-other

Any order, single or multi-leg, may carry two chained orders, each an order body
without `client_order_id` (it takes the order's own with a suffix) or `exits_only`:

- `oco`, one-cancels-other: accepted with the order as `<client_order_id>:oco`, both or
  neither. If it is refused, the order is recorded refused for its reason (the message
  says so) and it is not recorded at all. The two are linked through `oco`, and the
  first fill of either cancels the other with `OCO_FILLED`, so a breakout entry above
  the market and a dip entry below it cannot both open. One that fills on acceptance
  cancels the other at once. Unlike a bracket's exits, each reserves exposure and
  buying power in full while both are open, since they may differ.
- `then`, one-triggers-other: placed as `<client_order_id>:then` once the order fills
  completely, at that market time, by the same actor and with every check a new order
  takes then; a refusal is recorded as its own rejected order. An order that ends any
  other way (cancelled, expired, or with only part filled) places nothing. Staged
  scale-ins and a target placed only after the whole entry fills are chains.

A chained order may carry its own `then`, and a `then` its own `oco` (an order that,
once filled, places a pair), up to four orders in all; an `oco` order takes no `oco` of
its own, and held exits take neither. Orders report the pending `then` in the body's
shape, including `good_till`, `walk` and `group` (null when unused), `chained_order`
once it is placed and `chained_from` on the order placed. An identical retry still
answers with the first order. Bodies breaking these bounds are
400 `INVALID_REQUEST`, and the reducer refuses them with `INVALID_ORDER`.

## Multi-leg orders

An order with **legs** trades two to four contracts together: each leg names a
registered contract, a side and a ratio from 1 to 10, all on one underlying (expiries may
differ, so calendars and diagonals are allowed). A roll may take up to eight legs, so
an iron condor rolls whole in one order: past four legs, every leg but four must close
held contracts in full for its units, or the order is refused (`INVALID_ORDER`, with
the opening legs counted against four). The order has no symbol or side;
its `quantity` counts units, and `limit_price` is the net per unit, positive
for a debit paid at most and negative for a credit received at least (zero is even).
Plain market orders are IOC; triggered markets also take EXTO/GTC_EXTO/GTD. The net must be a multiple of the smallest lower-tier
tick among the legs ($0.05 for SPX-class roots, $0.01 for XSP and equities).

Checks run per leg where they apply: registration, expiry, the session, a fresh
executable book, and `units * ratio` within `max_order_contracts`. The net price must lie
in the price band around the net mid (a closing stop-limit's, around its combo trigger
level), where the band is as wide as the one for the legs'
gross premium (`max(absolute, relative * sum of ratio * mid)`). Buy-only plans reject
opening multi-leg orders (`BUY_ONLY`); inside the pre-expiry cutoff only closing
orders are accepted (`EXPIRY_CUTOFF`), and daily loss, exposure and buying power apply to the whole order.

Without the inside-fill model, a multi-leg order fills **all legs together**, in ratio, when the net at the slipped
far sides (asks plus slippage for bought legs, bids less slippage for sold legs) is
at or below its limit. Every leg takes its full slippage; an order whose net would
exceed the limit waits, rather than allocating a partial slip among its legs.
The net may improve on the limit. Without impact, units are bounded by every
leg's remaining displayed size. With impact, each leg uses the simulated blocks above. Multi-leg orders match after single-leg orders on the
same books, in acceptance order, and only on quotes newer than their acceptance (except
at submission). Each leg's fill is recorded under the order's ID; `filled_notional` and
the average fill are the net per unit. The projected fill is checked for daily loss and,
when any leg opens contracts, buying power, exactly like a single-leg fill. A working
multi-leg order counts as one pending exposure (its legs summed), and it is cancelled at
its session end or its earliest leg's last trade or auto-close deadline, whichever comes
first, like any DAY order. GTC limits instead last until that deadline.

A multi-leg **entry bracket** reverses every entry leg, keeping its ratios. Entry
brackets require opening legs; a roll or close cannot create a bracket on reversed
legs it no longer holds. Exits are created on the first fill and grow with partial
fills. Each exit fills all its legs together, and the pair counts once in risk and
buying power. It remains reducing under the kill switch and must keep a defined-risk
plan's shorts covered. Manual closes shrink or cancel the exits.

Take-profit limits use the closing order's signed net: positive for the maximum
buy-back debit after a credit entry, negative for the minimum credit after a debit
entry. The ticket expresses this as a percentage of the entry net: 50% buys back
half a credit, 150% receives one and a half times a debit. Prices snap to the combo
tick; an exit price off it rejects with `INVALID_TICK`, as a single contract's does off
its tier tick. This fixes a price when submitted; it does not reprice with later entry fills.
A target waits until the entire slipped closing net fits its limit.

A stop has `source: "combo"` or `"underlying"`. Combo levels compare the sum of the
closing legs' displayed asks for buys minus bids for sells, weighted by ratio,
without slippage. Negative levels are allowed. A leg whose fresh quote shows only an
ask, as a far option nobody bids for does, counts at that ask when the exit buys it
back, and at zero when the exit sells it. The exit trades it that way too: it buys
at the ask within its displayed size, and gives a long away at 0.00, without a size
limit and with the usual fee. So a worthless wing cannot hold a bracket's or held
spread's stop or target, or a flatten, back; a plain triggered combo, which is not an exit, still
needs a two-sided quote on every leg. Underlying levels must be positive
and read the first leg's fresh valuation. On reaching the inclusive direction, the
stop sends a closing market IOC combo with normal slippage and displayed-size
limits; a stop-limit (a trigger and a signed net `limit_price`) rests instead as a
closing GTC combo limit at that net, on the combo tick, and fills only at it or better. A partial stop fill re-arms the stop for the units still held and shrinks the
target to them, as for a single contract. Missing data never triggers. Plain triggered
combos are accepted only when every leg reduces holdings, including other manual
closing orders' claims. Opening combos cannot carry triggers.

To attach exits to a held spread, submit its **closing** legs with `exits_only: true`
and `bracket`. No entry fill is generated. The submitted order is the take-profit
(or the stop if there is no target); its `type`, `limit_price` or `trigger` must match
that exit. EXTO/GTC_EXTO extend held exits to every product session; GTD gives
both held exits its explicit deadline. Otherwise use GTC for its limit (a stop-limit's included, with its trigger) or IOC for
its triggered market order. Both exits last until the nearest leg's last trade or
auto-close, bounded by the earlier EXTO/GTD deadline when used. They can be attached in any
session; EXTO/GTC_EXTO held exits also work overnight and curb. Every leg must oppose a held
position, and `quantity * ratio` must fit the holding. For example:

```json
{
  "client_order_id": "held-put-exits",
  "legs": [
    {"symbol": "SPXW  261022P04900000", "side": "buy", "ratio": 1},
    {"symbol": "SPXW  261022P04890000", "side": "sell", "ratio": 1}
  ],
  "quantity": 1, "type": "limit", "time_in_force": "gtc", "limit_price": "0.40",
  "exits_only": true,
  "bracket": {
    "take_profit": {"limit_price": "0.40"},
    "stop_loss": {"trigger": {"source": "combo", "direction": "at_or_above", "level": "2.00"}}
  }
}
```

The response's `oco`, `stop_loss_order` and `take_profit_order` identify the exits.
Change their levels with `PUT /api/orders/{id}`; cancel both IDs to remove the pair.
The held-strategy row's **Exits…** dialog sets, changes and cancels them. Its Roll
dialog can select the put or call vertical of a condor or iron butterfly, a later
expiry and new strikes. The resulting four-leg order closes that side and opens
the new vertical together; the other side stays held. After a full side roll, the
held view shows the two remaining verticals with their current position cost bases,
not the roll’s combined closing and opening net.

## Changing, cancelling and flattening

A resting order changes in place (`modify`): a resting limit order (including EXTO/GTC_EXTO/GTD and a limit flatten), an armed order or a
bracket exit. It keeps its ID, its fills and its place among equal prices. The new
quantity counts filled contracts too and must exceed them; the limit price applies
to limit orders (a multi-leg order's signed net); the trigger level to armed orders
with a trigger. The changed order takes every pre-trade check a new one would, with
its own reservation released first, and a failure leaves it exactly as it was. A
limit that becomes marketable trades against the cached fresh quote, subject to
any configured fill latency, and an armed order whose new level is reached activates in an eligible session. Bracket
exits change their level or take-profit price (a signed net on the combo tick
for spreads, positive on the tier tick for a single contract), and their size: at
most the position they protect, so a target can take part off while the stop keeps
protecting the rest (a smaller exit that fills completely leaves the other working,
shrunk to what is still held, even if either exit had already partly filled). They
follow the position down as it shrinks, and a later fill of their entry grows them
back to its filled size. A flatten's reduce-only closes change only their limit
price; their size follows the position. A resting DAY or GTC limit
entry changes its time in force between DAY and GTC (`time_in_force`): a DAY order
then ends with the current session, a GTC one at expiry; an armed order keeps its
expiry until it triggers. EXTO, GTC_EXTO and GTD orders keep their time in force:
cancel and submit again to change it or `good_till`. Bracket exits are good until expiry and keep it. The engine applies a new order's feed gate
(`FEED_STALLED`) before a change, because a change can trade.

PUT and change preview reject a quantity below 1 with 400 `INVALID_REQUEST` and
record nothing. A positive quantity at or below the filled quantity remains a
422 `INVALID_ORDER` on PUT, recorded with the requested and filled quantities.

Changes with valid request bodies stay on open orders in `Order::changes`, oldest
first, applied or refused: their market time and actor, the terms requested (each left empty
kept), the terms the order had then, and the refusal's decision with its numbers.
A refused change leaves the order's terms as they were. `Order::ended_at` is when an
order stopped working: the fill that completed it, its cancellation (whatever the
reason: `USER_CANCEL`, `DAY_END`, `EXPIRED`, `RISK_CHANGED` and the rest) or its
rejection, zero while it is open. Both are journaled only on orders that have them, so
other orders keep their bytes, and orders from older journals load without them.

The snapshot also says what keeps each open order from filling now
(`TradingSnapshot::waiting`, derived from the account's books, sessions and clocks as
matching reads them, never journaled):

| Code | The order waits for |
| --- | --- |
| `TRIGGER` | Its trigger: the reference (ask, bid, the closing legs' net or the underlying), the level and the value now |
| `REGULAR_SESSION` | The regular session: legacy armed orders, GTC/GTD orders and their bracket exits wait for it |
| `SESSION_CLOSED` | An EXTO/GTC_EXTO order waits between its product sessions |
| `INVALID_QUOTE`, `STALE_QUOTE` | A two-sided quote with sizes, or a fresh one, on a leg; a one-sided book supplies no liquidity |
| `FILL_LATENCY` | A quote stamped at or after its acceptance (or activation) plus the account's fill latency, with the time |
| `NEWER_QUOTE` | A quote newer than its acceptance: resting orders take only those |
| `LIMIT` | The market: the far side (or a multi-leg order's net at the far sides) is worse than its limit, with both prices |
| `DISPLAYED_SIZE` | A new quote: paper orders used the displayed size at this one |
| `STALE_DATA` | Fresh marks on held positions and the valuations risk needs |

An order with nothing visible holding it back has no entry.

`cancel_all` cancels every open order, armed ones and bracket exits included, or
only one underlying's. `cancel_orders` cancels the listed orders still open in one
transaction, such as both exits of a pair, so neither is left working alone.
`close_positions` flattens the account or one underlying and
works until it is flat. It cancels the open orders in scope but the bracket exits,
then closes each unexpired position in scope with market orders under the account's
fill model:

- **Spreads close together.** Each short and the long that covers it (one of its type
  on the same underlying that expires with it or later, as `defined_risk` counts
  cover) close as one two-leg order: the short is bought back and the long sold in the
  same fill, so no short is ever left naked, with or without fill latency. Per type,
  the latest-expiring shorts take their cover first, from the nearest expiry and then
  the nearest strike, which covers the most shorts. The shorts and longs left over
  close alone.
- **Remainders keep working.** By default the closes are reduce-only market DAY orders
  (`reduce_only` on the order). What a thin quote or fill latency leaves unfilled
  works on later quotes until it fills, its session ends (`DAY_END`), the trader
  cancels it or the position closes otherwise. Like bracket exits, they shrink with
  the position and are cancelled with `POSITION_CLOSED` once it is flat, so they never
  open one; they keep working under the kill switch and personal guardrails.
- **Large positions split.** A close larger than `max_order_contracts` goes as several
  orders, none larger than the limit.
- **Exits stay until flat.** Bracket exits keep protecting whatever is still open,
  sized to it, and are cancelled with `POSITION_CLOSED` once it is flat. A pending
  close the account placed itself (an auto-close or liquidation) keeps working too.
- **Thin and missing quotes wait.** A stale, missing or invalid quote only makes a
  close wait for a usable one; it is not refused. A short nobody offers below the
  ask (a 0.00/0.05 quote with no bid) is bought back at its ask within the displayed
  size, and a long nobody bids for is given away at 0.00 with the usual fee, as a
  combo exit does, so a worthless wing cannot hold the flatten back.

Being reduce-only, the closes execute like bracket exits and the account's own
closes: without the price band, loss projection, exposure or buying-power checks,
and without needing fresh marks on every other position, so an expired position
awaiting its settlement no longer blocks them. They are the trader's own orders
(client IDs `openport-close-{version}-{n}`, numbered past any client ID the account
has already used). Expired positions wait for settlement, and so does a long that
covers a short that has expired. Shares in scope close at the underlying's
fresh price in the stock market's regular session. The response lists every position
in scope still open (`residuals`): the contracts still held, those its closes are
still working, and why the rest are not being closed (a refusal,
`AWAITING_SETTLEMENT`).

Pass `{"type":"limit","limit_ticks":0}` to `POST /api/positions/close` (or its
`/preview` and replay mirrors) to flatten with reduce-only EXTO limits in any open
product session. `limit_ticks` defaults to 0 and must be an integer from 0 to 10;
it is forbidden with market flatten. The offset is adverse from each leg's touch:
ask plus ticks to buy, bid minus ticks to sell, rounded outward to a valid price tick
(and floored at the minimum positive tick for a single sell). A combo's net limit
sums those signed leg prices. Limits cap the normal fill model; they reprice on each
fresh executable quote, recording `order_repriced`, and work until that trading
date's last session ends. Missing quotes leave them working until a price is available.
Manual limit modification disables automatic repricing (`limit_ticks` becomes null);
quantity cannot be changed on a reduce-only close. All combo, split, residual and
kept-exit behavior above also applies. Shares still need their regular session.

Before it cancels anything, a flatten checks what each close needs whatever the price:
the underlying's feed (`FEED_STALLED`, `MARKET_HALTED`), and a session that takes its order type (market gets `LIMIT_ONLY`
in overnight/curb; either gets `SESSION_CLOSED` between sessions) or, for shares, the
stock market's regular session and a fresh price (`SESSION_CLOSED`, `STALE_QUOTE`).
An underlying where every close is refused keeps its open orders, exits included,
and its closing orders are recorded as rejected with the reason; shares that cannot
close stay, listed in the response's `kept_stocks`. When nothing in scope can close
(market flatten overnight, or only shares after the 16:00 stock
close), the flatten itself is refused with the first such reason, its `scope` the
underlying, and changes nothing.

A flatten has a dry run, `POST /api/positions/close/preview`, with the same optional
`underlying`. It runs the flatten on a private copy of the account at the current quotes,
as a preview does: it writes no journal, allocates no IDs and takes no displayed size. It
answers 200 with `decision` (`ok`, or the reason the flatten would be refused, with
`reason`), the open orders it would cancel (`cancelled_orders`), its closing `orders`
without IDs (symbol, underlying, side, legs, quantity, filled quantity, average fill price,
status and reason: `working` for a close that waits for fill latency, `rejected` with the
reason for a refused one), their `fills` now, the shares it would close (`stock_fills`) and
keep (`kept_stocks`), what would still be held in scope (`remaining` contracts and
`remaining_shares`), and the account before and after it (`current` and `after`, as in a
[what-if](#what-if)). A spread’s close lists its legs with a signed net average, as
orders do; its symbol and side are null. A single-leg close has null legs. A close waiting for a later quote is shown working, so the account
after it still holds that position. Flatten in the terminal shows the dry run before
the flatten is confirmed.

### Disposing of worthless positions

A short nobody bids for still has an ask: an ordinary closing buy takes it (see
[quote matching](#orders-and-quote-matching)). A long nobody bids for cannot be sold,
so `abandon(symbol, time)` gives it up: the whole long leaves the account at 0.00,
without a fee, as an `Abandon` closure, and the trade history closes it there. It
needs a long that cannot be sold now: its fresh quote shows only an ask, or it has
expired and waits for its settlement (`INVALID_ORDER` "Someone bids for it" while a
two-sided quote is fresh; a stale or missing quote is refused with its reason).
Abandoning an expired position that would have settled in the money forfeits that
value. The long's bracket exits and the orders selling it are cancelled with
`POSITION_CLOSED`, since a sell would now open a short. It is allowed after a pass or failure, under the kill
switch and personal guardrails, may not leave a defined-risk plan's short uncovered
(`DEFINED_RISK`), and with `buying_power` a short it covered must still fit. The
portfolio marks such positions `no_bid`. An expired position awaiting settlement
blocks other orders (`STALE_QUOTE`, "All held positions need fresh marks"), so
abandoning a worthless one lets trading resume before an AM settlement arrives.

## Accounting, marks and equity

Let `q` be signed contracts, `M = 100`, `p` fill premium per unit and `f` the fill fee:

```text
cash change = -q * M * p - f
```

Basis is **signed remaining acquisition notional**, excluding fees. Same-direction
adds add signed notional. Opposite fills allocate the existing signed basis in
proportion to the closed contracts, round nearest micro-dollar with ties away,
and keep the rounding residue in the remaining position. Gross realised P&L is
signed close proceeds minus allocated signed basis. A final close releases the
entire residue. A reversal closes first and opens the excess at the fill price.
Closed positions disappear; cumulative realised P&L and fees remain in the account.
Position realised/fees describe the current open lifecycle: after a reversal, the new
position has realised nothing and carries the excess's share of the reversing fill's
fee, as the trades view splits it. Beside them, each position in `GET /api/portfolio`
names its round trip in progress (`trade`, the trades view's ID) and gives the
contract's `lifetime` this attempt: how many `round_trips` it has had, counting the
open one, and their `realised` P&L, `fees` and `net` together. Delivered shares do the
same, with dividends in their realised P&L.

```text
market value = q * M * mark
unrealised P&L = market value - signed basis
equity = cash + sum(market value)
equity = initial cash + realised P&L + unrealised P&L - fees
```

Fresh valid two-sided books establish a midpoint mark, rounded to the nearest
micro-dollar (a half micro-dollar rounds up). A quote with an ask and no bid, as a far
option nobody bids for shows (0.00 / 0.05), marks its position halfway to the ask and
keeps it current, though only a two-sided quote trades. An invalid or stale observation
never replaces the last valid mark with zero. Snapshots retain mark time/age, the last-mark
equity estimate and `valuation_complete = false`. Execution fails closed if any
held position lacks a fresh mark. Freshness (`max_quote_age`, and `max_valuation_age`
for Greeks) is measured at the market time while the contract's market is open, and at
its last session's end while it is closed, so a position in a closed market keeps its
closing mark: an SPY position held overnight does not block SPX trading in the
overnight session, and the day rolls over on it. An AM series past its last trade
keeps its closing mark and valuation until settlement time and does not block the
curb or overnight sessions. Quotes and valuations that arrive for such a series at
or after its last trade, such as the 0/0 quote a feed shows once it stops trading,
do not replace that close. Awaiting-settlement positions retain their last
mark and are always incomplete. If no mark exists, market value/unrealised are null;
the equity field is only a partial estimate and must be read with its completeness
flag. Normal session fills always establish a mark first.

## P&L by Greek

The snapshot explains the day's P&L by the Greeks (`attribution`, and
`attributions` per contract held or traded today). Each stretch a position is held
at one size starts from its mark and valuation (`Reference`): at a fill, at the day's
rollover, or when the position opens. Its change in value to the mark now is split
by the Greeks at its start, per unit times quantity times the multiplier:

```text
delta = delta0 * (S - S0)            gamma = 0.5 * gamma0 * (S - S0)^2
vega  = vega0 * (iv - iv0) * 100     theta = theta0 * (T0 - T) * 365
other = (mark - mark0) - delta - gamma - vega - theta
```

with spot `S`, the strike's smile volatility `iv` and years to expiry `T` from the
valuations at either end. Without valid valuations at both ends it is all `other`. A
fill ends the stretch at the mark and starts one at the new size; the spread paid
against the mark and the fee are `costs`. Settlement ends the last stretch at
intrinsic value, at expiry, with the settlement as the underlying's price and the
volatility unchanged. So the parts add up to the day's P&L exactly (up to floating
point): equity less the day's baseline. The HTTP API gives the parts and the total to
the micro-dollar, as the account's money is, and still adds up exactly: each part is
rounded down or up, the parts that rounding down cuts most taking the micro-dollars it
leaves over (largest remainder). A stretch whose mark moved without valid valuations at
both ends cannot be split, so all of it is `other`; `fallback: true` says some of the
parts came from such a stretch, on the account's, a contract's and a round trip's
parts alike. Rollover records the finished day's parts in
its `EvaluationDay` and starts every stretch again from the closing marks; a reset
starts afresh. `GET /api/portfolio` gives each held position's parts, and lists the
contracts and shares traded today and no longer held under `closed` (`symbol`,
`kind` `option` or `shares`, `underlying` and `attribution`), so the rows still add up
to the account's parts after a close.

Each round trip also keeps its own parts over its whole life, across days: the
stretches it was held for and the costs of its fills, the open stretch to the marks
now included, which add up to its P&L (realised less fees, plus the rest at its mark).
A reversing fill books its closing part to the round trip it ends and the rest to the
one it opens. Trades and share trades carry them as `attribution`; a round trip open
from before an upgrade has none (null). Stretches, finished parts and each round
trip's are state, journaled and recovered; a position held from before an upgrade
joins the day's parts at its next fill or rollover. The parts
are analytic dollars, not accounting: vega is per volatility point and theta per
calendar day, as in the valuations.

## Risk and kill switch

Valuations come from the caller's coherent strike-smile frame: spot delta/gamma,
vega per vol point, theta per calendar day, spot S, forward F, discount D, years T
and **the smile IV actually used for the Greeks**. All must be finite; S/F/D/IV must
be positive and T nonnegative. When a held option's strike has no smile IV, as a far
wing without a bid can leave it, the engine values it at the option's own ask IV (or bid
IV) on the expiry's forward instead. Quote and valuation max age default to 60
market-time seconds, configured independently. Missing or stale portfolio/pending-order values
block new trading. Expired positions do not pretend to have live Greeks.

Per-underlying and aggregate exposure:

```text
dollar delta          = sum(q * M * delta * S)
dollar gamma per 1%    = sum(q * M * gamma * S^2 * 0.01)
vega per vol point    = sum(q * M * vega)
theta per day         = sum(q * M * theta)
```

Gamma here measures dollar hedge change, not second-order scenario P&L. No dealer
sign convention or exposure time floor is used.

For delta and vega independently, start at current position exposure and add each
open order's remaining signed exposure to either the positive or negative endpoint:

```text
reachable low  = current + sum(min(0, order exposure))
reachable high = current + sum(max(0, order exposure))
reserved risk  = max(abs(low), abs(high))
utilisation    = reserved risk / limit
```

This protects against any subset of pending orders filling. Opposing orders cannot
cancel each other's reservations. Check each underlying (optional overrides,
otherwise common defaults) and the aggregate. Zero limits are allowed; positive
exposure against zero reports the largest finite double utilisation and rejects.
Overflowing analytical exposures mark risk incomplete and block trading.

An order is refused only when, with it pending, a bucket's reserved risk exceeds its
limit **and** exceeds that bucket's reserved risk without the order. An order that
does not raise any bucket's reserved risk is accepted, and fills, even while the held
book is already over a limit, whether the market or a tighter limit put it there: a
close that lowers exposure, a hedge no larger than the position, or an order on
another underlying that fits that underlying's and the aggregate's limits. A close
that removes a hedge, such as selling a protective put held against long calls,
can raise exposure. If it leaves the bucket further over its limit, it is refused
with `DELTA_LIMIT` or `VEGA_LIMIT`. Close the exposed side first, or use Flatten,
which closes the whole book.

Pre-trade checks require a supported registered unexpired contract, an open session that takes the order,
valid order/tick, max order quantity, valid fresh quote, price band, complete marks
and valuations, daily-loss allowance and exposure reservations. The kill latch and
daily-loss checks allow reducing orders as described below. Price protection is inclusive:

```text
abs(price - midpoint) <= max(absolute_band, relative_band * midpoint)
```

At acceptance, price is the limit or slipped market far side. At fill time it is the
actual slipped price, capped by a single-leg limit, allowing favorable moves without
comparing a stale limit to the new mid.
Checks rerun against current state before each proposed fill. Fill projection also
includes spread and fees in daily loss for orders that can open or increase
exposure. A failed fill check cancels the remaining order with `RISK_CHANGED`,
preserving the underlying reason in its message and actual/limit/scope; scaling failures keep `SCALING_LIMIT` as the code. A data gap is not a failure: when held positions' marks are stale
(`STALE_QUOTE`) or valuations are missing or stale (`MISSING_VALUATION`), as when a
batch brings an order's quote before the rest of the portfolio's after a stall, the
order keeps working and a later batch fills it once the data is complete.
Noncrossed resting orders wait; invalid quotes supply no fills.
Limit changes increment a revision and recheck working orders by the same rule, so a
working close survives a tighter limit while orders that would add to the excess are
cancelled with `RISK_CHANGED`. During an evaluation
or funded attempt, each tighter field applies immediately; each looser field waits
for the next trading-day rollover. The complete desired limits are journaled as
`pending_limits`. A later edit replaces the pending request but cannot loosen a
field already active today. Order size, exposure, price bands, daily loss and freshness
windows all follow this rule. Practice limits change immediately. Resetting an attempt
applies its pending limits. Limits themselves never liquidate positions.

Daily loss is `max(0, start_of_day_equity - equity)` including marks and fees. A loss
**strictly greater than** the configured allowance trips the latch **before matching**
on that market batch. Manual `trip_kill` does the same. The account becomes
**reduce-only**: orders that can open or increase a position are cancelled and new
ones reject with `KILL_SWITCH`. Closing orders, Flatten, bracket exits, the system's
own closes and share closes keep working. Early exercise rejects with `KILL_SWITCH`
because it delivers shares. Settlement remains permitted.

An order reduces only when every leg opposes its current position and its remaining
contracts, plus the other working user orders on that side of the same contract,
fit within that position. Multi-leg ratios count in contracts. Bracket exits do not
reserve this capacity: they shrink after each fill and cancel when the position is
closed. Every fill rechecks the holding, so working closes cannot flip it.
Reducing user orders skip the daily-loss allowance and fill projection; quote,
session, price, coverage and other risk checks still apply. Passed or failed
attempts likewise accept reducing orders; opening orders require a new attempt.

`reset_kill` requires a nonblank reason, records the reset, and immediately re-trips
if the loss still breaches. The account remains reduce-only while latched. A new
trading day's baseline permits a reset; rollover alone does not clear a manual or
daily-loss latch. While latched, `kill.reset_blocked` says why a reset could
not clear it now and when it can (this field describes `POST /api/risk/kill` with
`action=reset`, not `POST /api/account/reset`): `DAILY_LOSS` with the marked loss and the limit
while the loss still exceeds it (a reset works once it is back within the limit, or
from the next trading day's baseline), or a personal guardrail's code until it
expires. A refused reset answers with the same decision. Orders the latch refuses
or cancels say why: `KILL_SWITCH` with the manual reason, or that the daily loss
latch is set with the limit, and a personal guardrail's reason with its numbers.

Every trip, reset and release of the latch stays in the state's `kill_history`, oldest
first, with its market time and actor: a `trip` (with the reason it replaced, as when a
manual trip lands on a daily-loss latch, which its `kill_trip` event also names), a
`reset` (a kill-switch or account reset, with its reason and the latch reason it
cleared) or a `release` (a personal guardrail that expired at rollover or its cooldown
end). An account reset that retains the same personal latch adds no `reset` history
entry. If its reason changes, history records a `trip` naming the reason it replaced.
Account reset clears manual/daily-loss latches and re-checks `SOFT_FLOOR`, while
preserving `PROFIT_LOCK`, `TRADE_LIMIT` and active cooldown.
A reset cannot make stale data tradable.

`roll_day` is an explicit command on a later trading date. It closes the finished day
on the marks the account last published, which must be complete: the engine rolls an
account over on a new trading date's first market batch, before that batch's quotes
replace them, so the overnight move and fills at the open belong to the new day. A
trading date (`md::trading_date`) is a business day's New York date until
17:00 ET, when its last session (curb) ends; after that, and over weekends and
holidays, it is the next business day, whose overnight session opens that evening. So
an overnight trade counts toward the day it trades for, and a day's close is the last
marked equity before 17:00. A plan can end its day at another time
(`day_end_minutes`, see Plan objectives and the daily loss limit); rollover then
follows the plan's date. Rollover first monitors the old daily baseline, then
stores the new baseline. Repeated same-day rollover rejects. Manual and daily-loss kill latches survive rollover and recovery. Personal daily
latches clear at rollover; cooldowns expire on market time.
There are no deposits/withdrawals or cash interest. Without
the `buying_power` rule, negative cash and short positions are permitted subject to
the stated limits. With it, the selected margin requirement below applies; neither is a
full brokerage margin model.

### Personal guardrails

Guardrails belong to the account, separately from its plan, and default to off.
`PUT /api/risk/guardrails` takes `expected_revision` and a complete `guardrails` object:

| Field | Meaning; zero disables |
| --- | --- |
| `soft_floor` | Decimal dollar equity level above the plan floor |
| `soft_floor_percent` | Whole percent, 0–99, of the plan's drawdown distance to keep above its current floor; the higher of this level and `soft_floor` wins. New requests for 100 return `INVALID_LIMITS`: it can put the floor at the peak, where it latches at once. Stored 100 settings still load. On practice (no drawdown floor), the percent does nothing and raises an informational `SOFT_FLOOR_UNUSED` warning |
| `max_opening_trades` | Number of opening orders per trading day; an order counts once on its first opening fill, even if later partial fills cross rollover, and an atomic multi-leg execution counts once |
| `cooldown_minutes` | Market minutes without new opening orders after a triggered closing stop fills, up to 1440 |
| `cooldown_loss` | A closing fill's realised loss before fees must exceed this dollar amount to also start the configured cooldown |
| `profit_lock` | Day's marked P&L at or above this dollar amount makes the account reduce-only |
| `loss_streak` | Optional, 0–100: this many losing closes in a row start the `cooldown_minutes` cooldown (which must be set) and record a `LOSS_STREAK` tilt flag; a winning close ends the streak |
| `size_creep_percent` | Optional, 101–10000: an opening order whose contracts (every leg and ratio) exceed this percentage of the median of the recent opening orders is size creep. It records a `SIZE_CREEP` tilt flag, and the preview warns with `SIZE_CREEP` |
| `size_creep_window` | Optional, 3–100 recent accepted opening orders in that median; zero means 20. The median needs three. The window rolls across trading days |
| `size_creep_block` | Optional boolean: refuse size creep with `SIZE_CREEP` instead of only flagging it |

**Discipline guardrails.** The last four fields are opt-in and omitted from the journal
while off, so older settings keep their bytes; requests from before them remain valid.
A losing close is a closing fill (options or shares) whose realised P&L before fees is
negative; one at exactly zero leaves the streak alone. Each tilt flag is journaled as a
`tilt_flag` event (`time`, `code`, `message`) and kept for the trading day in
`guardrail_state.tilt` (the latest 50), beside `losing_closes` and `recent_opening_sizes`;
rollover clears the flags and the streak, not the window. Tightening applies at once
(the smaller nonzero streak and percentage, a block over a warning); loosening waits for
the next trading day, as for the other guardrails. The terminal's personal guardrails edit
them and list today's flags. They are personal settings, not plan rules: they never
decide an attempt and never take an account off its preset.

Touching the soft floor submits closing orders and latches `SOFT_FLOOR`. Liquidation
uses the same quotes, displayed liquidity and regular-session restrictions as a plan
floor breach, and retries remaining positions on later updates. It does not itself
fail the attempt. `TRADE_LIMIT` and `PROFIT_LOCK` leave positions open and cancel
opening orders. Working opening orders cancelled by a guardrail carry its code and
message, as new ones refused by it do, not `KILL_SWITCH`: `TRADE_LIMIT` with the day's
opening trades against the limit, `PROFIT_LOCK` and `SOFT_FLOOR` with the level, and
`COOLDOWN` with the time it ends. `PROFIT_LOCK` and `TRADE_LIMIT` last until
rollover, including across an account reset. `SOFT_FLOOR` lasts until rollover or an
account reset, which re-checks it against the new attempt's equity. The order whose
first opening execution reaches `TRADE_LIMIT` can finish its remaining partial fills;
other working opening orders are cancelled, and new opening orders are refused.
Other latches and all plan/risk checks can still stop the remaining fills. `COOLDOWN` lasts until the journaled
`cooldown_until`, including across rollover and account resets; wall time does not
shorten it. A later stop restarts it, and a longer cooldown setting extends one already active. Closing orders, Flatten and exits keep
working under all four reasons. The kill-switch reset cannot bypass an active guardrail.

Cooldown starts after any trader's triggered closing stop fills: a standalone stop or
stop-limit with an option, underlying or combo trigger, a trailing stop, the stop side
of an OCO pair, or a bracket stop-loss. A standalone price trigger must watch an adverse
move: an option price falling for a closing sell or rising for a closing buy. Underlying
triggers use the held option's direction: long calls and short puts lose when the
underlying falls; long puts and short calls lose when it rises. Underlying triggers on
combos use the legs' net delta; combo-price triggers use the signed closing debit, where
a rise is adverse. Trailing stops and bracket stop-losses always count as stops.
It does not require a realised loss; `cooldown_loss` adds other closing fills whose
realised loss before fees strictly exceeds the threshold. Profit-taking triggered
closes and take-profit exits do not start a cooldown unless that loss threshold is met.

Guardrails are tighten-only within the day on every account, including practice.
Enabling a rule, raising a soft floor, lowering a trade/profit/loss threshold or
lengthening a cooldown applies now. Other changes appear in `pending_guardrails`
until rollover. Starting a new attempt applies pending settings and keeps today's
`PROFIT_LOCK` and `TRADE_LIMIT` latches, opening-order count and active `cooldown_until`.
Among personal latches, it clears only the old attempt's `SOFT_FLOOR` and re-checks the floor against
new equity; personal settings persist. Lowering or disabling a daily discipline rule
through pending settings does not clear its existing latch before rollover.

An absolute soft floor can carry into a smaller plan. If the starting balance is at
or below the active absolute floor (or the pending floor the reset will apply), the
reset is allowed, but the soft floor latches at once. The terminal warns before
confirmation, and the reset response's account `warnings` list includes `SOFT_FLOOR`.
Lower the floor in personal guardrails before resetting: a lower setting is pending
until rollover, and a reset applies pending settings. A soft floor still at or above
equity can trip again after rollover or reset. The existing kill latch is shared;
a manual or daily-loss trip still requires its own reset after the personal rule expires.

`GET /api/risk` includes active and pending settings, `pending_effective`
(`next_trading_day` or null), the last `pending_applied_at`/`pending_applied_day` (a
rollover's, or an account reset's that applied pending settings), and
`guardrail_state`: opening count, latched reasons, effective soft floor, cooldown end
and market-time seconds left. Rules and Risk show pending values beside active ones.

### Order preview and breach risk

When a stop or trade-risk rule is enabled, opening previews also return
`trade_risk`, `trade_risk_limit` (decimal strings or null), and `trade_risk_basis`
(`stop_loss`, `expiry_payoff`, `unbounded_or_unknown`, or null when not applicable).
They are separate from the fee-inclusive `max_loss` used for floor sizing.


`POST /api/orders/preview` takes the same order body as submission, plus optional
`floor_share` in (0, 1], default 0.5. The const reducer path shares submission checks
and makes a private full-size projection. It writes no journal, changes no account or
ID counter, and consumes no displayed size. The normal HTTP write protections apply.

The response contains `decision` (`ok` or a reason code), `reason`, `buying_power`
(`required`, `before`, `working`, `after`), `exposure_change` (dollar delta, dollar gamma per 1%,
vega and theta), `max_loss`, `max_loss_basis`, `equity_at_max_loss`,
`breaches_floor`, `breaches_soft_floor`, `max_units`, `max_units_buying_power`,
`max_units_floor`, `max_units_basis`, projected `breach`, `execution`, `warnings` and
`liquidity`: for each leg, its `symbol`, `side` and `contracts`, whether paper orders
can fill on its quote now (`executable`, with the `INVALID_QUOTE` or `STALE_QUOTE`
`reason` when not, and a one-sided, crossed or sizeless quote named in its message),
the `displayed` size on the side it takes and `size_left`, what this account's orders
have left of it on that observation. Orders beyond `size_left` wait for a new quote,
or cancel as `IOC_REMAINDER`, unless impact supplies simulated depth. Missing
inputs produce null analytical values. `simulated: true` labels the projection.

`warnings` lists `{code, message}` advice on terms that are accepted but rarely meant;
it never changes the decision, and submission gives none:

| Code | When |
|---|---|
| `STOP_AS_LIMIT` | A bracket `stop_loss` has a `limit_price` and no trigger, so it rests as a limit exit, not a stop; the message says when the closing side already reaches it, which closes the position as soon as the entry fills |
| `STOP_REACHED` | The stop's trigger is already reached on the current quote (for a combo, the closing legs' far sides, which include both spreads), so it fires as soon as the entry fills, or as soon as held exits are accepted |
| `TARGET_REACHED` | The take-profit limit is already marketable, so it fills at once |
| `TRIGGER_REACHED` | A conditional order's level is already reached, so it activates at once |
| `SLIPPAGE_BAND` | The account's `slippage_ticks` alone price a market order outside the price band, so it is refused `PRICE_BAND`: widen the band or lower the slippage |
Market orders use slipped far sides, through the impact blocks a fill would walk; limit
orders use their limit debit or credit.
Fees are included. Identical client-ID retries return their original decision with no
additional buying power or Greek change; loss projection and sizing are unavailable.

`execution` is what submitting the order now would do, from the same acceptance and
matching run on a private copy of the account: its `status` just after submission
(`filled`, `partially_filled`, `working`, `armed`, `cancelled` or `rejected`),
`filled_quantity` and `remaining_quantity`, the `reason` its rest ended at once (an IOC
remainder past the displayed size, say) or it was refused, its `fills` contract by
contract with their prices, and `average_fill_price`. As displayed, a market order
larger than the displayed size fills that size and cancels the rest; with fill latency
nothing fills on acceptance and the order works until a later quote. `schedule` prices
the full size at the current far sides with the account's slippage, block by block
through impact as a fill would walk them and whatever the displayed size, each block
as `symbol`, `side`, `quantity` and `price`, and `average_price` is its average per unit
(a multi-leg order's net debit, negative for a credit). The full-size projection below
uses those prices for a market order and the limit for a limit order.

`fee` is the full remaining size’s fee at the current position and remaining cap;
`fees` breaks it into commission, clearing, regulatory and index charges under an
itemized schedule. Both are null without a projection; flat-fee previews have a
total and null breakdown.

`buying_power.required` is what the order reserves while it works. `before` is the
available buying power now, `working` the available buying power while the order works
before any of it fills, and `after` the available buying power once the order has
filled in full at the projected price: not `before` less `required`, since a fill that
releases margin, as a close or a roll does, raises it. An `exits_only` pair rests until
one of its exits fills, so its `after` is the buying power once it is accepted, with its
fee reservation held, as the account shows after submitting it; its loss and Greek
change still project the primary exit (the target, or the stop without one) filling.

For an order that opens every leg, a bounded same-expiry payoff has an exact maximum
loss at zero or a strike; a net short call tail is unbounded. Other orders use the worst
loss of the projected account on its configured spot × volatility grid, labelled
`scenario_grid`: among them every order that reduces a holding, such as a close, a roll
or a sale of more than is held, so selling held longs is never priced as writing new
shorts, and a close's loss is its cost against the marks plus whatever the account still
holds could lose. This finite grid is not a bound on all possible losses. The exact
`expiry_payoff` value describes the order's own payoff, while the scenario value
describes the account after the order. A bracket entry's projected account includes its
exit pair, so `buying_power.after` holds the exits' fee reservation as the account will.
`max_units` fits buying power, pre-trade limits and the requested share of current
room above the nearer plan or soft floor. Touching a floor never fits. The share is
rounded down to millionths for fixed-point sizing. `max_units_buying_power` is the
same size with buying power and the limits alone, floor room aside, and
`max_units_floor` the size whose loss fits the floor share among those the pre-trade
checks accept, without the fit to available buying power (a plan's `buying_power`
rule still refuses); it is null on an account with neither a plan nor a soft floor.
`max_units_basis` names the constraint that binds the final size: `floor`,
`buying_power` or `limits`, and is null when sizing is unavailable. Ties prefer
the pre-trade limits, then buying power, then the floor share; the floor is named
when it further reduces the feasible size. Previewing changes reports the same field.
Zero means no size fits: one unit already needs more buying power than is available,
or raises an exposure or order-size limit. Null means sizing is unavailable: an
identical retry, an account stopped by a journal failure, held exits (their size
follows the position), an order the checks refuse at any size for another reason
(a closed session, a stale quote, the kill switch, a price off its tick), or a loss
that cannot be projected without marks and valuations. The preview is a current projection,
not an execution promise; real orders still take all checks when submitted and filled.

`POST /api/orders/{id}/preview` previews a change to a resting order without making it.
It takes the fields `PUT /api/orders/{id}` takes (`quantity`, `limit_price`,
`trigger_level`, at least one of them) and `floor_share`, and answers with the same
response. Its `decision` is the change's, from the checks a change takes with the
order's own reservation released, and an unknown or finished order answers
`UNKNOWN_ORDER` (404) or `ORDER_TERMINAL` (409) as the change would. A quantity below 1
returns 400 `INVALID_REQUEST` and records nothing. `execution` is what
the change would do now, on a private copy of the account: a limit repriced through the
market fills at once (`filled_quantity` and `fills` count only what fills now), an armed order whose new level is reached activates, and a refused
change leaves the order as it stands, with its status and remaining quantity. The rest
projects the order on its new terms as a new order is projected: `required` is what it
would reserve, `working` the buying power while it works on them (its current reservation
released), and `after`, the loss and the Greek change are for its remaining units filling
in full, its filled ones already in the account. Sizing counts the units the order could
still work beside those already filled, so the quantity to send is the filled quantity
plus `max_units`; a bracket exit's size follows its position, so its sizing is null.
Edit order in the terminal shows this preview as the terms change.

`GET /api/risk` and `GET /api/account` expose `breach`: dollar `room` and `soft_room`,
`complete`, and `underlyings[]`. Each held underlying has `spot`, `close_sigma` (one
standard deviation of its log price to today's close), `complete`, and optional
`down`/`up` levels with signed `points`, `percent` and `touch_probability`. The levels
are where equity would reach the plan floor, or the personal soft floor on an account
without one (a practice account, say); with neither floor, held underlyings are listed
without levels. `soft_down`/`soft_up` are where equity would reach the soft floor,
which liquidates first when it sits above the plan floor; without a plan floor they
repeat `down`/`up`, and without a soft floor they are null. One underlying moves at a time while volatility and option
life stay fixed. The scenario solver scans by 0.25% to −99.75% and +100%, then by 1%
to +1000%, and bisects the first crossing. A missing level means no crossing in that
scan, not safety outside it or between scan points. Missing valuations leave levels
unavailable.

Touch probability is a model estimate from the driftless log-return reflection formula
`erfc(abs(log(level / spot)) / (sigma * sqrt(2)))`, about twice a terminal tail
probability, where `sigma²` is the market's implied variance to that trading day's
regular close: the nearest expiry's at-the-money IV² times its time to settlement (the
calendar time the IV was solved on), of which today's session takes its share in
regular-session time. A 0DTE expiry gives today's variance directly; a Friday expiry
seen on Wednesday morning gives today about 5.5 of its 18.5 remaining session hours.
Calendar time would spread that variance over nights and weekends and understate the
hours left today several times over. After the close the probability is zero; without
an IV it is null. These are simulated scenarios and
model estimates, not observed market outcomes. Dashboard, Risk and both ticket
previews label them accordingly. Tickets debounce previews, offer **Size to floor**,
and show an explicit failure without guessing when the endpoint is unavailable.

### Cash transfers and financing

Practice accounts support `POST /api/account/cash-flows` with a `client_id` and a
signed decimal-string `amount`: positive deposits, negative withdrawals, nonzero
whole cents. The terminal's Dashboard offers the same controls. Repeating an ID
and amount within the attempt is a no-op; changing the amount returns 409
`DUPLICATE_CLIENT_ID`. Keep the same ID when retrying a failed network request.
Withdrawals must fit cash and unreserved buying power, leave positive equity and
have complete marked equity. Evaluation, verification and funded accounts refuse
transfers with `PLAN_LOCKED`; funded withdrawals continue to use the payout rules.
Custom practice means no evaluation objectives or program fees and evaluation
phase, not simply an account named Practice.

Transfers move capital, the daily equity bases and daily extremes by the same
amount. They do not change realised profit, Greek P&L or day P&L and cannot clear
a latched risk limit. Existing closed-day history is retained.

Three optional annual rates are in basis points (100 bps = 1%, integer 0–100000):
`cash_credit_rate_bps`, `margin_debit_rate_bps`, `stock_borrow_rate_bps`. All default
to zero, preserving existing plans. Set them in custom rules or as create/reset
broker overrides, or in the terminal's margin settings. Rates are program rules:
an account whose rates differ from its preset's no longer reports that preset's
`plan_id`, and create/reset overrides that change a plan's rates are refused with
422 `PLAN_LOCKED` unless the plan is practice (no objectives, fees or evaluation
counts), since credit interest would otherwise pay an evaluation's target. Credit interest uses
positive closing cash. Debit interest applies to negative cash only under portfolio
margin on margin accounts. Borrow fees use each short stock's last closing marked
notional. No rate, borrow availability or price is inferred from a live broker.

`roll_day` posts simple actual/365 accrual for elapsed calendar days (weekends and
skipped days included), before the new day's assignments and dividends. Each
posting is rounded to whole cents, ties away from zero. A skipped interval uses
the last available balance and marks for all its days, without intra-interval
compounding; repeated rollover cannot post twice. Zero-cent accrual is omitted.
Complete marked equity is required, as for dividend rollover. Cash and realised
P&L change, and the amount appears in the account's Greek P&L **other** bucket;
borrow charges also appear under the stock's attribution. Financing counts toward
equity and daily loss, including evaluations; it does **not** move the opening
daily-loss base. Normal peak-based loss rules can still ratchet on higher equity.

`GET /api/account` includes `cash_transfers_allowed` and `cash_flows`: transfer and
financing history across attempts with ID, attempt, posting time/date, signed
amount, kind, symbol, calculation basis, rate, calendar days and transfer client ID.
The Dashboard shows the current attempt's latest ten postings. Journals retain
all postings and recover them without reaccruing.

### Single-name halt drills

Scenario `halt` events can select `symbols`, e.g.
`{"type":"halt","at":"11:30","minutes":5,"symbols":["SPY"]}`. Omit `symbols`
for a market-wide halt. Selected underlyings' quotes freeze while heartbeats keep
replay time moving. Other symbols continue trading.

During a replay's stock regular session, `POST /api/replay/market/halt` with
`{"symbol":"SPY","minutes":5}` authors a single-name pause at the current replay
time. The Dashboard's **Single-name halt drill** offers the same action. It accepts
subscribed stocks/ETFs and 1–390 minutes, capped at that day's regular close. It
requires replay or admin scope; the live endpoint refuses drills. Commands are
journaled and included in replay verification. An API-authored pause blocks
execution while the underlying replay feed may continue supplying prices.

New orders and stock trades on that underlying receive `MARKET_HALTED`; resting
orders keep their usual deadlines and wait for usable quotes after the pause.
Other underlyings are not halted; account risk checks and overlapping market-wide
halts still apply.

Stock prints stamped inside a halt are not taken as marks. When a halt is still in
force at the regular close (a scenario halt that runs past 16:00, or one capped at
it), held shares keep the last print before the halt, in that session, as their
mark. They stay not `fresh`, so opening trades still wait, but the holding reports
`halted: true` and day rollover closes on that pre-halt print (equity, borrow fees
and the next day's base) instead of refusing for incomplete marks. A halt that
ended before the close still needs a print after it.
Status/ticks publish single-name pauses beside circuit breakers, with `underlying`
and `simulated: true`. The terminal banner names the affected symbols. This is a
LULD-style rehearsal, not an exchange price-band calculation or a prediction of a
real regulatory halt. It does not simulate an auction at reopening.

### What-if

`POST /api/orders/what-if` compares candidate adjustments against the held book before
any is sent. It takes `candidates`, one to six, each with an optional `name` (at most 64
UTF-8 bytes) and `orders`: one to four orders as `POST /api/orders` takes them, a client ID
optional. Candidates containing `then` or `oco` run the real submission reducer on
the private copy at current quotes and available size. Parent fills activate children;
OCO fills cancel siblings. Resting limits, triggers and partial fills remain as they
would after submission, with their buying-power reservations. Child rejections
appear in the candidate and parent check. No future prices or trigger hits are assumed.
Without chains, a candidate's orders are checked in turn as submission checks them, each after the
ones before it have filled, and filled in full at the preview's projected prices:
slipped far sides through the impact blocks for market orders, the limit for limit
orders, fees included. Closing the tested side and opening a new spread is one candidate
of two orders. It runs on a private copy of the account, like a preview: it writes no
journal, allocates no IDs, takes no displayed size, and the normal write protections apply.

The response has `current`, the account as it is, and `candidates`, each with its
`name`, its `decision` (`ok`, or the first order's refusal, with `reason`), each order's
`decision` and `reason` in `orders`, and `after`. For chains, `after` is the account
following actual current-quote submission, including rejected or still-working
orders. For candidates without chains it assumes every order fills; it is null
when an order cannot be projected (an unknown contract, or no quote). A refused
unchained candidate is still projected, so its risk shows beside its refusal. Each account has `equity`, available `buying_power`, the book's
`exposure` (dollar delta, dollar gamma per 1%, vega and theta; null while risk is
incomplete), `max_loss` (the worst cell of the configured spot × volatility grid, from
today's equity, so a candidate's cost to trade counts), `equity_at_max_loss`,
`breaches_floor` and `breaches_soft_floor`, `scenarios` (each cell's P&L from today's
equity, spot-major, in the grid of `GET /api/risk`) and `breach` as above. The grid is
finite and the values are simulated estimates, not execution promises.

Positions (What-if) lists candidates built from a ticket's preview (Add to what-if, into a
new candidate or one picked to take them) or from closing picked positions, compares
them in a table with each one's change in Greeks, and shows P&L by spot move at unchanged
volatility. Removing a candidate's last order removes the candidate. Compare waits for
every trimmed name to be nonempty and at most 64 UTF-8 bytes, with a hint beside invalid
names. The list is kept in the browser, per account.

### Risk warnings

`GET /api/risk` and `GET /api/account` list `warnings`: what the held book is close to,
most urgent first. Each has a `code`, a `severity` (`warning` when it costs room, buying
power or a fill soon, `info` when it is worth knowing), a `scope` (the underlying, or
`aggregate` for the account), the contract's `symbol` when it is about one, a `message`
in words, and `actual` and `limit` numbers whose meaning depends on the code:

| Code | When | `actual`, `limit` |
| --- | --- | --- |
| `DELTA_LIMIT`, `VEGA_LIMIT` | A bucket's held exposure is over its limit. Orders that do not raise its reserved risk still go: a close that lowers exposure, a hedge no larger than the position, or an order on another underlying within its own and the aggregate's limits. Removing a hedge, such as selling a protective put against long calls, can leave the bucket further over its limit and be refused with these codes. Close the exposed side first, or use Flatten to close the whole book | The absolute exposure and the limit |
| `DELTA_HEADROOM` | An underlying is within max(1%, one standard deviation of its move to today's close) of a move that takes its dollar delta to its own limit or the account's. Per 1% move, dollar delta changes by dollar gamma plus 1% of itself; this is a first-order estimate | The signed percent move, and the threshold |
| `SOFT_FLOOR` | Equity is at or below the soft floor, which latches at once, closes positions and refuses opening orders; lower settings wait until rollover or an account reset applies them | Equity and the soft floor |
| `SOFT_FLOOR_UNUSED` | Informational: a percent soft floor is set on a plan without a drawdown floor, where it does nothing; use an absolute floor instead | No numeric threshold |
| `SOFT_FLOOR_ROLLOVER` | At rollover the soft floor would be at or above today's equity: pending guardrails apply, and a percent soft floor follows a ratcheted plan floor | Equity and that soft floor |
| `FLOOR_RATCHET` | An active end-of-day drawdown floor that tonight's close at today's equity would raise (and lock, at a lock balance) | The floor tomorrow and today |
| `EXPIRY_DELIVERY` | An American equity or ETF option expiring today a cent or more in the money, without a do-not-exercise instruction: held into expiry it is exercised or assigned and delivers shares, together the strike. Before an account's pre-expiry cutoff, names that time and the market close; only contracts still held because a close cannot fill are delivered | Buying power once every option expiring in the money today has delivered at today's price, and zero; `warning` whenever projected buying power is negative, including before the cutoff; otherwise `info` |
| `EARLY_ASSIGNMENT` | A short American equity or ETF option the rollover may assign (about half of it, as described under early assignment): marked below its exercise value, or a call with less time value than a dividend going ex within a week and before its expiry; `warning` for tonight's rollover | The mark and the exercise value, or the time value and the dividend |
| `EX_DIVIDEND` | A held underlying (options or shares) goes ex-dividend within a week; `warning` when short shares will pay it | The dividend a share, and null |

Warnings use the account's marks, valuations, limits and the dividend calendar the
rollover pays from. They are views, not rules: nothing is journaled and no order is
placed. Positions (Risk) and Dashboard show them above the breach estimates.

## Playbooks

[Playbooks](playbooks.md) combine versioned setups, server template selection,
preview sizing, GTC entries, brackets and per-account entry limits. Definitions
live beside the main journal, outside reducer state. Staged orders write nothing
until sent through the normal order path. Live paper accounts (practice or evaluation), replays and scenarios allow
auto entries and time stops. Live Auto persists versioned forward-test windows
and resumes after restart, subject to normal feed, session and risk checks. Orders retain the
exact playbook version in a tag. The Playbooks page reports adherence and expectancy;
the Journal filters across versions.

`GET /api/account/pass-odds` resamples recorded equity days through the same plan
arithmetic as the reducer. It requires ten completed days with intraday extrema:
with fewer, without an evaluation rule or without complete current marks it returns
422 `PASS_ODDS_UNAVAILABLE` with the reason.
It is an estimate from past results, not a prediction. Per-playbook sampling excludes
days whose account equity cannot be attributed exclusively to that setup.

**Challenge economics.** `?economics=true` adds `economics`: the expected value of buying
the account's program. The steps are the account's preset and those its passes unlock
(a two-step challenge, its verification, its funded step). Each evaluation step's
single-attempt pass odds come from a fresh attempt at its rules on the same recorded days
and seed; its eventual odds allow its `max_resets` (unlimited counts 100 attempts); its
expected cost is its purchase fee (the first step) or activation fee, plus its reset fee
times the expected resets. The funded step costs its activation fee and is worth its first
payout: the trader's share of the first cap (or the minimum without caps), with odds from
the same simulation of reaching the profit that withdraws it, with its qualifying days,
before its floor. `trading::challenge_economics` multiplies each step's eventual odds into
the next: `expected_value = reach × payout_odds × payout − Σ reach_i × cost_i`, and the
`break_even_pass` is the first step's single-attempt pass odds at which it is zero
(bisection; null when no rate breaks even, zero when nothing costs). `payout_odds` (0–1)
and `payout` (dollars) replace the simulated first payout, and are what a custom plan
without a preset program needs (`available: false` with a reason otherwise). Program fees
are bookkeeping in this simulator; the figure is an illustrative estimate, labelled so.
The Rules page's **Challenge economics** panel shows it, with the two overrides.

## Account rules and evaluations

`SessionConfig::rules` (`AccountRules`) turns the account into an evaluation. The
defaults describe the plain paper account above: no target, no drawdown floor, any
side, no buying-power check. All rule money is exact.

| Rule | Effect |
| --- | --- |
| `profit_target` | Pass when profit under `profit_basis` reaches the target and every objective below is met (zero disables) |
| `max_drawdown` | Fail when equity touches peak − drawdown (zero disables) |
| `drawdown_mode` | `Intraday`: the peak follows every fully marked equity high. `EndOfDay`: the peak moves only at rollover, from the last fully marked equity observed on the finished date. `Static` (API `static`): the floor stays at the starting balance − drawdown for the whole attempt and counts as locked from the start, while the peak still follows every high. The peak is the high-water mark the floor follows; an account without a target or drawdown (practice) keeps it the same way, although no rule reads it |
| `buy_only` | A sell must close contracts already held, counting working and armed sells on the same contract; otherwise `BUY_ONLY`. A manual (untriggered) close that only the account's own armed stop sells keep from fitting supersedes them: it cancels them with `POSITION_CLOSED`, newest first and only as many as it needs, and keeps them if it is refused anyway. Preview shows the same. A requested long-put exercise that would sell more shares than the long shares held (after any working share sells) returns `BUY_ONLY`; exercising a protective put against held shares is allowed. Working share sells count against the long shares held the same way, so an opening share sell is refused with `BUY_ONLY` at acceptance. Expiry auto-exercise and assignment still deliver shares: `buy_only` governs trader orders and exercise requests, not expiry delivery |
| `defined_risk` | Each short option needs a long of the same type on the same underlying that expires with it or later, any strike (`naked_shorts` counts the rest). An order, single or multi-leg, that would leave more shorts uncovered than before rejects with `DEFINED_RISK`, so closing a short is always allowed. Open orders count as if every sell they offer filled and no buy did (a multi-leg order fills whole; a bracket's two exits sell its position once), so a working sell can never take the long a short needs. Bracket exits and exercise keep shorts covered too. Off in every preset; custom rules take it |
| `max_contracts_held` | Option contracts held (sum of absolute position quantities), plus opening contracts of working orders and this order, may not exceed this cap (0 disables; 1–100000). Counts combo leg ratios and reserves closing capacity only once across working orders. Shares and managed exits do not count. Opening entries and quantity changes refuse with `MAX_CONTRACTS_HELD`, projected count and cap; reducing and system closes remain available |
| `require_stop_loss` | Every opening order needs a bracket `stop_loss` trigger (default false); otherwise `STOP_REQUIRED`. An option stop triggers below for a long entry, above for a short; a combo stop triggers above on its closing signed net. Underlying triggers also qualify. A target or a limit alone is not a protective stop. User cancellation of that stop, including cancel-all, or reducing its protected size, refuses while its position is open; cancelling an unfilled entry leaves its filled part's stop intact. Closing, flattening, OCO fills, partial stop re-arming and automatic playbook exits keep their normal behavior |
| `max_trade_risk` | Maximum loss per opening order, before fees, in dollars; zero disables. Uses the limit price, or a market order's current far side, and the total order quantity (including already-filled units when changing it). At an option stop the loss is (entry minus stop) × quantity × multiplier for a buy, reversed for a sell. A combo uses (signed entry net + signed closing stop net) × units × multiplier. Negative loss is zero. Without an option/combo stop, uses the existing bounded expiry payoff (long premium or spread width less credit); underlying-price stops also use this because their option fill price is unknown. Unbounded or unknown expiry loss always exceeds an active cap; never substitutes a scenario-grid estimate. `MAX_TRADE_RISK` carries actual dollars (null if unbounded/unknown) and the limit |
| `max_trade_risk_percent` | Whole percent 0–100 of positive room to the plan floor (equity − floor) at entry or change, rounded down to a micro-dollar. Zero disables; without a plan floor this rule does nothing. If an absolute risk cap also applies, use the smaller cap |
| `buying_power` | New orders and their fills must not take buying power below zero; otherwise `BUYING_POWER` |
| `slippage_ticks` | Integer from 0 to 10 adverse ticks per option fill, including each combo leg and closing orders; default 0 |
| `fill_latency_ms` | Integer from 0 to 60,000 milliseconds on market time before a quote can execute an order; default 0 |
| `inside_fill_percent` | Integer from 0 to 100; zero disables inside fills, 50 fills limits at or beyond midpoint inside the spread; default 0 |
| `impact_ticks` | Integer from 0 to 10 extra adverse ticks per additional displayed-size block; 0 keeps the displayed-size cap |
| `fees` | Optional [itemized fee schedule](#fees) in place of the flat per-contract fee |
| `margin` | `strategy` (default) or `portfolio`, selecting the position requirement below. Plans use strategy margin and As displayed fills by default; custom rules, or a margin override beside a plan, can select portfolio margin |
| `account_type` | `margin` (default), `cash` or `ira`, what the account may hold (see account types below). A cash account or IRA uses strategy margin and enforces buying power; an order, bracket exit, exercise, abandon or share sale that would leave it holding a call no shares (or, in an IRA, no long call) cover, or short shares, rejects with `ACCOUNT_TYPE`, counting open orders as `defined_risk` does |
| `house_margin_percent` | Integer from 0 to 400: a broker's house margin, raising each naked requirement (beyond its buy-back value) and each short sale's margin (beyond the shares' value) by that percentage under strategy margin, or each underlying's scan under portfolio margin; default 0 |
| `pm_vol_shock` | Integer from 0 to 50: portfolio margin also takes each price shock with implied volatility this many points up and down; default 0 |
| `expiry_cutoff` | From the last trade − cutoff until the last trade (`OptionContract::last_trade_time`: 16:00 ET on expiry day for index series such as SPXW, 16:15 for ETF options that trade until then, and the regular close the business day before for AM-settled series), every open order on the contract cancels with `EXPIRY_CUTOFF` (DAY, GTC, armed and bracket exits alike, held or not), positions are closed, and only closing orders are accepted |
| `phase` | `Evaluation` (default), `Verification` (the second evaluation step), or `Funded`; verification has the same target/objective semantics as evaluation; funded has no target and pays out under `payouts` |
| `lock_balance` | Caps the trailing floor: the floor is the lesser of peak − drawdown and the lock, and once peak − drawdown reaches the lock the floor stays there and stops trailing (zero disables). A lock at or below the starting floor (starting balance − drawdown) therefore fixes the floor at the lock from the start: a static floor, which below the starting floor gives more room than `max_drawdown` alone would |
| `lock_at_start` | The same lock at the attempt's own starting balance, so the floor trails until it reaches the start and then stays there. Not with `lock_balance`, and neither with a static floor (`INVALID_RULES`) |
| `profit_basis` | `Equity` (default): profit is fully marked equity less the starting balance. `Balance`: profit is the closed balance (cash plus the positions' cost, so realised P&L after fees, open P&L left out) less the starting balance, and a pass also needs every position closed. It also decides each day's profit for the consistency rule and profitable days: the day's equity change, or its net realised P&L |
| `daily_loss_limit` | Dollars below `daily_loss_basis` that equity may not touch in one trading day (zero disables); see Plan objectives and the daily loss limit |
| `daily_loss_basis` | `Equity` (default, the day's opening equity), `Balance` (its opening closed balance), `Higher` (the higher of the two) or `Peak` (the day's fully marked equity high, starting at its opening equity, so the limit trails the day's gains) |
| `daily_loss_action` | `Lock` (default): close every position and refuse opening orders until the next trading day. `Fail`: fail the attempt |
| `min_hold_seconds` | F61: ordinary user reductions need this many seconds of market time since the round trip's first opening fill (0–3600; 0 disables), for options and shares; `MIN_HOLD` reports seconds held and required |
| `microscalp_seconds` | F61: a round trip closed strictly before this age is short (1–3600); set with `microscalp_percent`, or leave both zero |
| `microscalp_percent` | F61: maximum whole percent of attempt profit from positive net P&L of short closed round trips (0–100); 0 disables; evaluation and verification only |
| `min_trades` | F30: minimum closed whole option trades to pass (0–10000; 0 disables); evaluation and verification only |
| `trade_consistency_percent` | F29: best profitable closed whole option trade, net of fees, at most this whole percent of attempt profit under `profit_basis` (1–100; 0 disables); evaluation and verification only |
| `consistency_percent` | A pass needs the best day's profit at most this whole percent (1–100) of `consistency_basis`; zero disables |
| `consistency_basis` | `Total` (default): the attempt's profit. `PositiveDays`: the profitable days' profits added up |
| `min_trading_days` | A pass needs this many trading days with an execution of the trader's own orders (0–366) |
| `min_profitable_days` | A pass needs this many days whose profit reaches `profitable_day_profit` and is above zero (0–366) |
| `profitable_day_profit` | The profit a day needs to count as profitable; zero counts any day above zero |
| `flat_time`, `no_overnight` | Optional mandatory close minute before day end; independently fail held positions at rollover |
| `time_limit_days`, `inactivity_days` | Calendar-day limits, 0 (off) to 366; the time limit applies to evaluation and verification |
| `underlyings` | Up to 32 allowed underlying symbols; empty permits all |
| `trading_start`, `trading_end` | Optional New York minutes internally, `HH:MM` or null on the API; both set or both off, start inclusive and end exclusive |
| `events` | Up to 256 saved news/earnings/ex-dividend/split events; sorted and deduplicated |
| `news_before_minutes`, `news_after_minutes`, `news_action` | 0–240 minutes each; both zero off; `block` (default) or `flatten` |
| `hold_restrictions`, `hold_cutoff` | Set of weekend, earnings, ex_dividend, split; empty off; cutoff `HH:MM` ET, default 15:45, before day_end |
| `day_end_minutes` | Minutes after New York midnight at which the plan's trading day ends, 975 (16:15) to 1440 (24:00); default 1020 (17:00). API `day_end` as `HH:MM` |
| `scaling` | Optional steps `{profit, contracts}`: the finished day’s closed-balance profit selects the next session’s cap on option contracts held; each leg counts, shares do not. Empty disables (see Scaling plan below) |
| `size_scaling` | Funded only: optional periodic profit/payout reviews grow account capital linearly from the original size, capped at a maximum. Absent/null disables (see Account size scaling below) |
| `payouts` | Funded phase: qualifying days, withdrawal share, trader split, minimum and caps (see Funded accounts and payouts) |

These trade-entry rules leave reducing orders, managed exits and system closes available.
Risk caps are checked at acceptance and changes, not retroactively when a floor moves.
Stops may slip or stop-limits remain unfilled: trade risk assumes execution at the
trigger price, without fees or slippage, and is not a guaranteed loss bound. Share
entries cannot attach brackets, so a stop-required plan refuses them; with only a
risk cap, a long share entry risks its purchase price and a short is unbounded.

Equity-based outcomes use **fully marked equity**: every position has a mark, fresh or not. A
position without any mark defers the decision rather than counting as zero. Every
transaction runs the monitor after its command and the daily-loss check, as well as
after each atomic fill and before matching a new quote batch: it records
the day's latest marked equity and applies `evaluate_plan`. That ratchets an
intraday peak, fails on `equity <= floor` (the floor is breached by touching it),
then locks the day or fails on the plan's daily loss limit, and otherwise passes
once the target and every objective are met. Breaches are checked on every
transaction in every mode; the mode only controls when the floor rises. The decision
is sticky for the attempt: open user orders cancel with `EVALUATION_CLOSED` and the
system tries to liquidate every position. After a pass or failure, orders that only
reduce held positions remain allowed, including Flatten (`close_positions`) and
[disposing of worthless positions](#disposing-of-worthless-positions). Each closing
leg must oppose a holding and fit within it after working user closes, as under the
kill latch; an order that opens, adds or reverses rejects with `EVALUATION_CLOSED`.
A resting closing limit can wait for executable liquidity. Ordinary closing orders
still need a fresh executable book, closing-side liquidity and their eligible trading
session; the system market IOC needs the regular session. System liquidation keeps
retrying, but leaves contracts reserved by working user closes to those orders, and
managed exits shrink with the position. The status, `decided_at`, `decided_equity`
and decision code never change after the decision; later fills mark equity without
running plan decisions again. The terminal explains leftover positions on the
Dashboard and Positions page and allows their closing tickets.
`Evaluation::decision_code`
names the rule that decided it: `PROFIT_TARGET`, `DRAWDOWN_FLOOR` or
`DAILY_LOSS_LIMIT`, `TIME_LIMIT`, `INACTIVITY`, `OVERNIGHT_HOLD` or `HOLD_RESTRICTED`. Calendar and
overnight-hold failures run before the
command, without requiring marks.

**System orders** perform liquidation and expiry auto-close: market IOC orders with
`system = true` and client IDs `system:drawdown:N` (a failed attempt), `system:target:N`
(a passed one), `system:time_limit:N` or `system:inactivity:N` (calendar failures),
`system:overnight:N` (overnight holds), `system:hold:N` (holding cutoffs or failures),
`system:news:N` (news flatten), `system:flat_time:N` (mandatory
flat time),
`system:daily_loss:N` (the plan's daily loss limit, locking the day or
failing the attempt), `system:expiry:N` (the expiry cutoff) or `system:soft_floor:N` (a
personal soft floor while the attempt is still active; once the plan decides the
attempt or locks the day, its own label wins, even when the soft floor latched in the
same update).
Like Flatten, they close shorts first: a short is bought back before the long that
covers it is sold, so the order of events never shows a naked short.
They need a registered unexpired contract, the regular session and a fresh
executable book and available closing-side liquidity under the selected fill model.
With latency, the close stays pending until an eligible later quote. They skip the kill
latch, price band, daily-loss, exposure, rule and buying-power checks because they
only reduce risk. Without executable liquidity nothing is recorded; the monitor retries
on later transactions until the account is flat, so system orders never accumulate.

**Buying power** is cash less the positions' margin requirement less working-order
reservations under strategy margin, the default, where long premium is paid in full;
portfolio margin, below, takes it from equity instead.
With the default `margin: "strategy"`, a naked short option holds its buy-back
value (last mark, or its entry credit without one) plus the naked requirement
`100 * max(20% of spot - OTM amount, 10% of spot for calls or of strike for puts)`,
with the strike standing in for a missing spot. `margin_requirement` nets spreads,
shares and straddles. A short pairs with a long of the same type on the same underlying
that expires with it or later, as a vertical: a put long below or a call long above its
short costs the width, one at or beyond it nothing, no pair costs more than naked, and
unpaired shorts are naked. Every 100 shares cover an option too, whatever it expires:
long shares make a short call covered, which holds nothing more (the shares are paid
for and deliver on assignment); short shares make a short put covered, which holds its
buy-back value (the short sale's proceeds buy the shares back on assignment); and a long
call caps 100 short shares at its strike, a protected short, instead of 150% of their
value. Short puts and short calls also pair as Reg T straddles or combinations,
of any strikes and expiries: each pair holds the greater naked requirement (buy-back
value included) plus the other side's buy-back value. Verticals, share covers,
straddles and expiry pools compete in one integer allocation. A pool holds shorts
from one expiry and longs expiring then or later, valued at intrinsic at the short
expiry. Its terminal call slope must be nonnegative. Every option contract is
allocated once; shares supply whole lots of 100. Two pools with the same expiry
can always be combined without increasing their requirement, so one pool per
expiry is sufficient.

The allocator minimizes the total requirement under these rules and certifies its
whole-contract optimum with exact rational bounds. A linear program proposes
allocations and prices; exact integer primal checks and rational dual checks decide
whether a result is proved. Fractional contract allocations are branched into the
two possible integer ranges. Floating-point proposal values never become money.
Proportional buy-back values remain rational until the underlying total is rounded
up to the next micro-dollar. The breakdown apportions that rounding in deterministic
part order, with parts summing exactly to the requirement. Splitting a position
cannot manufacture fractional savings. The [model and proof](margin-allocation.md)
explain the constraints, certificates, and subadditivity.

`margin[].allocation` reports `exact` for a certified minimum, `bounded` when a
hard work or arithmetic limit invokes the retained conservative search, and
`legacy` for an older recorded run's allocator. Portfolio scans report null.
The terminal shows this status beside the breakdown. Limits depend on operations,
never elapsed time; symbol, expiry and variable order resolve ties deterministically.
The fallback keeps the best of the previous pairing/separate-expiry candidates and
32,768 units of joint search. Only the explicitly labeled fallback can remain
above the optimum. New run drivers record `exact_margin: true`; absent flags retain
the older algorithm when verifying or resuming a recording.

The fixed-seed regression (`0xF490123`, three expiries, 2–7 distinct series per
book, quantities ±1–2) checks 20,736 margin-account pairs and 5,423 permitted IRA
pairs: both have zero subadditivity violations and zero fallbacks. Another 2,400
small books match an independent exhaustive whole-contract oracle exactly.

A credit spread holds its width, an iron condor its wider wing (a calendar beside
it adds nothing), a long butterfly nothing, a calendar nothing beyond its debit,
a diagonal the strike difference when its long is further out of the money,
a covered call nothing beyond its shares, and a short strangle its greater side
plus the other side's value.
A butterfly beside a calendar can hold nothing. Long options and long shares are
paid in full, so a protective put needs nothing beyond its premium; short shares
no long call protects hold 150% of their value. A long that expires before its short does not cover it. (European puts can
trade below intrinsic value before expiry; the pairing ignores that.)

With `margin: "portfolio"`, `portfolio_margin_requirement` sums a separate scan for
each underlying, so gains on one underlying cannot offset losses on another. The
scan uses 11 evenly spaced points including both ends: −8% to +6% for index products
(`md::is_index_underlying`, including SPX and XSP), −15% to +15% for stocks and ETFs.
Each option is repriced with Black-76, scaling its valuation's forward with the price
and keeping its implied volatility, time to expiry and discount unchanged. American
options use the same approximation. P&L is the shocked model price less the unshocked
model price, times the signed quantity and multiplier; shares move linearly with
their underlying. Each underlying holds its largest loss, or $0.375 times the
multiplier for every option contract held, long or short ($37.50 per standard
contract), if that is larger, so even a far out-of-the-money long holds the minimum.

Buying power under portfolio margin is taken from equity, as in a portfolio-margin
account: cash plus the positions at their marks (shares at their price), less the
requirement and the reservations. Long options and shares therefore count as
collateral and the account can borrow against them, so cash may go negative, while a
short's value is owed out of the credit it brought in. The price scan and the minimum
follow Cboe Rule 12.4 and FINRA Rule 4210(g); implied volatility is shocked only with
`pm_vol_shock`, and a broker's house margin is added only with `house_margin_percent`. The scan reuses the risk snapshot's scenario
repricing but its grid is fixed, independent of `SessionConfig::scenarios`. If fresh valuations or share prices are missing, the
snapshot flags incomplete data and uses strategy margin plus the option minimum
until a complete scan is possible; normal order checks still require fresh data.
The historical `short_requirement` field carries the whole requirement in portfolio
mode, longs and shares included; `requirement` carries the same amount under a name
that fits both modes. Strategy mode pairs shares with options as above.

`TradingSnapshot::margin` and the portfolio's `margin` array show what holds the
requirement, one entry per underlying: `{underlying, requirement, parts, scan}`. Under
strategy margin each part names the positions it takes, as `{symbol, quantity}` legs
(the underlying for shares; a position can be split between parts), with its kind
(`naked`, `vertical`, `covered`, `straddle`, `short_shares`, `protected_shares`,
`worst_loss`, `cash_secured` for a cash account's or IRA's short put, or `long` for
premium and shares paid in full) and its requirement; the
parts add up to the underlying's requirement, and the underlyings to `requirement`.
Under portfolio margin `parts` is empty and `scan` gives the worst scan point
(`spot_percent`, `vol_points`), its `loss` and the contract `minimum`; while a scan is
incomplete, the entry shows strategy margin's parts with `scan` null and a requirement
that adds the option minimum. The Positions page shows it as the margin requirement
panel. The breakdown is derived from the positions and is not journaled.

**Account types and house margin.** `account_type` says what the account may hold,
as a broker's account types do. A `margin` account holds anything its margin allows. A
`cash` account sells a call only against 100 shares it holds for each contract, never
sells shares short, secures each short put with its strike in cash (`100 × strike`,
the `cash_secured` part, instead of the naked requirement), and nets no spreads: a long
covers nothing, so a credit spread holds its short put's strike and a call spread is
refused. An `ira` (limited margin) adds spreads: verticals, condors and butterflies net
as in a margin account, and a long call that expires with a short call or later covers
it, but short puts are still cash-secured when nothing covers them and no straddle
pairs. Neither uses portfolio margin, and both enforce buying power. The rule counts
what the account cannot hold (`disallowed_shorts`) before and after each order,
bracket exit, exercise (a long put's exercise sells shares), abandon or share sale, with the open
orders' sells as if filled; one that adds to it rejects with `ACCOUNT_TYPE`, so closing
is always allowed. A share sale also cannot increase a short share position, even
by one share within an existing lot. Assignment and delivery still happen as they
come; the resulting shorts can be bought back. At fill, a changed account-type
restriction cancels with `RISK_CHANGED`, naming `ACCOUNT_TYPE` in the message,
like other changed order checks.
`house_margin_percent` adds a broker's house requirement on top of Reg T: under
strategy margin, each naked short's requirement beyond its buy-back value and each
short sale's margin beyond the shares' value rise by that percentage (25 makes an
at-the-money naked short hold 25% of spot, and short shares 162.5% of their value), while spreads,
covered positions and worst losses, which hold only what they can lose, stay as they
are; under portfolio margin, each underlying's requirement rises by it. `pm_vol_shock`
widens the portfolio-margin scan to implied volatility up and down by that many points
at each price shock, as the OCC's TIMS scan does. The terminal's Start a new attempt
dialog sets all four beside the plan, and they can also stand beside a plan in
`POST /api/account/reset` and `POST /api/accounts`.

Each working order reserves what filling it now would cost: its fees, plus the change
in the positions' margin requirement, plus the premium it pays less the premium it
receives, and never less than its fees (new shorts are valued at the order's price, or
at their marks for a multi-leg order). In strategy mode, an opening buy reserves its premium, a naked
sell its naked requirement (its credit covers the buy-back value), a sell against a held
long (legging into a spread) the width less its credit, a multi-leg credit spread its
width less its credit, and a closing order only its fees. Single-leg orders see the
positions less the contracts that earlier orders, in acceptance order, already claim to
close, so two sells cannot both claim the same long; otherwise each order is measured
against the held positions alone. Portfolio mode uses the same reservation and
projected-fill checks with the scan in place of strategy margin, and because portfolio
buying power is taken from equity, the new contracts count at their marks: a buy
reserves its fees, the scan's change and what it pays above the contracts' marked value;
a sell, its fees and the scan's change less what it receives above their marked value
(either part is negative when the price is better than the mark). That is what filling
the order now would cost the account's buying power. Market orders reserve
what they would trade at now: the slipped far side, through the impact blocks a fill
would walk when the account uses impact; the displayed far side, slipped, when only
that side is quoted; and a buy with no ask at all, its last mark. Limit orders reserve
at their limits, except a marketable buy limit on an account without fill latency, which
reserves what it would pay now, as a market buy does, when that is less: a protective
limit above the ask holds the ask, not its limit (a fill that later costs more is
rechecked as every fill is). An armed buy stop without a limit reserves at least its
trigger level, which is the least it can pay once the ask reaches it, rather than
today's ask. Fees are unchanged.

An order or a fill that would reduce free buying power (cash less the positions'
requirement) must leave available buying power nonnegative, otherwise `BUYING_POWER`
(`RISK_CHANGED` at a fill). One that frees buying power is always allowed: closing a
position, buying back a short and buying protection work even when buying power is
negative. So does an order, and its fill, that would leave more buying power available
than cancelling it, because it lowers what other working orders reserve: buying back the
long that a working sell was written against re-covers that sell, and fills even when
other working sells leave buying power negative after it. A close frees buying
power unless the longs it sells cover shorts that stay open. Selling the long leg of a
spread alone needs enough buying power to carry the short it uncovers, and so does
closing a whole structure, a long butterfly say, whose longs also cover other shorts
held beside it; buy those shorts back first, or close them together as one multi-leg
order (the terminal's Positions page picks positions and does this with Close together). When legging in, a short sold before its long is naked until the long is bought.
Fills recheck against the projected ledger and cancel the remainder with `RISK_CHANGED`.

`reset_account(initial_cash, rules, reason, time)` starts a new attempt. It cancels
working orders with `ACCOUNT_RESET`, records each open position as a `Reset` closure
at its last mark (average price without one; no fill, no fee), archives an
`AttemptSummary`, restores cash and applies the new rules and pending settings. It
clears manual/daily-loss latches and re-checks `SOFT_FLOOR`, but preserves today's
`PROFIT_LOCK`, `TRADE_LIMIT`, opening-order count and active cooldown. The
order and fill history is kept; `Evaluation::first_order/first_fill` mark where the
attempt begins. Settlements, early exercises and early assignments are also recorded
as closures.

`Evaluation` carries the attempt number, start time, starting balance, peak, floor,
status, decision and its code, plus one `EvaluationDay` per finished trading date (open
and close equity, peak and floor after that day's ratchet, net realised P&L after fees,
whether it qualified toward a payout, its executions when a rule counts them, and the
plan limit that locked it, if any), appended at `roll_day`.

### Evaluation time, inactivity and opening restrictions

Optional `time_limit_days` and `inactivity_days` are whole calendar-day counts,
0 (off) to 366. `time_limit_days` applies to evaluation and verification; funded plans reject it.
The deadline is the attempt start's **plan trading date** plus N calendar days.
An active attempt fails with `TIME_LIMIT` on the first transaction whose plan
trading date is **after** the deadline. A pass decided earlier stands. Dates
follow `day_end` and the business-day calendar, including weekends, holidays and
New York daylight saving; the counts between dates are calendar days.

Inactivity works in evaluation, verification and funded phases. The attempt start is the initial
activity. Each execution of your own option order (including partial fills and
bracket exits), or your share trade, resets activity. System liquidations, expiry,
exercise, assignment, quotes, submissions and cancellations do not. More than N
calendar days between the current plan trading date and the last activity's plan
trading date fails the attempt with `INACTIVITY`, before an overdue order can execute.
The normal post-decision liquidation follows both failures. Market time drives these
rules even in empty batches; missing marks do not postpone a calendar deadline.
The [multi-day deadline rehearsal](scenarios.md#rehearse-an-evaluation-deadline)
shows how to set a one-day window in Replay, reach the final allowed date, and
step past it to reproduce `TIME_LIMIT` and verify the saved run.

`underlyings` is an optional list of up to 32 distinct symbols of 1–12 uppercase
letters, digits or dots; empty allows all. It restricts opening or adding options
and shares by **underlying**, not option root: `SPX` allows both SPX and SPXW options.
Other openings receive `INSTRUMENT_NOT_ALLOWED`, with that underlying in `scope`.
Reducing orders, managed exits and system closes remain allowed.

`trading_start` and `trading_end` are optional `HH:MM` New York wall-clock times.
Set both, with start before end, or leave both null/omitted. Start is inclusive,
end exclusive; `00:00` is valid and `24:00` is valid for the end. The window applies
every day, including overnight and curb sessions; product-session checks still
apply. Outside it, openings receive `OUTSIDE_PLAN_HOURS`, with the current New York
`HH:MM` time in `actual`, the nearest window boundary as `HH:MM` in `limit`, and the underlying in
`scope`. At the first transaction outside the window, working opening orders cancel
with that reason before matching. A gap into the next day’s window still cancels
orders that crossed the previous window’s end. Reducing orders and bracket exits
keep working under their normal sessions and expiry rules.

Account `evaluation` adds `time_limit_days`, `deadline`, `days_left`, `last_activity`,
`inactive_days` and `inactivity_deadline`. Disabled rules report null; progress is
also null until the attempt starts. `days_left` is never negative. Dashboard shows
the time left and warns within seven calendar days of inactivity expiry. Custom
plan editing, plan facts, Rules and option, strategy and share tickets show the
restrictions; tickets block known openings. Old plans default to off and keep their
existing journal bytes.

### Mandatory flat time and no overnight holds (F6)

Custom plans may set `flat_time` to `HH:MM` New York time (`00:00`–`23:59`),
strictly before `day_end`, or null/omitted to disable it. Internally it is an
optional integer minute after New York midnight. The first transaction at or
past that time on a plan trading date, before its day ends, cancels working
opening orders with `FLAT_TIME` **before matching**. Empty quote batches and
clock advances count, including a day's first transaction already past the time.
Reducing orders and managed exits remain working, capped to the positions they
protect, and cancel when those positions close.

The reducer closes options through the same combo-aware planner as Flatten,
splitting closes at `max_order_contracts`. Option closes are system market IOCs
with client IDs `system:flat_time:N`; shares close at their fresh underlying price
with stock-fill source `rule`. Regular sessions, executable liquidity and fill
latency still apply. Missing prices or exhausted size leave remaining holdings
pending for subsequent quotes, including after day end. Options awaiting
settlement stay untouched (and retain covers required by the flatten planner).
The trigger date (`evaluation.flat_time_day`) and unfinished-close flag
(`flat_pending`) are journaled: recovery resumes remainders without repeating the
once-per-date trigger. These internal fields are omitted until used.

Until that plan day ends, option and share openings and their previews are refused
with `FLAT_TIME`: `actual` is the current New York `HH:MM`, `limit` is the configured
flat `HH:MM`, and `scope` is `account`. Order submission and opening preview
endpoints return HTTP 422 with the usual `error` envelope for `FLAT_TIME`.
Reducing orders remain allowed. After `day_end`, the next plan date has not reached
its flat time yet; its overnight session is not incorrectly locked.

`no_overnight` is independently optional, boolean, default false. On `roll_day`
or any transaction crossing a plan trading-date boundary, an active evaluation,
verification or funded attempt holding shares or any option not awaiting settlement fails
with `OVERNIGHT_HOLD`, before executions and without requiring marks. An already
decided attempt stays decided. The normal post-decision liquidation follows:
`system:overnight:N` market IOC option closes and stock fills with source `rule`,
retrying remainders on executable quotes. Without `flat_time`, the trader must
flatten manually before rollover. This position check runs before calendar or
equity verdicts on the same transaction. Settlement-pending options alone do not
cause failure.

Account `evaluation.flat_time` is the configured `HH:MM` or null;
`evaluation.flat_now` is true from that time until `day_end` on the current plan
trading date, otherwise false. Dashboard warns within 30 minutes beforehand and
announces the opening block afterwards. PlanEditor, plan facts, Rules, and option,
strategy and share tickets explain these restrictions; tickets leave reductions
available. Invalid `flat_time` or non-boolean `no_overnight` returns HTTP 422
`INVALID_RULES` on account create/reset. Off defaults are omitted from journals,
so old plans retain identical bytes and older journals recover unchanged. Replay
driver and scenario revisions are unchanged.

### News blackouts and holding restrictions (F17, F59)

Both rules use one **saved plan event calendar**, `events`, with at most 256 entries.
An event has `kind` (`news`, `earnings`, `ex_dividend`, `split`), `time`, optional
`label` (up to 64 characters), and an underlying `symbol` using the same 1–12 uppercase
letters, digits or dots as `underlyings`. Only news may omit the symbol, meaning
all underlyings. News time is an exact ISO-8601 UTC timestamp ending in `Z`;
other events use a valid `YYYY-MM-DD` date. Earnings alone accepts `session`:
`before_open` (default) or `after_close`. Input is sorted by time, kind, symbol,
session and label and identical entries are deduplicated. Unknown fields, kinds,
invalid dates, symbols, sessions or limits receive 422 `INVALID_RULES` with an
explanation. The event list is journaled at create/reset, including nanoseconds. HTTP news
times omit trailing fractional zeros (whole seconds have no fraction); significant
nanoseconds are preserved. Older nine-digit journal times remain readable.

`news_before_minutes` and `news_after_minutes` are integers 0–240. Both zero
turn the rule off. Each enabled news window is **[time − before, time + after)**.
Inside it, scoped opening orders and previews receive `NEWS_BLACKOUT`; working
opening orders cancel before matching at the first transaction inside the window.
Reductions and exits keep working. `news_action: block` is the default;
`flatten` also attempts one system liquidation per event (`system:news:N`). A gap
that skips an entire window does nothing retroactively. Overlapping windows are
independent and all active scopes restrict openings.

`hold_restrictions` is a set of `weekend`, `earnings`, `ex_dividend`, `split`;
empty disables it. `hold_cutoff` is New York `HH:MM`, default **15:45**, strictly
before `day_end`. The last holding date is the last business date before Saturday
for weekend (Thursday before a Friday holiday); before the event date for earnings
before open, ex-dividend and splits; the event date for earnings after close. The business-day calendar
is frozen into holding-enabled rules at create/reset for deterministic replay.

At the first transaction at or after that cutoff, even after a gap, scoped opening
orders cancel with `HOLD_RESTRICTED` and positions receive a single system-close
attempt per holding date, restriction and scope (`system:hold:N`). Weekend covers all underlyings; corporate events cover
both options and shares on their underlying. Openings stay blocked through that
holding date's `day_end` (exclusive). System closes use the existing executable
liquidity and integer micro-dollar money path; they do not invent fills when the
market is closed or quotes are stale. Expired positions awaiting settlement are
skipped. A position still held across the restricted boundary is recorded and
fails the active attempt with decision `HOLD_RESTRICTED` at the next rollover
(explicit `roll_day` or a transaction crossing the plan trading date), before any
late close can remove that evidence. Existing post-failure liquidation then applies.
For mandatory daily closes and the independent general overnight prohibition, see
[Mandatory flat time and no overnight holds (F6)](#mandatory-flat-time-and-no-overnight-holds-f6).

Combined F6/F17/F59 plans check crossed holding boundaries before any executions,
using the positions held at each boundary and excluding options already awaiting
settlement then. At rollover, **`OVERNIGHT_HOLD` takes precedence over
`HOLD_RESTRICTED`** when both apply; the broader no-overnight rule decides the
attempt. Both use the same position check and combo-aware system-close planner,
including share closes, order-size splitting and delayed-order ownership. A decided
attempt keeps its decision and liquidation label on subsequent quotes.

Flat time, news `flatten` and holding cutoffs share that close planner. A close
already submitted by an earlier transaction retains its `system:flat_time:N`,
`system:news:N` or `system:hold:N` label, including while fill latency is pending;
later triggers do not submit a competing close for its legs. If several triggers
first act in the **same transaction**, flat time acts first, then calendar windows
in start-time/key order. This also defines precedence after a gap that reaches
several cutoffs together. A calendar with no new close in a held scope preserves F6’s
normal equity-observation order. Shares and fully closed options cannot close twice.
Calendar news/holding closes still require fresh two-sided quotes. Flat-time and
overnight closes retain Flatten's existing ask-only reduction policy (buy back a
short at the ask; dispose of an unbid long at zero).
Calendar actions remain once-only; mandatory flat time retries remaining holdings,
and failed attempts retry liquidation with the decision's label. A new retry is
labelled by the rule submitting it; existing orders are never relabelled.

These rules also compose with F39 contract and funded account-size scaling.
System closes bypass contract caps; calendar cancellation releases working opening
reservations. Realised profit from a flat-time, news or holding close enters the
next session's contract-cap calculation and the funded size review at day rollover.
A holding or overnight failure prevents capital growth even if that review's
profit, payout and finished-day requirements are met. Reset starts both scaling
progress and calendar action tracking afresh, retaining both rule sets in the
archived attempt; recovery restores them from the journal.

HTTP evidence: `NEWS_BLACKOUT.actual` is the current UTC timestamp and `limit`
is the window end, with `scope` the underlying or `account`. For `HOLD_RESTRICTED`,
`actual` is null, `limit` is the cutoff `HH:MM` ET, and `scope` combines the kind
and underlying (`earnings:SPY`) or account (`weekend:account`). The reducer journals
timestamps as nanoseconds and cutoffs as New York minutes. Account `evaluation`
adds `next_event` (earliest active/future restricting window, or null) and
`active_events` (all active windows): kind, symbol/label or null, UTC start/end and
active flag. Holding start is its cutoff; end is its plan day end.

Custom PlanEditor edits all fields and event rows. Import server calendar reads
`GET /api/calendar/events`; import known dividend ex-dates reads the selected
underlyings' summary expiry dividend calendars (all subscribed underlyings when
no allowlist is set). Imports become plan rules only when saved. Plan facts and
Rules explain the restrictions, Dashboard reports the next/active window, and
chain, option, strategy and share tickets block scoped openings while reductions
remain available. The daemon's optional `--event-calendar FILE` never changes
an existing account. Rules at off defaults and empty action state are omitted
from journals, preserving older plans' exact bytes; old journals still recover.

### Plan objectives and the daily loss limit

A plan's **trading day** ends at `day_end_minutes` New York time (`plan_trading_date`,
from `md::trading_date(time, minutes)`): a business day's own date until then, the
next business day after it and over weekends and holidays. 17:00 is the default, as
for every other trading date; 18:00 matches a day that starts at 6 pm ET (or at
midnight in central Europe), and 24:00 keeps the evening with the day it follows. The
engine rolls each account over on the first market batch of its own plan's new
date, and `TradingSession::trading_date(time)` reports it. Rollover closes the
evaluation's day, so the day's boundary decides which day an overnight trade, a
day's loss and the end-of-day ratchet count toward. Equity samples carry the plan's
day.

**Scaling plan.** An optional sizing constraint in evaluation, verification and funded phases, independent of pass objectives and per-order limits. The limit stays fixed through the trading day and changes at rollover from closed balance less starting balance, including losses and withdrawals. See the funded section below for thresholds, reservations and enforcement.

**Daily loss limit.** On every observation of fully marked equity, the limit's level is
its reference less `daily_loss_limit`. The reference is the day's opening equity, its
opening closed balance (the balance now less today's net realised P&L, so a payout today
moves it down as it moves the opening equity), the higher of the two, or the day's
fully marked equity high (from its opening equity). Touching the level (`equity <=
level`) either fails the attempt (`Fail`, code `DAILY_LOSS_LIMIT`, liquidated as
`system:daily_loss:N`) or locks the day (`Lock`). A locked day records
`Evaluation::day_lock = DAILY_LOSS_LIMIT` with its time and a `day_locked` event,
cancels every open user order with that code, liquidates every position and share like
a decision (`system:daily_loss:N`, retried on later updates until flat), and refuses
orders that open or add with `DAILY_LOSS_LIMIT` until rollover; closing orders are
still accepted. The attempt stays active and the drawdown floor still applies. Rollover
clears the lock and records it in the finished day's `locked`. The limit is checked
once a day: after a lock it does not act again until the next day. The personal
`max_daily_loss` above is separate and still trips the reduce-only latch.

**Objectives.** `evaluation_objectives` lists what a pass needs, each with whether it is
met, where it stands and what it requires: `PROFIT_TARGET` (dollars of profit under the
basis; a closed-balance target also needs the account flat, with no option positions or
shares), `MIN_TRADING_DAYS`, `MIN_PROFITABLE_DAYS` (days) and `CONSISTENCY` (the best
day's percent of its basis against the limit). The attempt passes on the first
observation that meets all of them, so a trader who reaches the target early keeps
trading, and protecting it, until the other objectives are met. Days are the finished
`days[]` and the day in progress, which counts as soon as it qualifies: a trading day
once one of the trader's own orders executes on it or the trader trades
shares, a flatten's included (bracket exits count; the account's liquidations,
expiry closes, settlements, exercises and assignments do not), and a profitable day once
its profit so far reaches `profitable_day_profit`. Holding a position over a day does
not make it a trading day. Each day's profit follows `profit_basis`: its equity
change, or its net realised P&L. The consistency rule holds when `best day × 100 <=
percent × basis`, exactly, where the basis is the attempt's profit or the profitable
days' total; with no profitable day it holds. A breach never fails the account: it holds
the pass back, which is the same as raising the target to `best day × 100 / percent`
(reported as `consistency_target`, rounded up to the cent) or trading more days. The
reducer counts each day's executions only while `min_trading_days` asks for them, and
journals the plan rules, a decision's code (other than the target's or the floor's,
which the status implies), the day's lock and its executions only when they are set,
so a plan without these rules keeps the journal bytes it had.

The following pass objectives apply equally in evaluation and verification. Minimum hold, direction and volume-share restrictions also apply in verification, with the same live-account and market-data requirements.

**Trade consistency (F29).** `TRADE_CONSISTENCY` is satisfied with no profitable closed whole
option trade; otherwise it requires `best_trade.pnl × 100 <= trade_consistency_percent ×
attempt profit`, with exact integer micro-dollar products. A trade is closed only
when every round trip in its group is flat; rolls and adjustments stay one trade.
Shares are separate from whole option trades and do not enter this measure. Net P&L
includes all fees, settlement, expiry, assignment and account liquidation closures.
The best trade is reported as `best_trade: {id, pnl}` (null before one qualifies),
with the earliest opening fill breaking ties. Objective `actual` is its percentage
of attempt profit (null while profit is nonpositive), `required` is the limit, and
the message names the trade and its net P&L. A breach only holds the pass back.
Funded plans ignore this objective, like day consistency and minimum days. Pass
odds retain the attempt's best trade and simulate no additional closed trades.

**Minimum trades (F30).** `MIN_TRADES` reports actual and required counts of fully
closed whole option trades. Every group counts once, only after all its round trips
are flat, including losing and breakeven trades. Settlement, expiry, assignment,
protective exits, flatten and the account's own liquidation all count; shares are
outside option trade groups. Counts reset with the attempt and recover from its
journal. Funded plans ignore this evaluation/verification objective. Pass odds keep the count so
far and add no simulated trades; a still-unmet minimum therefore prevents a simulated
pass. The API reports `closed_trades` (null while this rule is off).

**Holding time and microscalping (F61).** An ordinary user order that reduces or
reverses any too-young option position or share holding is refused with `MIN_HOLD`.
The age starts at the round trip's first opening fill; adding and partially closing
do not restart it. Time is elapsed market timestamps (including overnight), never
wall-clock time. Exactly `min_hold_seconds` is allowed. Every closing leg is checked
before accepting a combo. Preview returns the same refusal and seconds evidence.
Messages show at most one decimal place without trailing zeros (for example,
`Held 12.5 seconds`); numeric `actual` and `limit` retain full precision.
Protective conditional triggers, bracket stops/targets, OCO and trailing stops,
flatten (including limit flatten), daily-loss exits, expiry cutoff, liquidation,
exercise, assignment and settlement remain executable. Minimum hold applies in
evaluation, verification and funded phases.

`MICROSCALPING` sums only positive net P&L of completed option and share round trips
whose final close is strictly before `microscalp_seconds` from first opening fill.
Option fees and share dividends are included; losses do not offset short profits.
Partial closes wait for the final close; holding exactly the threshold is not short.
Protective and system closures are included. A pass needs
`short_profit × 100 <= microscalp_percent × attempt profit` under `profit_basis`,
using exact integer micro-dollar products. Zero short profit satisfies the condition;
a breach only holds the pass back. Objective actual/required are percentages (actual
null while attempt profit is nonpositive); `short_profit` reports the dollar sum,
null when disabled. Funded plans ignore this objective. Set both microscalp fields
together; percent zero disables (the threshold may be retained). Pass odds keep short profit so far and add no simulated
round trips, just as they keep the whole-trade count and best trade.

**What liquidation costs.** A pass on marked equity liquidates at the bid or ask, which
can leave less than the equity that passed. `TradingSnapshot::exit_equity` is what
closing every position now with a market order at the displayed quotes, the account's
slippage and impact would leave after fees, using the account's fee schedule when it
has one (a long nobody bids for counts as nothing).
The account view shows it, the cost against equity, and, once a decided attempt is
flat, its equity then and the liquidation's cost against the decided equity.

Pass odds run the same `evaluate_plan` on each simulated observation, treating the
path as equity and closed balance alike, flat at every observation, with a trade on
every simulated day and the attempt's finished days counted toward its objectives; a
lock ends that simulated day at its level.

**In the terminal.** Starting an attempt offers a **Custom plan** beside the presets:
it starts from a preset's rules and edits the name, starting balance, target and its
basis, the floor (trailing every high or each close, or static) and its lock, the daily
loss limit with its basis and action, the consistency rule, the minimum days, the
trading day's end and the strategies, then resets with those rules (the server checks
them again). The Rules page explains each rule in force with the account's numbers, and
its plan table adds the daily loss limit and what a pass needs. The Dashboard lists the
objectives with where each stands, the best day and what the consistency rule asks of
it, a daily loss tile with its level and room, the closed balance on a balance plan,
and what closing every position now would leave; a locked day gets a banner, a
`locked today` badge, rule alerts and ticket notices that only closing orders work, and
a decided attempt names its rule and what liquidating cost against the decided equity.

#### Prop rules: open-loss caps, a balance floor, best-position and minute-volume caps

These rule types, drawn from the shapes of common futures and CFD prop-firm terms, are
configurable and off by default; no built-in preset uses them, and their parameters are
the plan author's, never a claim to match any named firm. Each is recorded in the
journal only when set, so plans without them keep their journal bytes, and each is an
`AccountRules` field, so overriding one takes an account off its preset (its `plan_id`
becomes null) exactly as financing rates do.

**Open-loss cap** (`open_loss_cap: {basis, amount, percent, action}`, null when off). On
every fully marked observation of an active attempt, each open trade is measured at its
marks: its open loss is the negative of its unrealised P&L summed across its legs (fees
excluded), so a hedged spread's profitable leg offsets its losing one. A trade is the
trades view's whole trade (an order's legs, and the round trips rolls and `group` join
to it); each underlying's shares are a trade of their own. The cap is `amount` with `basis: "amount"`;
`percent` (1–100) of the start-of-day profit balance with `day_profit` (the day's
opening closed balance above the starting balance; while that is not above zero the same
percentage of the drawdown room stands in, and without a drawdown rule there is no cap
that day); or `percent` of the drawdown room with `drawdown_room` (the day's opening
equity above the floor in force, which needs a drawdown rule). Percentages round down to
whole cents. A trade whose open loss reaches the cap is journaled once per breach as
an `open_loss_cap` event (`trade`, `symbols`, `symbol` (the first of them), `loss`,
`limit`, `action`), its round trips are remembered in `evaluation.open_loss_trades` for
payout reviews, and its `action` applies: `flatten` has the account close the whole
trade (`system:open_loss:N`, retried on later quotes like any rule close; shares at their
fresh price in the regular session) as Flatten closes positions, a short and the long
that covers it as one two-leg order and other shorts before longs, so no leg is left
naked, and refuses adding to its legs with `OPEN_LOSS_CAP` while that close is pending;
`block_adds` refuses orders and share trades that add to its legs with `OPEN_LOSS_CAP`
until an observation finds it back within the cap (reductions always work); and `fail`
fails the attempt with decision code `OPEN_LOSS_CAP` and liquidates as any decision
does. Trades with a leg without a mark are not measured. The account view reports
`evaluation.open_loss: {limit, positions, trades}` (null without the rule), `positions`
listing the legs of the trades over the cap.

**Balance drawdown basis** (`drawdown_basis: "equity" | "balance"`). With `balance` the
floor's peak follows, and the floor is checked against, the closed balance (cash plus the
positions' cost: realised P&L after fees) instead of fully marked equity; an end-of-day
ratchet uses the day's last closed balance (`evaluation.day_close_balance`, kept only
under this basis). Open losses then never touch the floor until closed, which is why such
plans usually add an open-loss cap. The daily loss limit, risk room displays and breach
estimates keep their own bases. Set it per phase: a two-step program's evaluation can
trail equity while its funded step measures the balance.

**Best-position cap** (`best_position_percent`, 1–100, evaluation and verification with
a target). A closed whole trade (rolls and adjustments count once, fees included, as for
trade consistency) counts at most this percentage of the profit target toward it; the
excess over that cap, summed over the attempt's closed trades, is left out. The
`BEST_POSITION` objective is met while no trade exceeds the cap, or once attempt profit
less the excess still reaches the target. It holds the pass back and never fails the
account; open trades count at their marks in full until they close. The account view
reports `evaluation.best_position: {cap, excess, counted_profit, trade}`.

**Minute-volume cap** (`minute_volume_percent`, 1–1000). An opening order's contracts per
leg may be at most this percentage of that contract's volume in the minute before
acceptance: the server's cumulative volume report within that minute less the last one a
minute or more old, on the same market date. Without both reports (a feed that reports no
option volume, a contract that has not traded, or a snapshot feed reporting less often
than each minute) the order is refused with `MINUTE_VOLUME`, since the cap cannot be
checked; the refusal's evidence carries the contract, its opening contracts, the volume
(null when unknown) and the percentage. It is an acceptance check in the server, like the
direction rules: fills are not rechecked and reductions are exempt. In a replay or
backtest it reads the volume the recording carries, so it needs a recording with option
volume.

#### Comparing rule sets on the recorded attempt

`POST /api/account/rules-compare` (read access, live accounts) replays the account's
recorded attempt against its own rules and up to seven more: presets by ID in `plans`,
and custom rule sets in `rules` (`{name, initial_cash, rules}` in the rules shape;
`include_current: false` leaves the account's own out). `trading::replay_rules` walks each
finished day as its open, its recorded low and high in timestamp order and its close,
then the day in progress to the current marks, through the same `evaluate_plan` the
reducer runs. Each set starts at its own starting balance and moves by the recorded
dollar P&L, everything payouts took out added back (the amounts, forfeited profit and
reset removals); the closed balance moves by each day's realised P&L
at its close, so a balance target counts only at a close. Days count as trading days with
recorded executions or, where the account did not count them, realised P&L. The attempt's
closed whole trades count toward trade objectives from when they closed. Nothing is
re-executed: a lock ends that day at its level and later days move from there, and rules
on orders, positions or the clock (open-loss and minute-volume caps, contract and trade
risk caps, strategies, hold times, instruments and hours, flat time and holding rules,
and the payout rules `payout_review`, `after_payout` and `reset_percent`) are listed per
set in `not_replayed`. Each set reports `status`, `code`, `message`, the
`decided_day`, `decided_at` (a recorded extreme's time, null when the decision came at a
day's open or close, whose times are not recorded), `decided_equity`, `locked_days`,
`closest_floor` and per-day `{day, outcome, close, floor}`, where `outcome` is `active`,
`locked`, `passed`, `failed` or `after`. The Rules page's **Compare rules** panel shows the
sets side by side and a day-by-day grid of where they part. Unknown presets or more than
eight sets are 400 `INVALID_REQUEST`; invalid custom rules are 422 with their code.

#### Versioned rule packs

A rule pack is a JSON plan file: `{format: "openport.rule-pack", version: 1, id, name,
effective_from: "YYYY-MM-DD", source: "https://…", verify, notes?, plans: [{id, name,
summary?, initial_cash, rules, unlocked_by?}]}`. `verify` says how and when the terms were
checked against `source`. `POST /api/rule-packs/validate` (read access; nothing is stored)
checks it: the format and version, a lowercase ID, an http(s) source, a date, 1–16 plans
with unique IDs and names, no built-in preset's ID or name, `unlocked_by` naming another
plan in the pack without a cycle (`where: "plans"`), and each plan's rules through the same parser and `validate_rules` as a
reset. It returns `{valid, errors: [{where, message}], warnings, pack}`, the pack
normalized with every plan's rules as the server reads them (a normalized pack validates
to itself), and warns when `effective_from` is after today. The terminal keeps imported
packs in that browser only, lists them with their effective date, source and verify
note, exports them as files, and offers their plans as starting points for a custom plan:
starting an attempt sends the pack plan's rules as custom rules, which the server checks
again, so a pack plan is never a preset and its terms are the author's, not this
project's. **Template from my rules** fills a pack from the account's own rules.

#### Plan presets with objectives

Two preset families use these rules, at 25K, 50K and 100K, with any strategy, strategy
margin, buying power and the five-minute expiry auto-close of the other evaluations:

| Preset | Target | Floor | Daily loss limit | To pass | Day ends |
| --- | --- | --- | --- | --- | --- |
| `static-*` | 10%, on the closed balance | Static, 8% below the starting balance | 4% below the day's opening balance; fails the attempt | At least 4 trading days | 18:00 ET |
| `locking-*` | 6%, on equity | 4% trailing each close, locking at the starting balance | 2% below the day's opening equity; closes every position and locks the day | Best day at most 50% of the total profit | 17:00 ET |

They have no funded counterpart. Like every preset, their parameters are this
project's own, modelled on common prop-firm terms; custom rules can combine the
same rules any other way.

#### Two-step programs (F27)

The `two-step-25k`, `two-step-50k` and `two-step-100k` presets are challenge
steps. A current pass unlocks `two-step-verify-25k` (or the matching size), whose
pass unlocks `two-step-funded-25k`. `GET /api/plans` exposes `phase`, `unlocked_by`
and `unlocks` (preset IDs); `GET /api/account` exposes `next_plans`, an empty list
unless its current passed preset unlocks a next step. Resetting consumes that
current pass; an older attempt's pass cannot unlock a different step later.
Custom rules can use `phase: "verification"` and the same optional target, floor,
daily-loss and objective rules as evaluation. Custom backtests also accept
verification with a positive target, contract scaling and program fees; payouts
and account-size growth remain funded-only. Locked verification and funded preset
IDs are still refused by backtests. They cannot borrow a preset's name
with different trading rules, or use read-back rules to bypass its lock.

| Step | Phase | Closed-balance target | Minimum trading days | Loss rules |
| --- | --- | --- | --- | --- |
| Step 1 of 2: challenge | `evaluation` | 10% | 4 | Static floor 8% below start; a 4% loss from daily opening balance fails |
| Step 2 of 2: verification | `verification` | 5% | 3 | Same as challenge |
| Funded | `funded` | None | None | Same static floor and daily loss; simulated payouts |

All sizes allow any strategy, enforce strategy buying power, auto-close five minutes
before expiry and end the trading day at 18:00 New York time. Funded payouts use the
existing 8 qualifying days, $100/$150/$200 qualifying profit, 50% withdrawal,
80% trader split and size-based minimum/caps. These are **this project's own
illustrative parameters**, modelled on common two-step programs, not any firm's
current offering or a claim to reproduce FTMO, The5ers, BluSky or Earn2Trade terms.
Existing preset IDs and trading rules are unchanged.

Creation (`POST /api/accounts`) and replay start accept only unlocked presets.
Use the passing account's normal reset to enter a locked step. The terminal always
offers verification, locked with **Pass ... to unlock** until the challenge passes;
Dashboard then offers **Start verification**, opening the reset confirmation with
that plan selected. Funded steps (including `two-step-funded-*`) and **Start funded
account** stay hidden unless `showFundedAccounts` is on or the account is already
funded. A funded step still needs its prerequisite pass.
The reset dialog, Rules and attempt history show the phase and its pacing.

#### Program costs and reset limits (F64)

`evaluation_fee`, `reset_fee` and `activation_fee` are nonnegative `Money` values
(decimal strings in HTTP; integer micro-dollars in the reducer and journal).
`max_resets` is a whole number from 0 to 1,000,000; **0 means unlimited**. All four
fields default to zero for a fresh purchase. A same-plan reset retains each current
setting unless explicitly supplied. Set them beside a preset in create/reset requests, or
inside custom `rules`; outer settings override the corresponding rule fields.
These bookkeeping costs never change cash, equity, floors, trading fees or outcomes.

- A new account or a different-plan purchase pays the destination `evaluation_fee`.
- Restarting the same plan, whether active, failed or passed, pays its `reset_fee`.
  The first purchase uses zero resets; each restart adds one. The effective
  `max_resets` (current, or explicitly overridden) applies to the existing count. Changing cost terms cannot clear it.
- Entering a preset unlocked by the current pass pays the destination
  `activation_fee` instead of its evaluation fee, and starts a new reset count.
- A different plan starts a new purchase and a fresh reset allowance. Switching
  away and back is another purchase, even if this account used that plan earlier.

Same plan means the same starting balance and all `AccountRules`, including its
name, phase, time and inactivity limits, allowed underlyings, trading hours,
mandatory flat time, overnight policy, contract `scaling` and `size_scaling`, after ignoring execution settings (slippage, latency, impact,
inside fills and the option fee schedule), broker margin settings (mode, account
type, house percentage, volatility shock), and these four program cost settings.
That is also the preset-identity comparison used for locks and plan-name checks.
Account-size growth retains the same funded plan: compare the **original purchased
starting balance**, original drawdown and daily-loss amounts, and original lock
balance. Restarting uses those original terms, charges the reset fee and consumes
the existing allowance; growth never replenishes resets. Submitting the displayed
live rules unchanged with the original starting balance also restores the original
loss and lock amounts; it does not adopt the scaled limits as new originals. Buying the grown balance
as a new starting balance, or changing either scaling rule, is a different purchase.
Live and archived rules still report the actual loss amounts and starting balance
at that time. Size-scaling progress exposes `original_max_drawdown` and
`original_daily_loss_limit` alongside `original`; archived journal summaries retain
that progress to recover the comparison exactly, without reversing rounded ratios.
Counts are derived from consecutive archived attempts with matching recorded
rules; an old summary without rules ends that known streak. A `RESET_LIMIT`
refusal is journaled without cancelling orders, closing positions, archiving an
attempt or charging a fee.

| Two-step size | Challenge purchase | Reset in any step | Verification activation | Funded activation | Resets per step |
| --- | --- | --- | --- | --- | --- |
| 25K | $100 | $50 | $0 | $50 | 2 |
| 50K | $200 | $100 | $0 | $100 | 2 |
| 100K | $400 | $200 | $0 | $200 | 2 |

Verification/funded presets have zero evaluation fees because ordinary entry uses
the prerequisite pass. These costs are project illustrations, not quoted firm
prices. All older presets retain zero fees and unlimited resets.

The account's `costs` object has `evaluation`, `reset`, `activation`, `total`,
`resets_used`, `resets_left` (null for unlimited), `payouts_received` (sum of
`trader_share` across attempts), and `net` (payouts minus total costs). It also
reports current `fee_charged` and `fee_kind`; each archived attempt exposes its
starting `fee_charged` and `fee_kind`. Zero charges have a null kind. Dashboard
shows costs and net; reset confirmation quotes the selected terms and remaining
resets, and disables an exhausted restart. The custom editor edits all four terms.
Funded-step cost displays, including their activation fees, follow the same visibility
as funded plans; verification costs remain visible.

Session start and account reset journal the actual nonzero fee with its kind.
The current charge, archived charges and archived payout totals are omitted at
zero; cost rule fields are omitted at their defaults. Old records recover and
re-encode unchanged, with absent charges treated as zero. Payout totals are also derived from journaled payout events, so payouts before
older resets remain included without adding fields to old state or snapshot bytes. No replay driver bump is needed:
old defaults produce the same start bytes, and recorded state/deltas remain readable.
New HTTP reset inputs opt into these semantics with an internal `program_costs` flag;
absent on old recorded inputs, it preserves old funded-reset locking and payout
archive behavior during re-execution.

Each reset archives the attempt in the account view's top-level `attempts[]`.
Alongside its plan, start/end, starting balance, final equity, status and decision,
it keeps the full `rules` at reset (the same JSON shape as the current account's
rules), `decided_at`, `decided_equity`, `peak` and `floor`. Decision time/equity are
null for an active attempt reset; floor is null without a drawdown rule. Summaries
from older builds return null for these unrecorded fields. The journal omits absent
fields, so old summaries re-encode unchanged. The Dashboard's Attempts list shows
the decision time/equity and a compact rules line, including fill settings.

### Equity extremes and history

Each finished `EvaluationDay` keeps `low_equity`, `high_equity`, `low_at` and
`high_at`. Current-day values use the `day_` prefix in `evaluation`. Only fully
marked equity contributes, and ties keep the first time. `closest_floor` is the
smallest equity minus floor observed through the attempt's decision, with
`closest_floor_at`. The deciding observation counts, then both fields freeze:
post-decision fills and later marks do not count. A drawdown failure therefore keeps
`closest_floor = decided_equity - floor` (zero or negative), before liquidation costs.
Older records leave these values null and times absent rather than inventing history.

The engine stores marked equity once per market minute, at every fill, when the
floor changes and when equity first reaches the target or the floor. Every decision
also keeps a sample at `decided_at` with exactly `decided_equity`, before any
liquidation fill sample at the same time. This includes intraday and end-of-day
plans, pass or failure, decisions without fills and `DAILY_LOSS_LIMIT` with action
`Fail`; minute sampling never discards this observation. Atomic spread legs
share their post-execution equity. When one update both marks equity and executes
(a pass or breach liquidating at the bid, an exit on a new high), the mark before the
execution is kept at the same time, so a passed attempt's history reaches the equity
that passed it and a trailing floor's new level shows where it was set. Share
deliveries and closes use their committed transaction mark; when several positions
settle at one time, their deliveries share the first fully marked equity after them.
History is
appended beside each journal as `<journal>.equity.csv`, reloaded on start and compacted
to the current attempt plus at most 90 days and 100,000 samples from older attempts.
The CSV columns are nanosecond timestamp, attempt, equity micros, floor micros, peak
micros, target micros, tomorrow-floor micros, option fill ID and share fill ID
(both zero for a minute mark). The earlier eight-column format defaults share ID to zero.
Absent levels are `null`. Invalid/torn rows are skipped and reported. Storage errors
are exposed as `error` and do not stop trading. Replays without a journal store no
history. The server never fills in a gap from its downtime.

`GET /api/account/equity?from=&to=` returns `{samples, error}`. Optional bounds are
inclusive UTC ISO timestamps; `account=ID` selects an account. Each sample carries
`time`, trading `day`, `attempt`, `equity`, `floor`, `peak`, `target`, `tomorrow_floor`
and `fill` (share IDs start with `s`). The Dashboard keeps its daily chart and adds a day-selectable intraday
chart, with gaps, ratchet markers and the target. End-of-day plans also show the floor
that would apply tomorrow if the day ended at that sample; once the attempt is decided
or the floor is locked, that is the floor itself, since rollover no longer moves it. A
finished replay serves its history read-only at
`GET /api/replay/history/ID/account/equity`.

### Funded accounts and payouts

A funded account is an attempt whose rules have `phase = Funded`: no profit target
(`validate_rules` rejects one), the usual drawdown floor, and usually a `lock_balance`
equal to the starting balance so the floor stops trailing there. Touching the floor
fails the account like an evaluation; it stays closed until a reset. `PayoutRules`:

| Field | Meaning |
| --- | --- |
| `qualifying_profit` | A finished day qualifies with at least this much net realised profit (realised P&L less fees, from rollover to rollover) |
| `qualifying_days` | Qualifying days needed since the previous payout; at least 1 on a funded account |
| `withdrawal_percent` | Share of profit above the starting balance one payout may take |
| `split_percent` | The trader's share of each payout |
| `minimum` | Smallest payout |
| `buffer` | Optional nonnegative money above `Evaluation::starting_balance` that must remain as equity after a payout; zero (default) disables |
| `buffer_payouts` | Integer 0–100: apply the buffer to the first N payouts; zero (default) applies to every payout |
| `caps` | Largest payout by payout number; the last cap repeats; empty is uncapped |
| `consistency_percents` | Optional list of up to 64 integer percentages, each 1–100. Best positive day at most this share of cycle net realised profit. Indexed by payout number; last repeats; empty (default) disables |

**Scaling plan (F39).** `AccountRules::scaling` is an optional list of up to 16
`ScalingStep {profit, contracts}` entries. The first profit is zero, subsequent
profits strictly increase, and contracts are non-decreasing integers from 1 to
10,000. Empty disables it. Either phase may use it; existing presets keep their
original rules. Invalid steps are `INVALID_RULES`; wrong JSON types are
`INVALID_REQUEST`. For example, custom rules may include:

```json
{"scaling":[{"profit":"0.00","contracts":2},
            {"profit":"1500.00","contracts":3},
            {"profit":"2000.00","contracts":5}]}
```

An attempt starts at the zero-profit step. `Evaluation::scaling_limit` is the
journaled limit in force; zero means none. Only `roll_day` changes it, selecting
the highest threshold reached by the finished day's **closed balance less the
attempt's starting balance** (`PlanInputs::balance`, excluding unrealised P&L).
The first step still applies below zero profit. The next session uses the new
limit, up or down; payouts lower this balance too. Changes emit `scaling_limit`
with `old`, `limit` and exact `profit` (journal money in micro-dollars). Empty rules
and a zero limit are omitted from journal JSON, preserving older journals' bytes.

The cap counts the sum of absolute option position quantities: each leg of a
multi-leg position counts, and **shares do not count**. Opening orders reserve
the contracts they could add, including armed conditional orders; closing capacity
is shared once across working orders on the same side of a contract. A promised
close cannot free capacity for a new opening. Atomic reducing orders are allowed,
including above a lowered limit; existing positions are kept after a step-down.
Single orders, combos, OTO children when submitted, and playbooks use the same
acceptance and fill checks. Excess increases are refused with `SCALING_LIMIT`,
`actual` contracts including reservations, `limit` and aggregate scope. A working
order that fails its recheck is cancelled with that same code. Previews and what-if
report it too. There is **no grace period**: excess orders are refused up front.
Other account checks still apply. Both contract caps use the same held-plus-opening
count. The fixed `max_contracts_held` cap still reserves the opening part of a
reversal or roll, even when F39 permits its atomic reduction; the tighter
applicable cap supplies the refusal reason.

`evaluation.scaling` in the account view is null without the rule, otherwise
`{limit, held, profit, next}`. `profit` is the current closed balance less start;
`next` is the first step with a higher contract limit, or null at the highest limit.
It remains visible when reached during the day, since that higher limit applies
only after a day closes at or above its threshold. Equal-limit steps are skipped.
The Dashboard shows this progress; plan facts and Rules explain it, and the custom
plan editor adds, edits or removes steps.

**Account size scaling (F63).** Optional `AccountRules::size_scaling` enables
periodic capital growth for long-horizon funded simulation. Absent or null disables
it. A non-null rule in the evaluation phase is `INVALID_RULES`. All five fields
are required: `profit_percent` (integer 1–100), `payouts` (integer 0–100), `days`
(integer 1–366 finished plan trading days), `increase_percent` (integer 1–100),
and `max_balance` (Money, at least the attempt's positive starting balance).
Wrong JSON types or unknown fields are `INVALID_REQUEST`; out-of-range values
are `INVALID_RULES`. Custom reset rules and custom funded backtests accept it.

```json
{"size_scaling":{"profit_percent":10,"payouts":2,"days":80,
                 "increase_percent":25,"max_balance":"200000.00"}}
```

At `roll_day`, after recording the finished day, a review occurs when the period
has lasted `days` finished days. It requires net realised profit of at least
`profit_percent` of the account size at period start, and at least `payouts`
completed payouts during the period. Profit is closed balance now minus closed
balance at period start plus withdrawals made during the period. An active
account below `max_balance` that meets both requirements grows. Every completed
review starts a new period, including a review that fails a requirement; profit,
payouts and days do not carry into the next period. Calendar gaps count as one
finished plan day, and a failed account never scales.

The increase is the lesser of `increase_percent` of the **original** starting
balance and `max_balance - current size`: growth is linear, not compounding.
Integer micro-dollar arithmetic rounds the percentage credit down to a micro-dollar
and the required profit up to a micro-dollar. Cash and closed balance receive the
credit as capital, never realised P&L or a fill. `Evaluation::starting_balance`
rises by the credit, preserving profit, payout eligibility and withdrawable profit,
the F38 buffer room and F39 closed-balance profit. The drawdown floor and peak
rise by the credit; an explicit floor lock balance moves with them. Original
`max_drawdown` and `daily_loss_limit` scale by new size / original size, rounded
down to cents directly from their original amounts on every step. The capital
lift is retained: a later ratchet never lowers the raised floor just because the
new drawdown distance is wider. New-day equity baselines include the credit.
The rules read-back and attempt rules snapshot retain the effective loss limits.

Each scale-up emits one `account_scaled` event (`old`, `size`, `increase`,
`period_start`, `period_end`, `day`), with journal money in micro-dollars. Enabled
review state, original loss amounts and size history are recorded and recovered.
Disabled rules and state are omitted from journal/rules serialization, preserving
older journal bytes. The API rules object returns `size_scaling: null` when off.

`evaluation.size_scaling` is null when disabled, otherwise
`{size, original, max_balance, period_started, period_days, days_required,
period_profit, profit_required, period_payouts, payouts_required, next_size,
history: [{day, old, size}]}`. Money uses decimal strings; dates use plan trading
days. At the cap, `next_size` equals `size`. The funded PlanEditor configures the
rule, plan facts and Rules explain it, and Payouts shows review progress and size
history. The existing rule notice mechanism reports scale-ups on their trading day.

Qualifying days need not be consecutive. Each finished day counts once, toward the
cycle in progress when it rolls over; a payout resets the count, so the trading day of
the request (`Payout::day`) and later days count toward the next payout.
`payout_quote(snapshot, rules)` reports the standing: the next payout
number, the qualifying days, profit, the withdrawable share (whole cents), the cap,
the maximum and the trader's share at the maximum, and `blocked`, the first unmet
requirement in this order: funded phase (`PAYOUT_UNAVAILABLE`), an active account
(`PAYOUT_UNAVAILABLE`), no positions or open orders including armed ones
(`PAYOUT_NOT_ELIGIBLE`), the qualifying days (`PAYOUT_NOT_ELIGIBLE` with actual and
limit), payout consistency (`PAYOUT_NOT_ELIGIBLE`, actual best-day share in percent,
null when cycle profit is nonpositive, and limit percent), and a maximum of at least the minimum (`PAYOUT_NOT_ELIGIBLE`). The maximum is
`min(withdrawable, cap)`; with a locked floor it also leaves equity at least a cent
above the floor. While the buffer applies, the maximum is also capped at
`equity - (starting_balance + buffer)`, rounded down to whole cents and clamped at
zero. Equality with that buffer balance is allowed; withdrawing anything requires
exceeding it. `buffer_balance` reports that balance, or null when disabled or past
the last buffered payout. When the buffer leaves no whole cent withdrawable, the
minimum check says "Equity must stay above the payout buffer of $X".

The quote also returns `consistency_percent` (null when disabled), `cycle_profit`,
`best_day` (null or `{day, profit}` for the largest positive day, earliest on ties),
and `consistency_needed` (additional profit rounded up to whole cents, zero when met
or disabled). Consistency uses every finished day's net realised profit, including
losses, from the cycle plus the day in progress. A payout excludes earlier finished
days; its own trading day remains in the new cycle in full, matching the qualifying
count's rollover boundary. Withdrawals do not reduce this realised-profit measure.
Cycle profit must be positive and `best_day * 100 <= percent * cycle_profit`, compared
in exact integers. Additional profit is `max(0, ceil((best * 100 / percent -
cycle_profit) / cent) * cent)`; with no positive best day, zero still needs positive
cycle profit. The amount assumes later profits do not create a larger best day.

For example, custom `payouts.consistency_percents: [40]` keeps the same limit each
cycle; `[20,25,30]` raises it for payouts two and three, then repeats 30%. Existing
funded presets are unchanged. A custom payout buffer of `"2100.00"` on a $50,000
starting balance retains $52,100.00; `buffer_payouts: 3` applies it to payouts 1–3,
and `buffer_payouts: 0` applies it to every payout. Default optional payout fields are omitted from the
journal, so existing funded journals retain their bytes and re-verify.

`request_payout(amount, time)` takes a positive whole-cent amount (otherwise it
throws `INVALID_PAYOUT`), returns `blocked` when set, and rejects amounts outside
`[minimum, maximum]` with `INVALID_PAYOUT`. It then withdraws the cash (realised P&L is
unchanged) and records a `Payout` (number, time, trading day, amount, trader share,
equity before the withdrawal). A withdrawal is not a loss: the daily-loss baseline, the
open and close equity of the trading day in progress (even when requested after its
date ends and before rollover) and an unlocked peak and floor all move down by the
amount, so the day's P&L and the drawdown room are unchanged and an end-of-day ratchet
compares closes net of it; a locked floor stays where it is.

**Payout reviews** (`payouts.review: {min_hold_seconds, max_trade_percent, open_loss,
ladder, cap}`, funded only, null when off). At each payout request the account reviews the
cycle's positive closed whole trades (closed since the start or strictly after the last
payout, so one closed at a payout's own timestamp is reviewed by that payout only, net of
fees; share round trips, from exercise, assignment or share trades, are trades of their
own, net of dividends, with IDs `s` and their opening stock fill): one held under `min_hold_seconds` from its first opening fill to its last close
offends as `short_hold`, one whose profit exceeds `max_trade_percent` of the cycle's net
realised profit (exactly, `pnl × 100 > percent × cycle profit`) as `oversized`, and, with
`open_loss` (which needs an open-loss cap), one whose positions breached it as
`open_loss`. The nth review that finds any offending trade takes the nth `ladder` step,
the last repeating (default `warning, forfeit, cap`): `warning` records the trades and
pays as usual; `forfeit` voids their profit, which leaves the account with the payout and
is not paid; `cap` forfeits and also caps the payout at `cap` (required exactly when a step
caps). Forfeited profit comes off the profit the withdrawal share is taken from, and off
the equity every floor and buffer check reads. The payout preview (`payout.review: {step,
trades: [{id, pnl, held_seconds, closed, offences}], forfeited}` and `payout.forfeit`)
shows what a request now would do; the journaled `payout` event and the account's
`payouts[].review` record it, only when a review found offending trades. Reviews add
alongside the other plan outcomes (a failure, a day lock, a pass held back): they act only
at payouts.

**The floor after a payout** (`payouts.after_payout`, needs a drawdown floor). `keep`
(default) leaves the floor as above. `reset` restarts the account at `reset_percent`
(1–100, default 100) of the starting balance: the payout's maximum leaves at least that
balance, the rest of the equity above it is removed with the payout, unpaid
(`payouts[].removed`), and the peak and floor restart from that balance, unlocked, as at
the attempt's start. `starting_balance` raises the floor to the starting balance after the
payout and locks it there; the maximum leaves equity at least a cent above it. The preview
reports `after_payout` and, for `reset`, `reset_balance`.

The web terminal always offers verification with its prerequisite lock, but hides
funded plans and the Payouts page (this simulator funds no one) unless the account is
already funded; `showFundedAccounts` in `web/src/lib/features.ts` offers them again.
The custom plan editor can explicitly select the funded phase
and edit qualifying days, shares, caps, consistency percentages and buffer rules.
The Payouts page shows cycle profit, best day/date/share, the current limit, remaining
profit and active buffer balance; plan facts and Rules also state these settings.
The server offers a funded preset for each intraday and end-of-day evaluation preset
(`funded-intraday-25k` and so on). Appended after the existing presets,
`funded-scaling-50k` uses Funded Intraday 50K's rules with the 2/3/5 option-contract
steps above and is unlocked only by passing `intraday-50k`. These count option
contracts, not futures lots. A reset into one requires that the current attempt passed the evaluation
it names, otherwise `PLAN_LOCKED`: that preset's starting balance and every one of its
rules, with execution, fee schedule, broker margin and program costs free to differ.
Once entered, the same preset may restart under its reset terms without another prerequisite pass. Custom rules may set `phase`
freely, but may not take a preset's name unless they are that preset (`INVALID_RULES`),
so an attempt recorded under a preset's name was that preset; `--plan` can start a new
journal on a funded preset directly. Preset parameters are this project's own,
modelled on common prop-firm terms: the evaluation's drawdown and strategy rules, the
floor locking at the starting balance, 8 qualifying days of $100, $150 or $200 (25K,
50K, 100K), up to 50% of profit per payout, 80% to the trader, a minimum of 1% of the
balance and caps of 2%, 3%, 4% and then 6% of the balance for payouts 1, 2, 3 and 4+.

`history.hpp`'s `lifecycles(fills, closures, contracts)` rebuilds round trips from flat
to flat. Each replays its own fills through a fresh `Ledger`, so realised P&L uses the
account's basis allocation and rounding exactly; a reversing fill closes one lifecycle
and opens the next at the same price with its fee split pro rata.
`share_lifecycles(stock_fills)` does the same for shares, per underlying: an
assignment that takes more shares than are held closes the round trip and opens the
opposite one at the same price.

## Trade review

Every option fill carries `context`, captured before it changes the account. It
contains `spot`, `spot_source` (`quote`, `parity`, or null), `iv`, `delta`, `years`
to expiry, `equity`, `floor_room` and available `buying_power`. Buying power is the
account's available buying power just before the fill, as the account showed it: the
filling order is already working then, so its own reservation is deducted (for a market
order accepted and filled at once, that is about the value after the fill). Equity and
floor room are the marked account before the order. The reducer copies
its current valuation; it does not calculate Greeks. Missing or stale analytics
stay null. A fresh stock price can supply spot when analytics cannot. Expiry time
is known from the contract even without analytics. Equity uses the account's last
marks and is null if a holding has no mark. Room above the floor is null without a
drawdown rule. Atomic multi-leg fills share the same pre-execution account values.
Older fills have null context. A trade's `entry_context` is its first opening
fill's; `exit_context` is its last reducing fill's, including a partial close.
Settlement, exercise, assignment, abandonment and reset have no closing fill context.

The reducer journals a `TradeReview` per option round trip and per opening
multi-leg order. It samples total marked P&L: realised gross plus the remaining
position's marked P&L, less all fees. Scaling and partial closes keep the same
round trip until flat. Strategy samples combine the legs at the same instant;
their extrema are not the sum of separate leg extrema. Ties keep the first time.
Samples use market time, at quote batches and executions, and finish on closure.
They resolve only to the marking cadence, about 15 seconds on Cboe's delayed feed;
they cannot recover highs or lows between observations. Missing marks are not zero.

The JSON `review` contains positive dollar `mae` and `mfe`, bounded below by zero.
`worst` and `best` retain the actual signed marked `pnl`, UTC `time` and underlying
`spot` (null when unavailable). A settlement sample's spot is the settlement reference,
not the expired contract's last valuation. Closed trades report `give_back = max(0, mfe - net)`
and `r_multiple = net / planned_risk`; `heat = mae / planned_risk`. Planned risk
excludes fees. A single-contract round trip uses its entry bracket's option-price
stop distance from the entry order's average fill price, times every contract
opened and the multiplier: an add keeps the entry's risk per contract wherever it
fills, even past the stop. Later changes to the exit do not rewrite that plan. A
strategy with an entry bracket stop on its combo price plans the same way: the entry
order's net per unit to the stop level (both signed as order prices, so a unit closed
at the stop loses their sum), for every unit that order opened, times the multiplier.
Without one, a strategy uses its opening debit or credit and the minimum payoff at
all strikes and zero, if every leg settles at the same instant and the call payoff is
bounded below. A calendar or diagonal whose every short is covered contract for
contract by longs of its type that expire with it or later, at a strike at least as
good (a call long at or below its short's strike, a put long at or above), plans its
debit: the longs are worth at least what the shorts owe when they expire.
Risk grows with opening quantities, and is not reduced by partial closes. Risk, heat
and R are null without a positive, measurable planned risk, including underlying-price
stops, unbounded structures, and calendars or diagonals with an uncovered short or a
credit. The option legs of a strategy
carry its `strategy_id` and combined `strategy_review` separately from their own reviews.

Strategy review follows the Journal's grouping by the order that opened each
contract round trip. It covers the legs that order opened as round trips of their
own, at least two of them: a roll's new legs keep a strategy review while the legs it
closes keep theirs. An order that adds to another order's round trip shares those
contracts, so it has no strategy review of its own.
Older round trips without entry context keep null review fields: historical marks
are not reconstructed. Share trades retain their existing P&L and notes. Each round
trip's P&L by Greek is its `attribution` (see [P&L by Greek](#pl-by-greek)).

### Edge reports, mistake picks and coaching

Journal reports also group closed trades by entry time in New York (before 09:30,
09:30–10:30, 10:30–12:00, 12:00–14:00, 14:00–16:00, after 16:00), entry DTE
(`<1`, `1–7`, `8–30`, `31+`), absolute entry delta (0.20-wide buckets), IV
(`<15%`, `15–25%`, `25–40%`, `40%+`), underlying, strategy template tag and
playbook ID across versions. Numerical upper bounds are exclusive. DTE is
`entry_context.years * 365`, not a rounded calendar-day count. Missing context or
unlabelled templates/playbooks appear as **Unknown**; no Greeks are reconstructed.
A template group uses the saved template parameter tag. Multiple labels count in
each matching group. Whole-trade mode uses its first entry context and combined
review; otherwise each closed round trip counts once.

Each group displays trades, win rate (wins divided by nonzero outcomes),
expectancy (net P&L divided by all closed trades), average recorded R, profit
factor and average recorded give-back. Missing review values are excluded from
those averages. Profit factor is infinite with wins and no losses, and undefined
when there are no decided trades. These are browser display summaries; accounting
remains exact on the server.

Notes now have a separate `mistakes` array with fixed picks: `chased`, `oversized`,
`moved stop`, `revenge`, `broke plan`, `early exit`, and the positive `A+ setup`.
They never become free-text `tags`. `PUT /api/trades/{id}/note` replaces note,
tags and mistakes; omitted fields clear them. Picks persist in the journal and
CSV export for options and shares. Journal compares observed net P&L and counts
with and without each pick. It is an association, not a counterfactual cost or
proof of causation. Trade-level edits on an archived replay remain read-only.

Closed option review flags give-back above 50% of a positive MFE, heat above 1R,
a held interval overlapping an authored scenario event, any explicitly false
playbook adherence rule, and peak size above twice the median closed-trade peak
size for the same underlying (at least three observations). Share cohorts use
peak shares separately from option contracts. The median includes the reviewed
trade. Flags use available evidence; absent evidence is not a passed rule.
The same flags can group Journal reports. A trade's `entry_order` joins its
playbook report even when it is a single leg. These are transparent rules, not
predictions or personalised trading advice.

The underlying trade chart and intraday equity curve show scenario events at
resolved UTC times, with New York labels. Event durations also mark their ends.
Markers describe authored simulated events; they do not create missing candles,
equity marks or market timestamps. Recorded days have no authored scenario events.

### Replay from a trade

**Replay from here** opens a new isolated, paused paper account five minutes before
a trade's entry, clamped to the available source/session open. It uses the
source scenario or recording, seed, date, generator revision and initial plan and
settings. It copies no prior trade commands. The source journal is preserved;
the new replay replaces whichever replay is active, while live accounts keep
running. Source identity is checked against the journal: missing/changed recordings,
missing/changed scenario files, unsupported drivers or changed calendars are
refused with a clear error. Replays without durable source history cannot use it.
This is distinct from **Restart from…**, which re-executes the earlier commands.

### Optional AI review

AI review is off by default. In a trade review, expand **Optional AI review**, check
**Enable for this review**, enter your own endpoint and bearer key, and press
**Send AI review**. No request is made on load, while typing, or without all three
settings. Settings/key stay in component memory and clear when it unmounts; the
key is not stored on the server or in browser storage. No built-in provider or key
is supplied. The browser sends the selected trade JSON (including notes/tags),
coach flags, `paper_trade: true`, and an instruction to `POST` the configured URL.
The endpoint must return `{"review":"text"}` and allow the terminal's origin with
CORS. This small adapter protocol is not an assumed vendor API format.

Use HTTPS, or HTTP on localhost. The request includes `Authorization: Bearer KEY`,
omits cookies, refuses redirects, and times out after 30 seconds. Returned text is
rendered as text. Configure only an endpoint you intend to receive the review.
Local tests use a mock HTTP server; no paid endpoint or external model validation
is claimed. Rule-based flags remain available with AI review disabled.


### Return on buying power

A premium-based return misleads for credit spreads: a $1.00 credit on a $5-wide
spread risks $400, not $100. Each trade also gives `buying_power`, what its entry
needed on its own under strategy margin, and `return_on_buying_power`, its net over
that once closed (null while open or when the need is zero). The need is the
premium its opening fills paid less what they received, plus the requirement of its
shorts at their opening prices, with the entry's underlying price for the naked rule
(the strike without one), every contract it opened counted; fees are left out. A
strategy's legs carry the same for the strategy together, `strategy_buying_power` and
`strategy_return_on_buying_power` (once every leg has closed), and a whole trade its
peak: after each opening execution (one entry order's fills at one instant), what
its round trips still open in fill order needed, the most of those. A roll's closing
legs are already closed then, even when it opens and closes at one market time. The
playbook report and backtest summaries give `average_return_on_buying_power` over
their closed trades, and the Journal, the Playbooks page and backtests show it. It is
the account's strategy margin at entry; portfolio margin or later price changes do not
rewrite it.

### Whole trades

A trade managed over time is one trade, not a string of round trips. Each round trip
is in a whole trade, named by a round trip's ID and carried as its `group`: its own,
or for a strategy's legs the first leg's. A multi-leg order that closes or adds to
another order's round trip at the instant it opens new ones, a roll or an
adjustment, joins the round trips it opens to that trade; so does any order that
names a trade in `group`, by its ID or one of its open round trips' (an order naming
no trade holding an open round trip on its underlying, or attaching held exits, is
refused with `INVALID_GROUP`). `POST /api/trades/group` joins the trades of two or
more open round trips into one, named by the oldest, so legs entered one by one
become one trade. A closed round trip can name its whole trade for grouping while
that trade still holds an open round trip. `POST /api/trades/ungroup` takes each
listed open round trip out of its trade into one of its own; it still refuses
closed round trips. Both refuse fully closed trades and mixed underlyings
(`INVALID_GROUP`) and unknown round trips (`UNKNOWN_TRADE`), are journaled, and
change nothing else.

A trade with more than one entry has a whole-trade review. A roll's starts as the
trade's review was before it (its first strategy's, or its first round trip's), so
its worst and best include the moves before the roll, then samples the P&L of every
round trip in the trade together, closed ones at their final P&L. A trade the account
grouped or ungrouped starts its review again then (`review_since`). Its planned risk
comes from what its open round trips can lose as a same-settlement structure, or
their debit when their later-expiring longs cover every short, as for strategy
reviews. When neither gives a positive risk, it keeps the planned risk of the review
it replaces: the target trade's earlier whole-trade review, or its first strategy's
or round trip's review. Its worst and best still start at the grouping. The trade
left after an ungroup plans its risk the same way. `GET /api/trades`
lists such trades under `groups`, newest first and filtered as the trades are: `id`,
`attempt`, `underlying`, `status`, `opened`, `closed`, `trading_day`, `round_trips`,
`entries` (the orders that opened them), `gross`, `fees`, `net`, `unrealised` while
open, `buying_power` and `return_on_buying_power` on its peak, and `review` with its
R-multiple on the trade's net. `GET /api/portfolio` lists
the positions held by whole trade under `strategies`: each trade's open `legs`
(`symbol`, `quantity` and the leg's round trip, `trade`), its `round_trips` and
`entries`, and its `realised`, `fees`, `unrealised` and `net` over every round trip of
it. The Journal shows whole trades as one row each beside strategies, with a leg
taken out in a click, and can count its headline numbers, calendar and reports by
whole trade instead of by round trip; on the Positions page, picked positions group
into one trade.

Each closed option trade says what closed it, `closed_by`: the closure that ended it
(`settlement`, `exercise`, `assignment`, `abandon`, `reset`), or the order of its last reducing
fill: the trader's own `order`, a bracket exit (`stop_loss`, `take_profit`), a
`flatten`, a playbook's close (`playbook`), or a reducer liquidation (`system`), with
`system_reason` saying why: `target` and `drawdown` when a decided attempt liquidates,
`soft_floor` for the personal guardrail and `expiry` for the auto-close before expiry.
The Journal marks such trades, so after a pass or fail the forced exits stand apart.

The Journal shows context side by side, excursions and risk multiples, notes and
tags, and stored one-minute underlying candles with entry, exit, MAE and MFE
markers. Missing candle history is shown as unavailable. The candles route returns
the latest 5,000 bars, so older trades may have partial or no coverage. Strategy
closing prices are net premiums per unit, signed as order prices are (a debit
positive, a credit negative) and shown as the opening price is ("$0.85 db");
return divides leg net P&L by the absolute net entry premium, or is null at zero
premium. Win rate everywhere, the Journal's tile and reports, the playbook report and
backtests alike, is wins over decided trades: a breakeven round trip is neither a win
nor a loss. List filters select
known or unknown planned risk and positive give-back. The weekday and month reports
group closed trades by the session they closed in, each trade's `trading_day`: the
engine's trading date of its close, a business day's New York date until 17:00, and
after that, over a weekend or on a market holiday the next business day, so a close in
Sunday evening's overnight session counts toward Monday and one on the evening before
Thanksgiving toward the Friday. (Trades from older servers have no `trading_day`; for
them the terminal counts weekdays without holidays.) The calendar keeps the New York
date.

### Fill quality

`GET /api/fills/quality` (and the Journal's **Fill quality** panel) places each of the
account's fills on the book it traded against (`Fill.quote`), measured from the mid
toward the natural (the far side): **mid or better**, within **one tick past the
mid**, further **inside**, at the **natural**, or **beyond** it (slippage, impact,
stop stress). Ticks use the natural price's tick tier and are positive when worse
for the side. Each group reports fills, contracts by place, contract-weighted mean
ticks from the mid and from the natural, and the dollars paid beyond the mid
(multiplier applied). Groups: overall, New York **time of day** (before 09:30,
09:30–10:00, 10:00–12:00, 12:00–14:00, 14:00–15:30, 15:30–16:15, after 16:15),
**spread width** in ticks (1, 2, 3–4, 5–9, 10+) and calendar **days to expiry**
(0, 1, 2–7, 8–30, 31+). `attempt=current` (the default) or `all`. Fills recorded by
older builds without their book, on a book without both sides, or given away for
nothing are counted as `unmeasured`.

The report suggests a **Conservative calibration** as a labelled rule of thumb
(`basis: "heuristic"`): Conservative's 1,000 ms, 1 slippage tick and 1 impact tick,
with 2 slippage ticks when at least half the contracts traded on spreads of five
ticks or more, in the first or last half hour or at 0 DTE, and 2 impact ticks when at
least a quarter used up the displayed size left on their quote. It describes paper
fills only; real fills calibrate from data below.

### Real-fill calibration

`POST /api/fills/calibration` (the Fill quality panel's **Calibrate from real fills**)
reads a CSV of the trader's real broker fills into a separate analysis and returns
it; **nothing is stored, journaled or traded**, and no account changes. It needs a
token with read scope (or admin), even on a server whose GET routes are public:
anonymous callers are refused unless the server is an open loopback one, where they
may already trade. Sandbox tokens are refused. The request is JSON, `{"csv": "...", "format": "auto",
"max_quote_age_seconds": 60}`, within the 64 KiB request limit (split longer
exports); at most 5,000 fills are read. Formats:

| `format` | Reads |
| --- | --- |
| `generic` | Header columns `time` (ISO 8601; New York unless it names an offset), `symbol` (OSI, padded or compact), `side` (buy/sell, or BTO/STC…), `quantity` (its sign stands in for a missing side) and `price`, with optional `bid` and `ask` at the fill |
| `tastytrade` | The transaction history export: `Type` Trade rows of option instruments; time from `Date`, side from `Action`, price from `Value / (Quantity × Multiplier)`, which excludes commissions and fees |
| `schwab` | A thinkorswim account statement's Account Trade History section (`Exec Time`, `Side`, `Qty`, `Symbol`, `Exp`, `Strike`, `Type`, `Price`; a spread's later legs share the first leg's time), or a Schwab transaction export (`Date`, `Action`, `Symbol` like `SPXW 10/22/2026 5000.00 C`, `Quantity`, `Price`), which gives dates only |
| `ibkr` | A Flex Query trades file (`Symbol`, `DateTime` such as `20261021;101532` or `TradeDate`/`TradeTime`, `Buy/Sell`, `Quantity`, `TradePrice`, options only when `AssetClass` is given), or an activity statement's `Trades` section (`Date/Time`, `Quantity`, `T. Price`, symbols like `SPXW 22OCT26 5000 C`) |
| `auto` | Detects the format from the header |

Times without an offset are read as New York time, as these exports usually show
for US accounts; check the export's time zone setting. The layouts follow the
brokers' documented and commonly exported columns; they were built and tested from
sample files, not live exports from each broker.

Each timed fill is compared with the book at the same time: the file's own bid and
ask when it has them, otherwise the **last recorded quote** at or before the fill and
no older than `max_quote_age_seconds`, from the complete recordings in the directory
the replay picker lists (`--record-dir`, else `~/.openport/recordings`) that start
on the fill's trading date. A fill with no time, or no quote, is listed with a note
and left out of the comparison.

The search runs on a worker, one calibration at a time (another meanwhile is 503
`CALIBRATION_BUSY` with `Retry-After`), and is bounded: timed fills may span at most
31 days (400 otherwise); recordings are read **newest first** and the search stops
once every timed fill has a quote, or at 32 recordings, 4 GiB, 20 seconds or 10,000
recordings in the directory. A fill keeps the latest quote any recording read gave
it, so a fill covered by two overlapping recordings uses the newer one's. The
response's `search` gives `files`, `bytes` and why it `stopped` (`complete`,
`all_matched`, `file_limit`, `byte_limit` or `time_limit`); fills a stopped search
did not reach are unmatched. Imported quantities above 1,000,000 contracts are
skipped as misread. For each matched fill the response gives its place
on the book and what each paper model would have done at once with a **limit at the
real price** on that book: **as displayed** fills at the natural only when it is
marketable and within the displayed size; **Conservative** adds a slippage tick
and an impact tick per displayed-size block and fills only if the whole price fits;
**inside at midpoint** also fills a limit at least halfway across the spread at its
limit. Latency is not simulated here: broker exports carry no order-entry time.

The suggestion (`basis: "real_fills"`) brings paper fills closest to the real ones:
slippage ticks equal to the contract-weighted median ticks beyond the natural when
that is at least half a tick; otherwise, when most real contracts filled inside the
spread, inside fills at the median fraction of the way from the own side to the
natural; otherwise as displayed. One impact tick is suggested when at least a
quarter of the matched contracts were in fills larger than the displayed size.
`fill_latency_ms` is null. The suggestion is advice to apply when starting a new
attempt; it never changes an account.

## Trade notes and tags

Order requests accept optional `tags` and `note` with the same validation below.
When a fill opens a round trip, these annotate its opening fill ID in the same
transaction. Each newly opened combo leg receives them, so its Journal strategy
and tag reports use them too. Adds to an existing round trip keep its annotation;
closing fills add nothing. Orders preserve the submitted metadata for inspection.
Tickets accept optional tags and notes from their caller; this tree has no strategy
template catalog yet. Older journal orders default to empty tags and notes.

`annotate(trade, note, tags, time)` records the trader's note and tags on a trade,
named by the fill that opened it (the trades view's `id`); `annotate_shares` does the
same for a share round trip, named by its opening stock fill and kept under `s` and
that ID, so the two never collide. The note is trimmed and at
most 2,000 bytes of UTF-8 text, keeping newlines and tabs (a CRLF or lone CR line break
is kept as a newline, and counts as one byte); up to eight tags of 1 to 32
bytes without commas or control characters are trimmed, lowercased and kept once
each. Text past these limits throws `INVALID_NOTE`, whose message says whether the
length or a control character is at fault; a fill that opens no trade returns
`UNKNOWN_TRADE`. An empty note without tags clears them. Notes are journaled
(`trade_annotated`) like any command, so they recover with the account and stay with
its history across attempts, and they are allowed whatever the account's state or
session. The snapshot's `annotations` maps each trade's ID to its note, tags and the
time it last changed.

### Account alerts

An account keeps up to 100 alerts. Each watches one value, fires when it reaches an
inclusive level, and is kept in the journal with the account, so it fires whether or not
the terminal is open, survives a restart and fires at the same steps when a replay is
[verified on the same build and platform](runtime.md#verifying-a-run). A one-shot alert fires once; with `repeat` it fires again each time its
condition returns after a fresh value showed it lapsed.

| Scope | `symbol` / `legs` | Metrics |
| --- | --- | --- |
| `contract` | A registered padded OSI | `bid`, `ask`, `mark`, `iv` (smile IV in vol points), `delta`, `gamma`, `theta`, `vega` (per unit, from the contract's valuation) |
| `spread` | Two to four `legs` (`symbol`, `side`, `ratio`) | `mark`: the net mark per unit, buys adding and sells subtracting |
| `underlying` | An underlying, such as `SPX` or `VIX` | `price`, `iv30`, `iv7`, `term_ratio`: the values [conditional orders](#conditional-and-bracket-orders) watch |
| `account` | None | `equity`, `day_pnl` (equity less the day's opening equity), `unrealised`, `floor_room` (to the plan floor or the soft floor, whichever is nearer), `buying_power`, `dollar_delta`, `vega`, `theta` |

Alerts are checked after each market batch and when created, so one whose condition
already holds fires at once. Quotes and marks count while they are within the quote age,
valuations and underlying values within the valuation age; account measures need complete
marks (exposures complete valuations; buying power also fresh working-order quotes).
A missing or stale value neither fires an alert nor rearms it. The server supplies the quotes and valuations of contracts that alerts watch,
and the values of underlyings, like those of the account's own orders. Levels may be
negative, as for a day P&L or a put's delta. Each firing is recorded as an `alert_fired`
event and in the alert's `fired`, `fired_at` and `value`; the terminal announces it, and
[external notifications](runtime.md#external-notifications) forward it as the `alert`
event from live accounts. Demo and replay accounts also forward when the operator
sets `include_simulated: true` in the server notification config; otherwise their
alerts fire in the terminal only.
Alerts change no orders or positions: they are allowed whatever the account's state, and
a reset keeps them. In **Alerts → Account alerts**, choose the scope and measure,
enter the inclusive level and optionally enable repeat. A spread takes two to four
explicit legs with ratios from 1 to 10, or can start from held legs; the saved legs
stay fixed when positions change. Firings show their time and value in the list.
Alerts keep working only while the server and its feed are running. A restart
restores their state but cannot evaluate market moves during the downtime.

### Day notes

`annotate_day(day, plan, review, time)` replaces the account's note for a New York
calendar date. Both fields use trade-note validation: trimmed UTF-8 text, each at
most 2,000 bytes, with newlines and tabs allowed. Empty fields clear the note. A
valid date is required; no trade is required on that day. Notes are reducer commands,
journaled with market time and retained across account resets. The Journal calendar
marks days with notes and opens the day's plan and review when selected.
`GET /api/trades` includes `day_notes`, an object keyed by `YYYY-MM-DD`, with
`plan`, `review` and the last edit's UTC `time`, independently of attempt filters.

`GET /api/orders/{id}?account=ID` reads one order with the same fields as the
orders list, including reason, note and changes; replay and archived-run mirrors
also support it. The response is `{account_version, order}`. Unknown ids return
404 `UNKNOWN_ORDER`; malformed ids return 400 `INVALID_REQUEST`.

### CSV downloads

Trade CSV appends `time_stop_orders`: a JSON array of objects containing
`order_id`, `reason` (code/message/evidence, or null for the closing order) and
`note` for related automatic playbook cancellations and closes. The same exit event
can appear on each contract round trip of a spread. Share rows leave it empty.

`GET /api/trades.csv` exports option and share round trips; `GET /api/fills.csv`
exports option fills. Both accept `account`, `from` and `to`; dates are inclusive
New York calendar dates. Fills filter by execution date; closed trades by closing
date and open trades by entry date. Invalid dates or an inverted range return 400.
Trades also accept the JSON route's `status` and `attempt`, with CSV defaulting to
all attempts. The Journal's trade download uses the selected attempt; fills always
include all attempts. Export dates do not filter the page's other panels.

Downloads use `text/csv; charset=utf-8`, attachment filenames, a fixed header even
with no rows, CRLF row endings and RFC 4180 quoting. Commas, quotes and newlines in
notes are preserved. Money retains micro-dollar precision, with two to six decimal places. Every JSON
row field is included, with nested context, review and attribution fields in dotted columns;
arrays, including tags and fill IDs, join with `;`. Missing values are empty cells.
Times remain ISO UTC and each row adds `new_york_date`, account, account version,
current provider and a price-source label. For `/api/replay/trades.csv`,
`/api/replay/fills.csv` and their `/api/replay/history/ID` equivalents, `account`
is the replay run ID, matching the JSON trades view's `run.id` and the CSV `run_id`,
even when `account=main` selects the internal replay account. Ordinary account
exports retain the selected account ID (default `main`). Exports from a replay, running or saved, also
name the run on every row: `run_id`, and `scenario` and `seed` or the `recording` it
replayed (empty for a live account), so rows from several practice runs stay apart once
merged; `GET /api/trades` gives the same as `run`. Fill rows carry their `attempt`, as
trade rows do. Share trades list their stock fills with an `s` prefix (`s3`), as their
own IDs have, so they never read as option fill IDs. Paper P&L is simulated; demo prices are
labelled simulated too. Other exports leave price provenance unrecorded because
the current provider cannot establish the source of historical fills. The separate stock-fill, dividend and day-note collections are not
trade rows in these exports.

## Scenarios

Default spot shocks: -10, -5, -2, -1, 0, +1, +2, +5, +10 percent.
Default volatility shocks: -5, 0, +5, +10 **vol points**. Rows are spot-major.

```text
S' = S * (1 + spot_percent / 100)
F' = F * (1 + spot_percent / 100)
sigma' = max(vol_floor, smile_IV + vol_points / 100)
D' = D; T' = T
P&L = sum(q * M * (Black76(F', K, T, sigma', D) - Black76(F, K, T, IV, D)))
```

Black-76 consumes the scaled forward; scaling spot together expresses the sticky
strike carry assumption ([other smile dynamics](#smile-dynamics) are an analysis read). Default floor is 0.0001. Each cell flags any floor clamp.
Zero spot/vol shock is **exactly zero**, bypassing subtraction and floor artifacts.
P&L is analytical double dollars, not booked cash. Invalid grids throw; axes are
bounded to 101 values each, spot > -100% and <= +1000%, absolute vol shock <= 1000
points. Missing/expired/nonfinite pricing inputs flag the whole grid incomplete;
its numeric placeholders are zero and must not be displayed as measured P&L.

## Scenario explorer

`GET /api/risk/scenarios` returns day-major `slices` of the held book's derived
P&L. Default `mode=parallel`, `days=0` and `vols=-5,0,5,10` use the configured
spot axis. `mode=beta` reuses the profile's beta selection (benchmark SPY or SPX,
optional `betas=SYMBOL:BETA`). Missing betas make the entire grid unavailable.
A beta-weighted spot decline is capped at 99.99% to keep prices positive.
`days=0,1,7` adds up to eight calendar-day slices, including intrinsic value
past expiry. `term_scaled=true` multiplies each volatility shock by
`sqrt(30 / max(1, remaining_days))`, relative to 30 days, after time advances.
Tenors below one day use a one-day cap; this is a model assumption, not observed
volatility. `range` and `steps` select the spot axis as in the profile.
`dynamics=sticky_moneyness` or `sticky_delta` moves each smile along its
underlying's SSVI surface in every mode, exactly as the profile does ([smile
dynamics](#smile-dynamics)); a term-scaled shock is applied on top of the moved
smile. The response names `dynamics` and lists `surface_fallback` underlyings
priced sticky strike for want of a surface.

The terminal's Scenario explorer offers mode, time, term scaling and smile controls.
These are read-only analyses: order previews, What-if max loss, grid-loss rules
and portfolio margin retain their existing configured parallel, zero-day grid.

## Historical stress and simulation risk

`GET /api/risk/history` (also under `/api/replay` and saved replay histories)
returns `stress`, `sticky_strike` and `atm_vol_change`. The terminal shows these
in Risk; Python and MCP expose `risk_history`. All amounts are derived analytics,
not booked cash, forecasts, or a reconstruction of historical holdings.

The checked-in [stress definitions](../data/risk-stress.json) contain four
one-session shocks: February 5, 2018 Volmageddon; March 16, 2020 COVID;
August 5, 2024 yen unwind; and April 4, 2025 tariff shock. Each identifies its
close dates, public sources, per-underlying moves and 7/30/90-day volatility
changes, with the label **derived from public closes**. S&P spot declines are
rounded outward to a tenth of a percent. VIX close changes proxy 30-day ATM
changes; other tenors use a stated square-root extrapolation. SPY/XSP follow
the S&P move. Nasdaq and Russell families use explicitly labelled 1.5-times
S&P/VIX proxies, not those indexes' observed returns. Other held underlyings
are reported in `missing_underlyings`, and make that entire stress unavailable.

Each stress reprices every held option at its sticky-strike smile with the
interpolated tenor shock (flat outside the tenor endpoints), and every share at
the shocked spot. It reports analytical dollar P&L, whether equity would reach
the nearer plan/personal floor, and the resulting held-book margin under the
account's strategy or portfolio model and house settings. No floor means a null
breach result. Margin excludes open-order reservations. The shock is instantaneous:
no floor ratchet, path, bid/ask widening, assignment or liquidation is simulated.
A missing, stale, invalid or unsettled valuation makes the whole result null.

Historical simulation uses at most 252 stored completed daily closes **per held
underlying**, without substituting betas or other indexes. It aligns both dates
of each return, rejects missing trading sessions, excludes today's partial bar
and future rows, and requires at least 20 aligned returns (at most 251). Each
joint spot move is applied to today's held book, using the same Black-76 repricer
with one calendar day of decay and shrinking carry. Sticky strike holds IV
constant. The ATM variant additionally requires observed 30-day ATM IV at both
endpoints for every factor, and applies each factor's change uniformly to its
contracts' smile IVs. It has its own observation count and date range; absent
IV history leaves it null. Stored sampled closes retain their limitations;
no additional prices or market timestamps are fabricated.

`var95` and `var99` are the empirical inverse-CDF loss quantiles. `es95` and
`es99` average the upper 5%/1% probability mass, weighting the boundary observation
fractionally. Values are nonnegative dollar losses. The 95% figures need at
least 20 observations and the 99% figures at least 100: below that the 1% tail
holds less than one observation, so both would only repeat the worst loss.
`var99` and `es99` are then null and `tail99_reason` says why (it is null when
they are reported). The UI always shows the count. These are historical
estimates, not guaranteed loss bounds. The model omits liquidity, intraday paths,
dividends paid during the horizon and physical settlement; beyond expiry it uses
intrinsic value at the shocked price.

## Portfolio risk history

`GET /api/account/equity` now includes optional nullable `risk` on each sample:
`dollar_delta`, `dollar_gamma_1pct`, `vega`, `theta`, `delta_utilisation` and
`vega_utilisation`, plus nullable gamma, theta, grid-loss and short-vega utilisation
when those caps are enabled. Greeks use the held book; utilisation includes pending-order
reservations and the limits in effect at that market-time mark. Only complete
contemporaneous valuations are stored. Older eight/nine-column equity CSV rows
and intermediate fill marks have no risk observations; they are never backfilled
from current Greeks. The extended CSV round-trips doubles at full precision.
The existing minute cadence and 90-day/100,000-row history retention apply.

Intraday equity offers a risk metric selector and an Across days view. Missing
values, missing minutes and attempt changes break the line. This history is a
record of paper-account analytics, not a historical market data feed.

## Additional exposure limits

`aggregate`, `per_underlying` and `underlying_overrides` accept four optional
nullable caps in addition to dollar delta and vega:

| Cap | Meaning |
| --- | --- |
| `dollar_gamma_1pct` | Worst reachable absolute dollar gamma for a 1% underlying move |
| `theta` | Worst reachable absolute dollar theta per calendar day |
| `worst_grid_loss` | Worst loss in the configured parallel, zero-day trading-rule grid, including independent pending-order reservations |
| `short_vega` | Gross reachable short vega per vol point, summed by contract; long wings in other contracts do not offset it |

Null or omitted disables a new cap; zero forbids any exposure on that measure.
Gamma/theta reservations sum each order's signed exposure, keeping atomic combo
legs together. At each grid cell, the book's loss plus each pending order's
positive loss reserves every independent fill subset. The greatest cell loss
is the cap's value. A pending buy to close a short lowers no worst reachable
reservation (it might never fill), but does not increase it and can still pass.
The gross short-vega bound similarly accounts for current signed vega in each
contract and the orders that could make it more negative. Scoped and underlying
caps complement the aggregate cap.

`tag_overrides` and `playbook_overrides` map scope names to exposure caps.
Tags match exactly (order tags are normalized to lowercase); playbook IDs match
`playbook:ID@vVERSION` across versions. Filled positions inherit their lifecycle's
order tags and current trade/group annotations. Delivered shares retain their
originating option's tags. A fungible position shared by several tags counts in
each scope, rather than inventing lot ownership. Retagging cannot erase entry
order tags. A tagged order joining an existing fungible position reserves the
whole position in its new scope before acceptance. The check values that scope
over the same positions with and without the order, so a tagged order that only
reduces an untagged book already over the scope's cap still passes, while one
that adds to it is refused. Untagged closing orders
still count against the held position's scopes. Manually traded untagged shares
are covered by the aggregate/underlying caps, and by a scope if their trade is
annotated with it.

The risk response publishes `scopes` keyed by `tag:NAME` or `playbook:ID`,
expanded reachable Greek ranges, gross short vega and nullable cap utilisation.
Grid exposure is calculated when its cap is enabled. Each grid is remembered
per thread under its exact inputs (positions, their usable valuations, share
prices and the grid configuration), so repeated order checks reuse it without
changing any value. The editor supports all
caps and scopes. Breach reasons are `GAMMA_LIMIT`, `THETA_LIMIT`,
`GRID_LOSS_LIMIT` and `SHORT_VEGA_LIMIT`. As with delta/vega, an already excessive
bucket permits an order that does not increase its worst reachable exposure.
Tightening applies immediately; evaluation-account loosening waits for rollover,
including removing a scope. Old journals omit new defaults and recover unchanged.

## Risk-metric alerts

Server account alerts additionally accept `dollar_gamma_1pct`,
`delta_utilisation`, `vega_utilisation`, `grid_loss` and
`floor_touch_probability`, alongside the existing dollar delta, vega, theta and
`floor_room`. All use inclusive `at_or_above`/`at_or_below` comparisons and the
existing one-shot/repeat lifecycle. Utilisation and odds are ratios: 0.8 means
80%. Grid loss is the held-book configured rule grid, excluding pending orders;
the limit's reachable grid loss separately includes them.

Floor-touch odds are the **maximum individual underlying/barrier** driftless
reflection probability to today's regular close, considering both plan and soft
floors. This is not a joint portfolio probability or the probability of either
barrier. It uses the same implied close variance as the breach panel, passed at
its observed market time. Missing or stale valuations, variance or a missing
floor leave this metric unavailable and do not fire an alert. Greek levels and
utilisation likewise require complete risk valuations. Unavailable data does
not rearm repeating alerts.

These are ordinary server alerts, persisted and checked with the browser closed.
Firings use the existing browser sound/notification watcher and configured
external notification channels; demo/replay forwarding still follows
`include_simulated`. No new delivery configuration is needed.

## Risk profile and probabilities

`GET /api/risk/profile` draws the held book's P&L across moves of a reference price
on several dates: where the book, and the room to the floor, will be later today,
tomorrow or at the first expiry. With `underlying=SYMBOL` it covers that underlying's
contracts and shares against its own moves. Without one it covers the whole book
against a benchmark, `benchmark=SPY` (the default) or `SPX`, each underlying moving
its beta times the benchmark's percent move. The same read is available for the
running replay at `/api/replay/risk/profile` and for saved replay runs at
`/api/replay/history/{run_id}/risk/profile`.

| Parameter | Meaning |
| --- | --- |
| `days` | Up to eight comma-separated curve dates, in calendar days after the account's market time (0 to 366, fractions allowed), or `expiry` for the first unexpired held expiry in scope; default `0,1` |
| `term_scaled` | Optional `true`: scale the IV offset by sqrt(30 / max(1, remaining calendar days)); default false |
| `iv` | Vol points added to every contract's smile IV, -100 to 100; default 0 |
| `dynamics` | `sticky_strike` (default), `sticky_moneyness` or `sticky_delta`: how IVs move with spot ([smile dynamics](#smile-dynamics)) |
| `range`, `steps` | The moves run evenly from -`range`% to +`range`% (at most 99) in an odd number of `steps` (3 to 201); default 10 and 41 |
| `betas` | `SYMBOL:BETA` overrides, comma-separated, each within 10 of zero |
| `legs` | Up to eight `OSI:QUANTITY` hypothetical legs (padded OSI, signed whole contracts from -10000 to 10000 with at most one sign, so `+-5` is refused), comma-separated; requires `underlying`. A leg larger than the held position flips it |
| `held` | `false` values the legs alone, without the held book or its floors; default `true` |

```text
x = the reference's move; each underlying moves beta * x (floored at -99.99%)
T' = T - days / 365 (0 once the contract has expired by then)
F' = (1 + beta * x) * (S + (F - S) * T' / T);  D' = D^(T' / T)
sigma' = max(vol_floor, smile_IV + iv / 100)
P&L = sum(q * M * (Black76(F', K, T', sigma', D') - Black76(F, K, T, IV, D)))
    + shares * price * beta * x
```

Today's curve with no IV change is the scenario grid's zero-volatility column, so
its levels are the breach estimate's. Later curves lose the time value the days
take, at the same smile (sticky strike); a contract expired by then is worth its
intrinsic value at the moved spot. P&L is analytical double dollars from the book's
model value now, not booked cash. Each curve lists the reference's levels where
equity reaches the plan floor (`down`, `up`; the soft floor's without a plan floor)
and the soft floor (`soft_down`, `soft_up`), found as breach levels are, with the
odds of touching each by the curve's horizon (today's close for today's curve,
the curve's date otherwise). Its `horizon` holds the reference's implied standard
deviation of log price to then and the one and two standard deviation cones. A held
contract without a fresh valuation, an expired one awaiting settlement or an
underlying without a beta leaves the profile incomplete: every `pnl` is null and no
level is given.

Betas come from the first source that has one: 1 for the benchmark itself, the
request's `betas`, 1 for the same index (SPX, SPXW, XSP and SPY; NDX, NDXP, XND and
QQQ; RUT, RUTW and IWM), the slope of the underlying's daily log returns on the
benchmark's over at most the last 252 completed closes (at least 20 paired), and
the same over 5-minute closes within each session (at least 30). Each `betas` row
gives its source, the returns it used, the correlation, and the underlying's dollar
delta with its beta-weighted dollar delta; `weighted_dollar_delta` sums them, and
`weighted_delta` divides that by the benchmark's price: the book's exposure in
benchmark deltas. Spot and implied variance come from the reference's analytics; a
held position's valuation stands in for a reference spot the analytics lack, and
without either the curves run against the percent move alone (`prices` and level
prices are null).

### Hypothetical legs and the Analyze tab

`legs` adds contracts to the profile as though they were held: each is found in the
underlying's current analytics (404 `UNKNOWN_CONTRACT` otherwise) and valued with the
same model, from `valuation_for` unless the account already values that contract, so
a candidate's curve and the held book's add up exactly. A leg on a held contract adds
to or offsets it. With `held=false` the profile is the legs alone: `room` and the floor
levels are null, because the account's floors measure its whole equity. The response
echoes `held` and `legs`. P&L is from the contracts' model values now, as for the held
book; what entering costs beyond the mids is the client's to subtract.

The Trade page's **Analyze** tab draws up to four candidate strategies (one to four
legs each, saved per underlying in this browser) on one chart through this route:
today, an optional custom date and a minute past each candidate's first expiry (its
expiring legs at intrinsic value), under an IV-shift slider, with or without the held
book (grey: the held book alone). The selected candidate's strikes have handles on the
chart: dragging one, or focusing it and pressing the arrow keys, moves that leg to the
nearest listed strike with a tradable contract; each leg also has a strike picker and
step buttons, side, ratio, type and expiry. P&L can be entered at the far sides (the
default, subtracting each leg's half-spread) or at mids; covered calls add their
shares' move. The price slices table reads every curve at percent moves, entered
prices, ±1σ and ±2σ of the last date's implied standard deviation, and each
candidate's breakevens at its expiry, linearly between the curve's points. **Send to
ticket** opens the order ticket for one leg, the strategy ticket for two to four, and
the shares-then-call dialog for a covered call; nothing is sent before the ticket's
own review.

### Strategy optimizer

`GET /api/underlyings/{symbol}/optimizer?target=&budget=` searches listed candidates
for a target price at a target date within a loss budget, for the Analyze tab's
optimizer, the Python client (`optimize_strategies`) and the MCP tool of the same name.
It needs only analytics, not a paper account, and has a running replay mirror.

| Parameter | Meaning |
| --- | --- |
| `target` | Required: the price expected at the target date |
| `budget` | Required: the most one candidate may lose, in dollars; candidates whose unit loses more are left out, and `quantity` is how many units fit |
| `date` | `YYYY-MM-DD`; valued at that date's regular close, or a leg's earlier expiry. Default: the first searched expiry's date |
| `expiry_ids` | Up to four expiry IDs; default the first four settling on or after the date. Earlier ones are listed in `skipped` |
| `structures` | Any of `vertical` (bull and bear call and put spreads up to eight strikes wide), `condor` (short iron condors, wings one to four strikes), `butterfly` (long call and put butterflies with equal wings), `calendar` (long calendars, same strike, near and far chosen expiries), `single` (long and short calls and puts) and `covered` (100 shares and a short call, equity and ETF underlyings only); default all |
| `rank` | `expected` (the user's expected P&L, default), `risk_neutral`, `pop` (the user's probability of profit) or `return_on_risk` |
| `vol_points` | The user's volatility change (-50 to 100): added to the distribution's volatility and to legs alive at the valuation time |
| `price` | `natural` (buy at asks, sell at bids; default) or `mid` |
| `limit`, `max_candidates` | Candidates returned (1 to 100, default 20) and valued (1 to 25000, default 5000) |

Strikes within 2.5 implied standard deviations of the spot and of the target (at most
40 per expiry, nearest their geometric middle) with tradable two-sided quotes and an
IV are combined. A candidate whose calls are net short (a naked short call) loses
without bound and is counted in `unbounded` rather than valued. When more bounded
candidates are listed than `max_candidates`, an even stride through the list is
valued, so every structure and expiry keeps its share, and `truncated` is true.

```text
at = min(target date's close, first leg's expiry)
leg value(S) = Black76(S*(S0 + (F - S0) T'/T)/S0, K, T', IV_strike + shift, D^(T'/T)), or intrinsic once expired
P&L(S) = sum(±ratio * 100 * value) + shares * (S - S0) - net * 100
v = implied_variance_until(at)                   (as the probability route's)
    or, without a term structure, IV_atm^2 * t   (first searched expiry settling at or after at)
market:  ln S_at ~ N(ln S0 - v/2, v)             (forward at spot)
user:    sigma_u = max(1%, sqrt(v / t) + vol_points / 100), v_u = sigma_u^2 t
         ln S_at ~ N(ln target - v_u/2, v_u)     (mean at the target)
```

When the chain has no at-the-money term structure for a valuation time (no strike
IVs to interpolate, say), the distributions use the at-the-money IV of the first
searched expiry settling then or later (else its nearest kept strike's), as strikes
are chosen, and `variance_fallback` is true; an expiry with no IV at all lists no
strikes and is skipped.

`max_loss` and `max_profit` (null when calls are net long) are the extremes over a
price grid from 0.01% to ten times the larger of spot and target, the strikes and both
distributions' nodes, at today's volatility. `breakevens` are bisected sign changes.
`pop` integrates each distribution between the breakevens with `price_odds`;
`expected` is a 161-node normal quadrature over ±6 standard deviations;
`return_on_risk` is the user's expected P&L over `max_loss`. Fees, early exercise and
assignment are not modelled. Under the market's own distribution a fairly quoted
candidate expects to lose about its spread, so `expected.risk_neutral` mainly measures
cost; an edge comes only from the user's view. These are derived estimates from
current quotes, not forecasts or advice.

A search can value tens of thousands of payoffs, so network requests run on a pool of
two worker threads; up to four more wait, and beyond that the route answers 503
`SEARCH_BUSY` with `Retry-After`. HTTP I/O threads never wait for a search. Invalid
queries answer 400 before queuing. A search stops early, answering what it valued
with `truncated` true, once its client closes the connection or the server is
stopping; one still queued at shutdown is dropped unrun and answers 503
`ENGINE_STOPPING`, so stopping waits only for searches already running.

### Hedge suggestions

`GET /api/risk/hedges?target=delta|vega|grid&scope=aggregate|SYMBOL&utilisation=1&betas=`
proposes, for one risk bucket, the smallest trade of each candidate hedge that
brings it back within its limit (times `utilisation`, 0 to 1). `delta` and `vega`
keep the bucket's dollar delta or vega within its limit at both ends of what its open
orders can reach; `grid` keeps every cell of the rules' spot × volatility grid for the
held book at or above the worst loss allowed, the larger of `-worst_grid_loss` (when
that limit is set) and the floor (the higher of the plan and soft floors, as equity
minus floor). A bucket for one underlying offers its shares (none for an index) and
the at-the-money call and put in the expiry nearest 30 days; the aggregate offers SPY
shares and SPY and SPX at-the-money options as beta hedges. It has replay and
saved-run mirrors.

For a `delta` target the beta hedges are sized on the beta-weighted dollar delta to
SPY rather than one for one: each underlying's reachable dollar delta is multiplied
by its beta (a negative beta swaps the ends) and summed, the hedge's unit delta by its
own underlying's beta (SPY 1, SPX 1 as SPY's index family), and the smallest size
brings that range within the limit. Betas come from the risk profile's sources:
`betas=SYMBOL:BETA,...` overrides (within 10 of zero), the reference, the index
family, then 252 daily closes (at least 20 paired returns) or 2,000 five-minute bars
(at least 30). `betas` lists each underlying's beta and source. When any underlying
has none, `betas_missing` names it, `beta_note` says so, no beta-weighted delta is
reported, and beta hedges fall back to one-for-one dollar delta (`sizing`:
`dollar_delta`). The risk limit itself still counts every underlying one for one, so
each hedge's residual shows both `delta_utilisation` and, when known,
`beta_weighted_delta_utilisation`; a beta-sized hedge can leave the one-for-one delta
over its limit. `vega` and `grid` targets are sized on vega and the grid as before.

```text
exposure after q units = exposure + q * unit exposure          (delta, vega)
cell after q units     = cell + q * unit cell - |q| * half-spread
```

Each constraint is linear in q, so the smallest whole size, buying or selling, is
exact: zero when the bucket is already within the target, none when no size up to the
cap meets it (shares 10,000,000, contracts 100,000). Each hedge lists its side,
quantity, price at the far side, premium, spread cost against the mid, the residual
exposure (beta-weighted too, with `instrument_beta`), worst cell and utilisations, what
it was sized on (`sizing`), and `exceeds_order_limit` when it is larger than
one order may be. Feasible hedges come first, then by spread cost, then by residual
risk. Fees are not included, and pending orders' grid reservations are not modelled;
the what-if and the ticket preview price both. An option hedge carries `order`, ready
for `POST /api/orders/what-if`; a share hedge carries `stock_trade`, the body of
`POST /api/stocks/trade`. On Positions, **Hedge suggestions** lists them; **Add to
what-if** puts an option hedge in the What-if panel to compare with other
adjustments, and **Ticket** opens its order or share ticket for review.

`GET /api/underlyings/{symbol}/probability?days=0,1,7,30&prices=` gives, for up to
eight horizons and twenty prices, the underlying's implied standard deviation of
log price `sigma` to each horizon, the cones `one_sd` and `two_sd` (`S e^(±k sigma)`),
and each price's odds. A horizon of 0 days ends at today's regular close, any other
that many calendar days after the analytics' market time. Before today's close the
variance is today's share of the front expiry's in regular-session time (as the
breach estimate's); past it, each expiry's at-the-money total variance (IV² times
its years) is interpolated linearly in calendar time, with flat volatility before
the first expiry and after the last, and never less than today's. The running replay
mirror is `/api/replay/underlyings/{symbol}/probability`.

```text
v = sigma^2
above = N((ln(S / K) - v / 2) / sqrt(v))   (lognormal, forward at spot)
below = 1 - above
touch = 2 N(-|ln(K / S)| / sqrt(v))        (driftless reflection estimate)
```

These are the market's risk-neutral odds under one volatility, not a forecast: skew
and drift are left out, and touch odds overstate a level reached only on paper.

## Smile dynamics

The scenario grid and the risk profile hold each strike's IV fixed as spot moves
(sticky strike); this is what the account's rules, margin scans and breach
estimates use, and it never changes. For analysis, `GET /api/risk?dynamics=` and
`GET /api/risk/profile?dynamics=` and `GET /api/risk/scenarios?dynamics=` (and their replay mirrors) also accept
`sticky_moneyness` and `sticky_delta`, which move each smile with the forward
along the underlying's fitted SSVI surface ([SVI and SSVI](svi.md)). The grid read
recomputes the account's configured spot × vol grid; the published snapshot's grid
stays sticky strike. The terminal's Positions page has a Smile selector beside the
grid and the profile.

```text
k  = ln(K / F),  k' = ln(K / F'),  s(k, T) = SSVI IV (theta interpolated in T)
sticky strike:    sigma' = smile_IV + shift
sticky moneyness: sigma' = smile_IV + s(k', T') - s(k, T) + shift
sticky delta:     sigma' = b(k*) + shift, where b(x) = smile_IV + s(x, T') - s(k, T)
                  and d1(k*, b(k*), T') = d1(k', b(k*) + shift, T')
d1(x, v, T) = (-x + v^2 T / 2) / (v sqrt(T));  shift = vol_points / 100 (grid) or iv / 100 (profile)
```

The difference of fitted IVs moves the contract's own market IV, so a fit
residual is carried along unchanged and an unmoved book is exactly the sticky
strike book. T' is the contract's remaining life on the curve's date (T on the
grid and today's curve), so later profile curves also roll down the surface
without a move. Theta is linear in T through zero and the fitted knots and grows
in proportion to T beyond the last, as the SSVI construction requires. Sticky delta
keeps the smile fixed in forward-delta space (Black d1 at each point's own vol);
with no vol shift that is the same as sticky moneyness at a fixed tenor, so the two
differ only under a vol shock, which sticky delta applies in delta space. The
sticky-delta root is bracketed outward from k' and bisected; a shock that leaves no
positive vol is floored as usual. An underlying without a successful SSVI fit stays
sticky strike and is listed in `surface_fallback` (in `scenarios` for the grid, at
the top level of the profile), and `dynamics` echoes the choice. The surface is the
same cached fit the surface API serves; the first such read for a snapshot fits
it on that API worker, never in the engine.

**How it changes skew P&L on big moves.** On a negative skew (puts richer than
calls) the smile slides down the strike axis with spot under sticky moneyness:
after a selloff a fixed strike sits higher on the moved smile and its IV falls;
after a rally it rises. Long downside protection therefore gains less in a
selloff than sticky strike shows, and short upside calls lose more in a rally.
Sticky strike, conversely, assumes a fixed strike keeps its IV however far spot
travels. Neither is a forecast. Index skews have often behaved closer to sticky
strike, or with IV rising in selloffs beyond it, so these choices bracket model
risk rather than predict it. An illustration on a synthetic 30-day SPX book (SSVI
ρ=−0.7, η=1.1, γ=0.4, ATM ~18%; F=5000; P&L in dollars per contract):

With no vol shock (sticky delta then equals sticky moneyness):

| Position | Move | Sticky strike | Sticky moneyness / delta |
| --- | ---: | ---: | ---: |
| Long 4750 put (IV 22.2%) | −10% | +24,468 | +22,174 (IV 14.8%) |
| Long 4750 put | +10% | −3,569 | −3,111 (IV 28.3%) |
| Long 5000 call (IV 18.5%) | +10% | +39,693 | +40,935 (IV 25.2%) |
| Short 5250 call (IV 15.1%) | +10% | −25,153 | −27,953 (IV 21.9%) |

With +5 vol points, sticky delta applies the shock where the moved contract sits
in delta space, so it parts from sticky moneyness:

| Position | Move | Sticky strike | Sticky moneyness | Sticky delta |
| --- | ---: | ---: | ---: | ---: |
| Long 4750 put | −10% | +26,432 | +23,618 | +23,888 |
| Long 4750 put | +10% | −3,222 | −2,380 | −2,604 |
| Short 5250 call | +10% | −27,143 | −30,402 | −30,099 |

Tests in
`tests/trading/dynamics_test.cpp` cover the interpolation, both definitions
(including the delta-matching equation), flat surfaces, exact sticky-strike
reproduction of the snapshot grid, the direction of these effects for a long call,
roll-down on later curves and the fallback reporting.

## Expiry and explicit settlement

At `OptionContract::expiry_time()` orders cancel and open positions become
`awaiting_settlement`: 09:30 ET for AM-settled contracts, and for PM-settled ones their
last trade, 16:00 or 16:15 for ETF options that trade until then (13:00 and 13:15 on
early-close days). No underlying quote is automatically taken as settlement.
The caller supplies the authoritative reference with `settle(OSI, value, time)`
after expiry. The hosting engine obtains PM closing prints, AM opening-print
approximations in demo/replay, or explicitly imported AM values for live feeds,
passing optional provenance to the core as the fourth argument.

```text
intrinsic = max(0, omega * (settlement_reference - strike))
cash payment = q * M * intrinsic
realised settlement P&L = cash payment - signed remaining basis
```

`GET /api/settlements` (read scope, optional `account`) lists all the account's
settlements, newest first across attempts. Each record has `symbol` (OSI),
`underlying`, `expiry`, `settlement` (`AM`/`PM`), the exact decimal-string `value`,
`time`, signed `quantity`, signed `cash` after the settlement `fee`, and gross
`realised` P&L released by this settlement before fees. Cash excludes any separate
share delivery. `source` carries `kind` and, when recorded, `provider`, `symbol`
and `quote_time`; it is null for older journals without provenance. Recovery
rebuilds these records from the settlement events without changing persisted state.
The same read is available at `/api/replay/settlements` and
`/api/replay/history/ID/settlements`. Settlement round trips in `/api/trades`
carry `settlement_value` and `settlement_source` (the source kind), null when
unavailable or outside settlement closures. The trades CSV keeps both as tail
columns before the final `time_stop_orders`, preserving earlier column
positions. The terminal Journal shows
the reference and source on settlement rows and a settlements list across attempts;
waiting positions name the expected print or manual import.

Settlement removes the position and is exactly once per OSI. Cash settlement is free;
physical delivery charges the itemized schedule’s `exercise` fee per contract when
one is configured. OTM options pay zero and release their entire basis into realised P&L. Negative
references, premature settlement, missing positions or unknown contracts reject.
AM-settled series stop trading at the regular close the business day before expiry
(`last_trade_time`) and wait for an explicit settlement value. Revision 3 demo
and scenario quotes follow that boundary for SPX, NDX, RUT, VIX and VIXW: the
AM series still quotes on its last trading day, disappears at the regular option
close, and is absent on its settlement date. Same-date SPXW, NDXP and RUTW PM
series remain tradable through that date's 16:00 close; XSP has PM series only.
VIX and VIXW both settle AM, usually Wednesday, and last trade Tuesday.
American equity and ETF options held into expiry deliver shares at settlement (see
[instruments](#instruments-and-prices)); the settlement closure keeps the option's
trade at intrinsic value.

## Journal, recovery and failure handling

`FileJournal` creates a new exclusive single-writer POSIX regular file or resumes
an existing verified one. Each line is one **atomic reducer transaction**. Its
payload contains ordered typed outcomes (`order_accepted`, `order_rejected`,
`fill`, `cancel`, `definition`, `limit_change`, `kill_trip`, `kill_reset`,
`settlement`, `day_rollover`, `session_start`) and the resulting reducer state, whole
or as its change from the record before. The published snapshot is derived from the
state, so it is not recorded. Grouping a partial fill and IOC cancellation in one committed
line prevents recovery from exposing half a command. The top-level type names the
command (`submit`, `market`, etc.); outcomes are in `payload.events`. Empty market
batches on an idle account are not transactions, so a flat account without working
orders adds no records while the feed polls; with positions or open orders, every
batch is recorded because time moves their rules and marks.

Creation and resume acquire a non-blocking exclusive advisory `flock(LOCK_EX |
LOCK_NB)` on the journal file before reading or writing its contents, held for the
session's lifetime and released when the journal closes on destruction (including
engine shutdown). `flock` is used instead of process-owned `fcntl` record locks so
separate opens in the same process also conflict, and closing a recovery reader
does not release the writer's lock. Another owner causes `JOURNAL_LOCKED` without
reading or changing the journal: analytics continue, paper trading is disabled,
and all trading writes return 503 `TRADING_UNAVAILABLE`. `/api/status`'s
`trading.reason` and the daemon's startup diagnostic name the path and say it is
in use by another openportd; use `--paper-journal` to choose another file or
`--no-paper`. All writers must honor the advisory lock; do not replace or unlink
an active journal.

Every record has exactly `seq`, `time`, `type`, `payload`, `prev_hash`, `hash`.
Sequence starts at 1; the genesis previous hash is 64 ASCII zeroes. New payloads use
schema 3 and tick policy `v2` (the `index-v1` table plus equity and ETF classes): the
`events`, the command's `decision`, and either `state`, the whole reducer state (a
checkpoint), or `delta`, its change from the previous record's state. A delta is a
tree of nodes: `{"v": value}` replaces a value, `{"o": {key: node}, "d": [keys]}`
changes some of an object's keys and removes others, and `{"a": {"index": node},
"n": length}` changes some of an array's elements and sets its length, new elements
arriving whole. The first record is a checkpoint, as are the first after every
recovery, one in every 1,000 records, and any record whose change is more than half
the size of the last checkpoint. Recovery rebuilds each record's state from the one
before and checks it against the record's sequence and time. Order, fill and closure
history therefore costs a record only what the transaction added: records stay a few
hundred bytes however long an account trades, where schema 2 wrote the whole state
and snapshot every time (a 14,063-record journal of mostly idle polls shrank from
60.6 MB to 5.4 MB, and its startup from 5.8 s to 0.6 s).

Schema 2 added `config.rules`, the evaluation, attempts and closures to the state and
snapshot, and `system` to orders; it also records `evaluation_passed`,
`evaluation_failed` (each with a `code` when a rule other than the target or the floor
decided it), `evaluation_day`, `day_locked`, `account_reset` and `payout` outcomes. Later
schema 2 fields (conditional and bracket orders, the funded phase, payout rules and
records, qualifying days) default when absent, so earlier schema 2 journals recover
unchanged; multi-leg orders record their `legs`. F24 appends TIF enum values without
renumbering DAY/IOC/GTC. `request.good_till` is written only for GTD; order
`limit_ticks` only for automatic limit flatten pricing (including zero). Default
orders and market flatten commands retain their journal bytes. Recovery reads schema 1 and 2
records, which hold the whole state and snapshot, in any mix with schema 3. Schema 1
journals keep their original keys required and default the added ones, and the
evaluation starts from the first record with the recorded starting cash. Resumed
schema 1 and 2 journals continue with schema 3 records; builds from before schema 3
refuse them rather than guess. The canonical encoding is compact nlohmann JSON
3.12 serialization: recursively lexicographically sorted object keys, array order
preserved, UTF-8 strings, integer money, round-trip decimal doubles, no whitespace.
Hash is lowercase hex SHA-256 (OpenSSL EVP) over the canonical entire record with
**only the `hash` member omitted**. Newline is not hashed. Verification also requires
the original line to equal canonical serialization, detecting duplicate keys and
whitespace alterations. Hashes, sequence and monotone time are verified.

Live paper accounts write and sync every complete line before publishing the
transaction: with `F_FULLFSYNC` on macOS, whose `fsync` leaves data in the drive's
cache for a power loss to lose, and `fsync` elsewhere. This is `FileJournal`'s
default policy for both creation and resume.

Replay, drill and scenario journals, including kept runs, use batched syncs. Each
append writes its complete line immediately; the first record syncs, then an append
syncs when at least 250 ms of steady-clock time has passed since the last sync.
`flush()` syncs pending records immediately, and destruction attempts a final flush
without throwing. Replay pause, stop, finish and teardown flush on the owner thread
before handing on the journal. A process crash loses no completed writes. A power
cut or kernel panic may lose the last quarter second of replay records. Readers can
see complete lines before the sync. Batching is append-driven: an idle journal
waits for another append or a boundary. Compaction and repair still sync fully.

A transaction that would leave less than 64 MiB free on the disk is refused. The
error names the filesystem device and directory basename, free bytes and the
67,108,864-byte reserve without exposing an absolute path. Thus a
full disk stops trading without tearing the journal. If a write is torn anyway, writable resume
refuses the journal (live accounts remain readable at the verified prefix) until, with openportd stopped, `openportd --repair-journals` cuts
the torn last line off it and each account journal beside it, keeping the original as
`FILE.torn-YYYYMMDDTHHMMSSZ`; damage before the last line is reported and left alone.

An account exists from its journal's first record, its `session_start`. A new journal
whose first record the disk refused (or that a crash cut off before it) is empty, or
holds only that torn line, which the repair cuts off: it holds no transaction, so
nothing is lost by starting over. After freeing space, the main account starts
afresh on its empty journal; startup removes a named account's empty journal and its
name file, so the name can be created again. A failed `POST /api/accounts` removes
the journal and name file it made, and a replay that fails to start removes its
run's files, so neither leaves an account or history entry behind. The repair
reports an empty journal as `empty, holds no transaction`. Opening a journal also
checks, once it holds the lock, that the path still names the file it locked, so a
writer never appends to one another process removed.
An append failure throws `JOURNAL_IO`, leaves the prior account/orders visible, sets the
snapshot's `journal_failed` flag, and refuses every subsequent command. The disk
outcome can be indeterminate after a failed write/sync: **stop trading and recover**;
do not retry against the old in-memory account. A custom `Journal` must honor the
same complete-write or error contract and its chosen durability policy. A deferred
sync failure also latches `JOURNAL_IO`; the replay account stops trading and reports
the failure while preserving its last published state. File creation uses restrictive
permissions; the hosting application should durably provision the containing directory.

Recovery verifies the chain and restores **recorded outcomes/state**, including
cash, basis residues, liquidity budgets, observation high-water marks, definitions,
orders, fills, limits, kill state and daily baseline, and the published snapshot:
recorded (schemas 1 and 2) or derived from the recovered state as the reducer
derived it (schema 3). It never reruns market matching or reprices from new data. A resumed sink
must match the recovered head and sequence exactly. Subsequent commands calculate
normally from recovered state.

A non-newline-terminated final suffix is reported by
`JournalRecovery::truncated_final_line` and ignored, even if it looks like complete
JSON. Broken newline-terminated records fail loudly. Read-only recovery of the
verified prefix is possible, but `resume` refuses a torn suffix. The operator must
explicitly preserve/export the verified prefix before resuming; no recovery API
silently truncates the file. Persist the reported head independently and pass it
to `read`/`verify_journal` to detect removal of complete trailing records. An
unanchored hash chain cannot detect wholesale history replacement or a clean tail
truncation.

Every record's whole state stays recoverable, so the journal is a complete audit of
the account at each transaction. All v1 orders/fills remain in memory and the
snapshot's `recent_*` arrays; there is no retention cap, and a checkpoint grows with
the account's history, one per thousand records.

Inside-fill and walk fields are sparse: `inside_fill_percent` is written only when nonzero;
`walk` only when present; `walked_at` only after a walk steps or is changed.
Walk changes record new and previous settings only when requested. Accounts with
inside fills off and orders without walks keep their prior journal bytes. Recovery
defaults absent fields; old journals continue to load and replay hashes remain
verifiable on the same platform.

### Compacting older journals

Records written before schema 3 keep their whole states until rewritten. Stop
openportd, then run `openportd --compact-journals` (with the same `--paper-journal`
if you set one; in Docker, `docker run --rm -v openport:/var/lib/openport openport
--compact-journals` while the server's container is stopped): it rewrites the main journal and each account journal beside it
that has such records, and reports the sizes. A rewrite takes the writer's lock, so
a journal in use is left as it is with `JOURNAL_LOCKED`; keeps every transaction's
sequence, time, type, events and decision; reads each rewritten record back before
writing it; and must recover to the same account before it replaces the journal. A
final schema 1 record stays as it is, since recovery completes that journal's
evaluation from it. The original stays beside it as `FILE.bak` (then `.bak2` and so
on): move it back to undo, or delete it once you are satisfied. The rewrite has a
new hash chain, so replace any head you persisted to anchor the old one.
`TradingSession::expand` does the reverse, writing every state and snapshot whole
(schema 2), for audit tools and for returning a journal to an older build.
Identical inputs give identical output on the same build. Exact monetary results
are portable; floating-point analytics are not promised bit-identical across
compilers/architectures, although recovery restores the recorded doubles.

## Reason codes

`NEWS_BLACKOUT` refuses/cancels scoped openings during an enabled news window.
`HOLD_RESTRICTED` refuses/cancels openings after a holding cutoff and fails an
attempt at rollover if a position crossed the boundary. See F17/F59 above for
string timestamp/cutoff evidence and the `kind:scope` holding convention.

`TIME_LIMIT` fails an overdue evaluation or verification; `INACTIVITY` fails an attempt after too
many calendar days without an own execution. `INSTRUMENT_NOT_ALLOWED` refuses
opening options or shares outside the plan whitelist. `OUTSIDE_PLAN_HOURS` refuses
opening orders and cancels working openings outside the plan window.
`FLAT_TIME` refuses and cancels openings after mandatory flat time until day end.
`OVERNIGHT_HOLD` fails an active attempt holding tradable positions at rollover.

| Code(s) | Meaning |
| --- | --- |
| `NONE` | Success |
| `INVALID_MONEY`, `ARITHMETIC_OVERFLOW` | Invalid decimal/value or checked arithmetic range exceeded |
| `AMERICAN_UNSUPPORTED`, `NONSTANDARD_UNSUPPORTED`, `ROOT_UNSUPPORTED` | Outside v1 instrument scope |
| `INVALID_CONTRACT`, `UNKNOWN_CONTRACT` | Invalid/conflicting terms, or missing resolved definition |
| `INVALID_ORDER`, `DUPLICATE_CLIENT_ID`, `INVALID_TICK` | Malformed order, a key reused with other terms, invalid price increment |
| `INVALID_QUOTE`, `STALE_QUOTE`, `MISSING_VALUATION` | No executable book, stale/incomplete marks, missing/stale/invalid Greeks |
| `MAX_VOLUME_SHARE` | Held plus opening contracts exceed the plan percentage of current-date option volume, or that volume is unknown/stale (acceptance and fill checks) |
| `HEDGING` | Opening dollar delta opposes held delta on the same underlying within this account (reducer rule) |
| `COUNTER_POSITION` | Opening dollar delta opposes held or working opening delta in another live account on the same underlying (acceptance and fill checks; acceptance refusal is not journaled) |
| `STOP_REQUIRED` | Entry has no protective bracket stop, or cancellation would remove a required stop from an open position |
| `TRADE_CONSISTENCY` | Best profitable closed whole trade must fit the attempt-profit percentage limit |
| `MIN_HOLD` | User reduction is too young; `actual` and `limit` are seconds, `scope` is the underlying in HTTP responses (the reducer identifies the option/share symbol) |
| `MICROSCALPING` | Positive net profit from short round trips exceeds its allowed share of attempt profit; pass waits |
| `MIN_TRADES` | Pass waits for the required count of closed whole option trades |
| `MAX_TRADE_RISK` | Entry risk exceeds its plan cap; `actual` and `limit` are dollars, `scope` is `trade`; actual is null for unbounded or unknown risk |
| `MAX_CONTRACTS_HELD` | Projected held options plus working opening contracts exceed the plan cap; `actual` and `limit` are contract counts, `scope` is `account` |
| `MAX_ORDER_CONTRACTS`, `PRICE_BAND` | Quantity or protected-price bound exceeded |
| `DELTA_LIMIT`, `VEGA_LIMIT` | The order raises worst reachable exposure above an underlying/aggregate limit |
| `SOFT_FLOOR`, `TRADE_LIMIT`, `COOLDOWN`, `PROFIT_LOCK` | Personal guardrail is active; new opening orders and manual latch resets are refused while closing orders and exits remain available |
| `DAILY_LOSS_LIMIT` | The plan's daily loss limit locked the day (opening orders refused and open orders cancelled until rollover) or failed the attempt |
| `PROFIT_TARGET`, `DRAWDOWN_FLOOR` | Decision codes: the target passed the attempt, or the floor failed it |
| `MIN_TRADING_DAYS`, `MIN_PROFITABLE_DAYS`, `CONSISTENCY` | Objective codes: what a pass still waits for |
| `DAILY_LOSS`, `KILL_SWITCH` | Daily equity allowance breached, or an order would open/increase exposure (or exercise) while the kill latch is active |
| `RISK_CHANGED` | Fill/limit-change recheck failed; original cause at the start of the message, its `actual`, `limit` and `scope` kept on the order. Scaling rechecks keep `SCALING_LIMIT` as their code |
| `PLAYBOOK_TRAILING_STOP`, `PLAYBOOK_DTE_STOP`, `PLAYBOOK_DAYS_IN_TRADE_STOP` | Automatic playbook trailing, DTE or business-day deadline cancelled working protection; see [playbook rules](playbooks.md) |
| `PLAYBOOK_TIME_STOP` | Automatic playbook deadline cancelled an entry or working exit; message `Playbook time stop`. Older replay drivers retain `USER_CANCEL` |
| `IOC_REMAINDER`, `USER_CANCEL`, `DAY_END` | IOC remainder (a stop exit's re-arms instead), explicit cancellation, the end of a DAY order's session (a triggered one's activation session) or an EXTO trading date |
| `SESSION_CLOSED`, `EXPIRED`, `AWAITING_SETTLEMENT` | Outside the product's sessions (or an AM-settled series after its last regular close), expiry or last-trade boundary, or pending settlement quality flag |
| `LIMIT_ONLY` | Overnight/curb executions require limits. Plain market entry/flatten is refused. Only EXTO/GTC_EXTO simulator-managed stops and brackets work there; legacy protection waits for regular hours |
| `GTD_END` | The order reached its explicit `good_till` timestamp |
| `CORPORATE_ACTION` | A split went ex on the order's stock: working share orders on it are cancelled, as their prices no longer apply |
| `FEED_STALLED` | Market data lags what a healthy feed would show by more than `max_quote_age` (a delayed feed: at least three minutes); message includes the lag behind the wall clock |
| `REPLAY_FAST_FORWARD` | The replay is preparing its start state; wait before submitting orders or changing playback |
| `REPLAY_STEPPING` | A lockstep step (`PUT /api/replay {"until"}`) is playing; orders wait for its response and then use the paused market time |
| `REPLAY_READ_ONLY`, `REPLAY_RUNNING` | A finished run refuses writes; a running run cannot be opened as history or deleted |
| `ACCOUNT_TYPE` | A cash account's or IRA's order, bracket exit, exercise, abandon or share sale would leave it holding a call no shares (or, in an IRA, no long call) cover, or short shares, now or once the open orders fill |
| `RUN_ENDED` | A saved replay run's order that was still working or armed when the run ended: its archive shows it cancelled, as it can no longer fill. The journal keeps it as it was |
| `REPLAY_NOT_RESTARTABLE` | HTTP 409: a source run cannot be restarted with its commands because its inputs, calendar, driver or journal integrity do not permit deterministic re-execution |
| `REPLAY_NOT_RESUMABLE` | HTTP 409: a saved replay run cannot resume: it has ended, or it was recorded by an older driver, or its recording, scenario or the exchange calendar has changed; the message says which |
| `REPLAY_HISTORY_FAILED` | A saved replay run's journal cannot be opened, as when it was edited; the message gives the reason |
| `MARKET_HALTED` | A market-wide circuit breaker or authored scenario halt has stopped trading; the message identifies the halt and resume time |
| `INVALID_REQUEST` | HTTP 400: a malformed body or query, including an order no market could make valid (see the HTTP errors below); nothing is recorded |
| `UNKNOWN_ACCOUNT`, `ACCOUNTS_UNSUPPORTED` | HTTP 404 for an `account=` the server does not have; 409 when creating an account on a server that keeps one (a replay, or no accounts directory) |
| `LIMITS_REVISION` | HTTP 409: `expected_revision` on `PUT /api/risk/limits` or `/api/risk/guardrails` is not the current `limits_revision`; refetch and retry |
| `TRADING_UNAVAILABLE` | HTTP 503: the command inbox is full or the engine is stopping (with `Retry-After`), or the account's journal failed or is locked |
| `PASS_ODDS_UNAVAILABLE` | HTTP 422: pass odds need an evaluation rule, complete current marks and ten completed days with intraday extremes |
| `INVALID_SETTLEMENT`, `ALREADY_SETTLED` | Invalid/premature settlement or already settled OSI |
| `UNKNOWN_ORDER`, `ORDER_TERMINAL` | Invalid cancellation target or already finished order |
| `INVALID_LIMITS`, `INVALID_TIME`, `INVALID_SCENARIO`, `INVALID_REASON` | Invalid control/configuration input |
| `ACCOUNT_DAMAGED` | Read-only verified prefix; diagnose with `--repair-journals --dry-run`; restore a verified backup for mid-file damage |
| `JOURNAL_IO`, `JOURNAL_CORRUPT` | Persistence stop condition or invalid/tampered recovery chain/schema |
| `JOURNAL_LOCKED` | Journal already owned by another writer; analytics remain available |
| `EVALUATION_CLOSED` | The attempt passed or failed; closing orders, Flatten and disposal remain allowed. Reset before opening, adding to or reversing positions |
| `BUYING_POWER`, `BUY_ONLY`, `EXPIRY_CUTOFF` | Account-rule rejections (see Account rules); `EXPIRY_CUTOFF` also cancels every open order on a contract at the account's pre-expiry cutoff |
| `ACCOUNT_RESET` | Working order cancelled by a reset |
| `SCALING_LIMIT` | An increase would exceed the session’s scaling cap on held option contracts, counting working opening orders and every leg; refused at acceptance and cancelled with this code at fill. Shares do not count; reducing orders remain allowed |
| `INVALID_RULES` | Invalid account-size scaling (evaluation phase, profit/increase percentages outside 1–100, payouts outside 0–100, days outside 1–366 or maximum below starting balance), invalid scaling steps (over 16, first profit not zero, non-increasing profits, decreasing limits or contracts outside 1–10,000), negative rule money, a negative cutoff or one of a day or more, a plan name over 64 bytes or one that names a preset whose balance and rules these are not, payout percentages outside 0-100, consistency limits outside 1-100, or nonpositive caps, a negative payout buffer or buffer_payouts outside 0-100, slippage or impact outside 0-10 ticks, fill latency outside 0-60,000 ms, inside fills outside 0-100%, an unknown margin mode or account type, house margin outside 0-400%, a portfolio vol shock outside 0-50 points, a cash or IRA account without strategy margin and enforced buying power, a funded phase with a profit target, a consistency percentage outside 0-100, minimum days outside 0-366, a day end outside 16:15-24:00, or a floor with two locks, or a static one with any. Trade-entry rule ranges are a contracts held cap of 0–100000, nonnegative risk money and risk percentage 0–100. Malformed margin settings (`margin`, `account_type`, `house_margin_percent`, `pm_vol_shock`) also return 400 `INVALID_RULES`. Other malformed legacy fields, such as fractional ticks, a payout qualifying-day count below one, or payout arrays over 64 entries, return 400 `INVALID_REQUEST` |
| `OCO_FILLED`, `POSITION_CLOSED` | Bracket sibling cancelled when the other exit filled completely, remaining entry cancelled by an exit fill, or an exit whose held legs closed |
| `PAYOUT_UNAVAILABLE`, `PAYOUT_NOT_ELIGIBLE`, `INVALID_PAYOUT` | Not a funded, active account; a payout requirement unmet; or an amount that is not whole cents or outside the minimum and maximum |
| `PLAN_LOCKED` | A locked verification/funded preset was requested without the current prerequisite pass: that preset's own balance and trading rules; restarting the current plan is allowed under its reset terms |
| `RESET_LIMIT` | HTTP 409: the same-plan reset would exceed `max_resets`; `actual` is the attempted reset count, `limit` is the allowed count. Choose a different plan for a new purchase |
| `INVALID_NOTE`, `UNKNOWN_TRADE` | An order or trade note or tag past its limits, or a note on a fill that opens no trade |
| `INVALID_ALERT`, `UNKNOWN_ALERT` | Alert terms that do not fit their scope, or a 101st alert; an alert ID the account does not have |
| `INVALID_GROUP` | An order's `group` names no open round trip or whole trade holding one on its underlying; grouping names a fully closed trade, mixed underlyings or too few round trips; or ungrouping names a closed round trip |
| `DEFINED_RISK` | A defined-risk plan's order, bracket exit or exercise would leave a short option uncovered, now or once the open orders fill |
| `OPEN_LOSS_CAP` | A trade's open loss, net across its legs, reached the plan's open-loss cap: adding to it is refused (`block_adds`, and under `flatten` while its close is pending), or the attempt failed (`fail`); `system:open_loss:N` closes the whole trade under `flatten` |
| `MINUTE_VOLUME` | An opening order's contracts exceed the plan's percentage of the last minute's volume, or that volume is unknown so the cap cannot be checked; evidence as for `MAX_VOLUME_SHARE` |
| `BEST_POSITION` | Objective: profit above the best-position cap does not count toward the target |
| `LOSS_STREAK`, `SIZE_CREEP` | Tilt flags from the discipline guardrails; `SIZE_CREEP` also refuses an oversized opening order with `size_creep_block` |

## Engine integration and HTTP API

Paper trading is enabled by default in `openportd`; `--no-paper` disables it and
reports `PAPER_DISABLED` in status.

### Accounts

The journal at `--paper-journal` is the **main** account. More named accounts live
in an `accounts` directory beside it, one journal each (`accounts/<id>.jsonl`, the
display name in `accounts/<id>.name`). Accounts list after main in ID order
(`swing-50k` before `swing-50k-2`), both when created and when recovered at startup,
so a restart keeps the list as it was. Every
account trades the same market at once: each receives the quotes and valuations for
its own positions, open orders and commands, marks, fills, settles, rolls its day
and applies its rules whichever account the terminal shows. An account whose
journal cannot open, or fails later, reports why in its status and refuses writes;
the others carry on.

`POST /api/accounts` takes a `name` (1 to 64 characters, counted as Unicode code
points, none of them a control character) and either a
preset `plan` or `initial_cash` and complete `rules`, and returns 201 with the new
account's `id`, a slug of the name made unique (`swing-50k`, `swing-50k-2`). Funded
plans are refused: they start by resetting an account that passed the evaluation.
Every other trading route acts on the main account, or on another given as
`account=<id>` in the query (reads and writes alike; a write takes no other query
parameter). An unknown account is 404 `UNKNOWN_ACCOUNT`; a malformed one is 400.
Every route and the token check read query keys and values percent-decoded, so
`%61ccount=beta` is `account=beta` and needs beta's scope. A key given twice in any
spelling, or a malformed escape, is 400.
Without an accounts directory (`--paper-journal` empty in tests) the server keeps
one account, as a replay does, and account creation returns 409 `ACCOUNTS_UNSUPPORTED`,
which a retry cannot change.

`PATCH /api/accounts/{id}` takes `name` and/or boolean `archived`; `DELETE
/api/accounts/{id}` deletes a named live account. Both require admin. Main, replay
and sandbox accounts return 403 `ACCOUNT_PROTECTED`; an unknown ID returns 404
`UNKNOWN_ACCOUNT`, malformed bodies 400 `INVALID_REQUEST`. Names use creation's
validation; duplicate display names are allowed, and renaming never changes the ID.

Archiving freezes the account, including working orders, positions, rollover,
alerts and playbooks. Writes return 409 `ACCOUNT_ARCHIVED`. Unarchiving resumes
processing, including those working orders. Archived accounts remain readable with
`?account=ID`, but disappear from default lists and status ticks. `GET /api/accounts?archived=true`
includes them with `archived: true`. The `<id>.archived` file is atomically replaced,
as is `<id>.name`; missing archive files mean active. A combined patch writes the
name before the archive flag, so a crash between them may preserve just the rename.

Deletion refuses positions, working orders or unreadable journals with 409
`ACCOUNT_NOT_EMPTY` unless archived first. It retains files under
`accounts/deleted/<id>/`, releases the session and account observation/staging state,
and permanently reserves the ID. The durable destination directory is the deletion
tombstone; startup completes interrupted moves. Retained files are not served over
HTTP. An already dispatched notification may finish after deletion.

Account creation replies, account lists/status `trading`, account `rules`, and replay
history summaries report `plan_id` beside the plan name. It is derived from the
stored name, starting cash and objective rules, including old journals; execution,
fee and margin overrides do not change it. Custom or unrecognized rules return
null. When read-back rules are used in a custom request, `plan_id` is accepted
as metadata and ignored; the server always derives it from the actual rules.
Compact past-attempt summaries lack full rules and report null rather than
guessing from the display name. Replay control's existing `plan` remains its starting
preset ID; history `plan_id` describes the recovered account's current rules.

`POST /api/accounts` and replay starts (`POST /api/replay`) accept
`copy_settings_from: "main"` or another live account ID, including an archived
account. They copy **active** limits (including underlying overrides) and personal
guardrails, using the same validators as risk writes. Pending values, kill latches,
opening counts and cooldown state are not copied. The new account's first journal
record includes the settings; replay verification and recovery never consult the
source again. Unknown sources return 404 `UNKNOWN_ACCOUNT`; malformed IDs return
400 `INVALID_REQUEST`. Copying is not offered when resuming an existing run.

#### Account groups and copied orders

An **account group** names up to sixteen live accounts (`main` for the main account)
and how a copied order is sized across them. `PUT /api/account-groups/{name}` creates
or replaces one with `{"allocation": "ratio" | "equity", "members": [{"account":
"main", "ratio": "1"}, ...]}`; `DELETE` removes it and `GET /api/account-groups`
lists them. Names follow the account ID rules. Writes need admin scope, every member
must exist (404 `UNKNOWN_ACCOUNT` otherwise), and the server keeps at most 32 groups
in `account-groups.json` beside the main journal, replaced atomically. Groups are
configuration, not trading: no journal records them, and deleting one changes no
account. Sandbox servers and visitors are not offered them.

`POST /api/orders/copy` takes `{"group": name, "order": {...}}`, the order as
`POST /api/orders` takes it, and mirrors it in the server layer: each member account
receives its own submission of the order at its own size, through its own inbox,
rule checks, journal and client order ID namespace, exactly as if it had been posted
to `/api/orders?account=<id>`. Nothing is shared between the submissions; one account's
refusal (a delta limit, a plan rule, a decided attempt) does not stop another's.

- **Ratio**: each member trades the order's quantity times its ratio (0.0001 to 100),
  rounded down: a 3-lot copied at ratios 1, 0.5 and 2.5 is 3, 1 and 7.
- **Equity**: the order's quantity is split across the members in proportion to their
  marked equity when the request arrives, each taking the floor of its share and the
  largest remainders (earlier members first on ties) the units rounding leaves, so the
  sizes add up to the order's. A member whose positions are not all marked is skipped,
  sent nothing, with `code: "STALE_QUOTE"` in its result; the others share the whole
  order.

The sizes are computed once, before the submissions, and each journal records only its
own order. A member allocated no units is skipped and sent nothing. The reply is 200
when every member accepted and 207 when any was skipped or refused, with
`results[]` giving each account, its `quantity`, `ok`, and its own HTTP `status` and
`response` (the `SubmitOrderResponse` or error it would have answered alone).
`POST /api/orders/copy/preview` previews every member's order on a private copy, as
`/api/orders/preview` does. A retry with the same `client_order_id` is answered by each
account as first submitted; under equity allocation a retry after equity moved may
size an account differently and get `DUPLICATE_CLIENT_ID` from it. Copied orders take
no `then`, `oco` or `exits_only` (each account sizes its own exits; brackets are
allowed and size with each fill), nor a query parameter. They need `trade:*` (or
admin) scope, since they trade every member.

The terminal's **Manage accounts** dialog creates, edits and deletes groups, and the
option ticket's **Copy to group** sends the order to a group instead of the active
account, listing each account's outcome.

Risk replies include `pending_requires_reset`. It is true when pending limits or
guardrails exist and the replay's last complete batch does not reach a later plan
trading day. Reset the replay account while it is writable to apply them now;
resetting discards the current attempt's positions and orders as usual. Runs with a
later trading day apply queued values at rollover and report false; another session
on the same trading day does not suffice. Live accounts always report false.

The terminal’s account switcher opens **Manage accounts** for rename, archive,
unarchive and confirmed deletion, with archived accounts grouped separately. New
account and Replay start forms offer **Copy limits and guardrails from**.

Overrides replace the common `per_underlying` cap for that symbol. PUT limits
replaces the whole map: omission or `{}` removes overrides. Symbols use 1–16
uppercase letters, digits or dots; unsubscribed symbols are allowed so settings
can be prepared before market data arrives. Values have the same finite,
nonnegative validation as aggregate/common caps. In evaluations each tightening
applies now and each loosening queues, including removal when the fallback is
looser. Practice changes are immediate. GET risk returns active and pending maps;
the terminal limits editor adds, edits and removes rows, and risk buckets identify
the effective override or common cap. Revisions and replay mirror behavior match
other limits. Dropping redundant overrides needs no new replay driver: older runs
cannot contain overrides, so their journal bytes are unchanged and driver-6 runs
remain resumable.

For a new main account, `--paper-fill-model as_displayed|conservative|midpoint`
selects the same execution preset as reset. `--paper-slippage-ticks` (0–10),
`--paper-fill-latency-ms` (0–60000) and `--paper-impact-ticks` (0–10) override its
values regardless of flag order. Existing journals retain their saved rules, as
with `--plan`. Named accounts and resets use their request's rules/model; sandboxes
keep displayed practice fills. The flags apply only to main, unlike `--paper-fee`.

### Commands and views

The engine thread alone owns every session. A bounded FIFO inbox (256 pending
commands) sequences writes, applies the drained market batch first, then applies
commands in ingress order. HTTP threads enqueue
and return; completions are posted onto the requesting Beast session executor.
A full inbox or stopping engine returns 503 `TRADING_UNAVAILABLE` with
`Retry-After: 1`: the inbox drains as the engine applies commands, and an order sent
again with the same client ID and terms is answered once. A command's cost grows with
the account's open orders, since every check counts what each working order reserves
and could add: working orders on one contract share their margin and valuation
lookups, so a ladder of hundreds of resting bids costs a few milliseconds a command
(`BM_TradingSubmitWorking` in `bench/bench_trading.cpp` measures it). A script that
keeps many orders working should send them at a pace the inbox can drain, and retry
503s after `Retry-After` with the same client IDs.

Every referenced listed contract is registered before its first quote batch. Held
positions, open orders and pending-command symbols receive quotes from ChainBook
and valuations from the latest coherent analytics frame at the strike smile IV.
Sizes are floored to whole contracts. Observation numbers increase only for new
OptionQuote events and resume above recovered high-water marks. Cached analytics,
underlying prints and HTTP reads never replenish displayed option liquidity.
**The market queue coalesces a contract’s quotes within one drain, so a fleeting
cross can be missed on streaming feeds.** Underlying prints are retained in order
so settlement uses the first qualifying print.

Each market batch and command publishes an immutable trading view per account. GET
endpoints read that view without touching the reducer. `/api/status` and WebSocket
ticks include the main account's `trading: {enabled, reason, account_version,
kill_latched, write, fee_per_contract, initial_cash}` and `accounts: [{id, name,
trading}]` for every account. The fee and original session cash are exact money
strings (defaults `"0.65"` and `"100000.00"`), taken from the active/recovered session
configuration; `initial_cash` is not the current balance or daily equity baseline.
Versions are decimal strings and `write` is `open`, `token`, or `disabled` (see
[write protection](#write-protection)). Clients refetch
portfolio, orders and risk when the version changes. Chain option objects include
canonical padded `symbol`, whole `bid_size`/`ask_size` (null when unavailable),
`tradable` and `untradable_reason`, using the core eligibility policy, and
`executable`: whether paper orders can fill on the displayed quote, a positive,
uncrossed bid and ask with at least one whole contract on each side. When it is false,
`quote_issue` says why (`no_quote`, `no_bid`, `no_ask`, `crossed` or `zero_size`); a
contract can be tradable with a quote that is not executable.

Each `/api/status` and tick `underlyings[]` entry includes
`paper: {accepting: boolean, reason: code|null, message: string|null, session}`, where
`session` is the session new orders enter by the underlying's market-data clock
(`regular`, `global`, `curb` or `closed`; null before any data). This uses the
same session/feed check as new orders, the underlying's market time (including
persisted quotes after recovery), provider delay and the active session's
`max_quote_age`. An accepting entry has null reason and
message; missing market data reports `INVALID_QUOTE`. Contract eligibility, risk,
kill-switch and write-access checks still apply separately. The existing `session`
field continues to describe the wall-clock product session; just after 09:30 a
15-minute delayed feed still trades the overnight session, which `paper.session` shows.

Both `/api/status` and WebSocket ticks also include a top-level `circuit_breaker`
object, shared by all accounts and available even with paper trading disabled:

| Field | Meaning |
| --- | --- |
| `symbol` | The watched symbol: `SPX` if subscribed, otherwise `SPY`. No reference is invented when its data is unavailable. |
| `day` | Current trading date as `YYYY-MM-DD` on the market-data clock; null before any data or recovery. As with the paper accounts, the next trading date starts after 17:00 ET. |
| `previous_close` | `{date, price}` for the previous business day, or null when unavailable. The date is `YYYY-MM-DD`; the price is a number. An official close takes precedence over a recorded closing print. |
| `level` | Highest level tripped on `day`, from 0 (none) through 3. |
| `inactive_reason` | `MISSING_PREVIOUS_CLOSE` when the price-triggered breaker has no reference, otherwise null; optional on older servers. Authored halts work independently. |
| `halts` | Halts of the most recent day that tripped one, in start order. Each has `level`, `start`, `end`, `reference`, `price` and `active`. The two prices record the previous close and the print that tripped that halt. An authored scenario halt has level 0 and both prices 0; levels 1–3 retain the real circuit-breaker thresholds. |
| `active` | Whether any halt covers the latest market time, including its start and excluding its end. Each halt's flag uses the same clock. |
| `market_time` | Latest observed market time, or recovered time at startup; null before either is available. This and halt timestamps use the API's UTC ISO timestamp format. |
| `error` | Last breaker storage failure for this engine instance, or null. A later successful write does not erase the warning. |

The engine thread publishes this snapshot under the status mutex after each market
batch. The active flag follows delayed or replayed market data, so elapsed wall time
alone does not end the banner. A connected tick replaces the terminal's breaker
state; REST supplies it before the first tick and while disconnected. Older servers
without the object show no banner.

The Brief page shows the selected account's plan, attempt, floor and effective soft
floor, closest recorded floor approach, guardrails, pending changes and open holdings
and orders. Its loss allowance is the smallest of daily-loss limit minus current daily
loss, equity minus the soft floor, and equity minus the plan floor. Disabled limits
are excluded, exhausted allowance is zero, and incomplete or mixed-version valuations
leave it unavailable. These are exact decimal-string calculations for display; the
server still enforces all trading rules. Position deadlines use the contract's last
trade and the selected plan's cutoff, including AM settlement on the following day.

Brief edits the trading day's plan through the regular session and prompts for review
after the close. Session rows identify early closes; without those rows the prompt
uses 16:00 ET. The next engine trading date starts a new plan. Both fields use the
existing day-note route, preserving the other field. Drafts stay local until saved and
are cleared on account, source or date changes. Missing note reads cannot be overwritten
with an empty note. Read-only runs keep the note visible with saving disabled.

The Journal page edits each trade's note and tags (a strategy's apply to each of its
legs), filters every panel by tag and reports P&L by tag. Browser alert settings
for price levels on an underlying and each new fill are kept in
the browser's local storage and run while the terminal is open, on the live feed only
(not a replay). They show on the page and, where the browser allows notifications, as
system notifications, with an optional chime. A price alert fires once, when the
feed's price reaches its level from the side it was set on. Rule alerts use the same
notifications and chime: floor room below 50%, 25% and 10% of the plan drawdown;
daily loss at 50%, 75% and 90% of its limit; a guardrail latch; pending rules taking
effect; and a target within 10%. Each fires once per account and trading
day, including after a page reload. Claims are stored in localStorage with an
in-memory fallback when browser storage is denied.

External notifications run on the server while the browser is closed. Live account
publications supply new fills, rejected orders, kill and daily-loss trips, assignments,
exercises and staged playbooks. The engine also detects stalled feeds without an API
reader. Each channel can warn when `breach.room` is at or below its dollar distance;
missing room or incomplete account valuation supplies no price estimate. Channels
rearm when room moves above the distance. Recovered fills and deliveries are history
and are not sent again. Demo, replay and interactive drill forwarding requires the
operator's `include_simulated` opt-in. Catch-up history, verification, sandbox
servers and backtests never send.

Automatic share deliveries with negative recorded buying power also emit `alert`,
including the exact `buying_power_after` and option `symbol` in details. This follows
the configured alert channel filter and fires once for each new delivery.

The observer and delivery queue do not change reducer inputs or journal records.
Messages label trading as paper/simulated and use the event's market time. Delivery
timing uses a separate clock. The Notifications section in Alerts settings shows
channels, editable filters and a test button; destinations and credentials stay on
the server. See [external notifications](runtime.md#external-notifications) for setup,
delivery guarantees and limits.

The web ticket uses the preview’s `fee` and itemized `fees`, falling back to
`fee_per_contract` on flat-fee accounts; only older servers without that setting
expose a manual fee estimate. Ticket and Positions notices use `paper.message`,
and `paper.accepting: false` disables ticket submission. In the overnight and curb
sessions (by `paper.session`) the tickets offer limit entries, with a condition or
bracket on EXTO/GTC_EXTO to work there, or GTC/GTD to wait for regular hours.
EXTO/GTC_EXTO also allow simulator-managed stop-market orders at the touch.
GTD exposes a UTC date-time input. The flatten dialog offers Market or Limit with
a bounded tick offset; Limit enables Close all in overnight and curb. The
flatten dialog names an underlying whose paper orders are refused (a stalled feed, a
halt), whose positions and orders the flatten leaves, and does not count its orders
among those cancelled; Close all is disabled when that is every underlying in scope.
After a flatten, its dialog lists each closing order's outcome (a close still
working says so), the shares it traded and any it left with the reason, and
each position still open with the contracts being worked and why the rest stay. The
dialog counts the orders it cancels without the bracket exits, which stay until the
position they protect is flat. For
older servers without `paper`, they fall back to the session-based notice and
submission gate.
Limit prices display cents, with buttons and arrow keys following the root's tier
tick table above (including downward steps across $3.00). Typed off-tick prices
still receive the server's `INVALID_TICK` reason. On wide screens the ticket docks
beside the chain; elsewhere it is a dialog. It names the strategy from the held
position (Long Call, Close Short Put...), sets the limit from Bid/Mid/Ask, says whether
the order is marketable at the far side or will rest, and requests buying power and
floor risk from the server preview. The web has no second copy of margin rules.
Quoted premium estimates exclude slippage; the server preview includes the plan's
slippage. Buy-only plans block opening sells; decided attempts allow closing orders and block opening ones with the reason.
Both tickets offer **Join trade** when their underlying has an open whole trade,
labelled by its held contracts and trade ID. **None** is the default; choosing a
trade sends its `group` with the preview and the order. Joining at submission keeps
the whole trade's earlier review, including its extremes, as described above.

Strategy mode's **Templates** menu selects verticals, iron condors, iron butterflies,
strangles, straddles, long call or put butterflies, calendars and diagonals. Delta
selection uses the chain's per-strike deltas. Forward offsets use points or the
expiry's expected move, F × ATM IV × √T. Positive offsets go out of the money;
condors use both sides. Verticals target the short leg for credit and debit spreads.
Calendars and diagonals sell the selected near expiry and buy the chosen far expiry,
whose full chain must load first. A diagonal's signed offset sets its far strike
relative to the near strike. Calendars require the same listed strike in both chains.

Targets snap to listed strikes in the loaded chain, with ties going to the lower
strike. Targets outside that range, missing quotes or deltas, overlapping legs and
unequal butterfly wings give a reason instead of an order. Widen the chain window
when needed. Review shows the legs, actual widths, net credit or debit, max loss and
probability of profit before submission. Calendars and diagonals retain the ticket's
estimated first-expiry risk, valuing later legs at today's IV. Named presets are
stored per underlying in this browser; the selected near expiry remains the near
expiry when a preset is reused. Each template exposes a parameter tag, displayed in
the ticket and cleared on manual leg edits. It goes with the order as a tag, so the
trade it opens carries it into the Journal's reports by tag.

Single-leg and strategy tickets show risk-neutral probability of profit before fees,
using the distribution of the underlying at the order's first expiry. They prefer
that expiry's raw SVI fit when its butterfly and calendar checks pass, then its SSVI
slice, then the chain's smile. The digital probability is the strike derivative of
the smile-priced call, N(d2) − φ(d2)√T ∂σ/∂ln(K/F). SVI and SSVI use analytic
volatility derivatives. Chain IV is interpolated in log moneyness; skew comes from a
local least-squares fit over up to five neighbouring strikes. Without a smile the
ticket labels the flat-IV estimate “lognormal, no smile”. No usable volatility means
no probability estimate.

Outside the fitted or quoted range, volatility stays flat and skew is zero. A fixed
log-strike grid, including quoted knots and both sides of the range endpoints, clamps
the digital to [0, 1] and takes its running minimum. Interpolation of that grid gives
non-increasing probabilities at every strike; flat smiles retain N(d2) exactly.
Any repair is labelled approximate, and breakevens outside the range disclose flat
volatility. These are model probabilities, not forecasts of realised returns.

After HTTP 400/403/404/409/422, the ticket shows the rejection details and **New order**,
which preserves form values and starts a fresh client ID. **Retry same order** keeps
the frozen request and ID only after a network failure, timeout or 503. Other
unexpected responses direct the user to check Positions and Orders. Results receive keyboard
focus at the top of the ticket.

| Endpoint | Request / response |
| --- | --- |
| `POST /api/positions/exercise` | Canonical `symbol` and positive `quantity` of long equity or ETF contracts to exercise early; returns the portfolio |
| `POST /api/positions/abandon` | Canonical `symbol` of a long nobody bids for, or one awaiting settlement, to give up at zero without a fee ([disposal](#disposing-of-worthless-positions)); returns the portfolio |
| `POST /api/positions/instruction` | Canonical `symbol` of a long option and boolean `do_not_exercise`: true makes it expire worthless at settlement, false withdraws that; returns the portfolio, whose positions carry `do_not_exercise` and `no_bid` |
| `POST /api/stocks/trade` | `symbol` (`SPY`), `side` (`buy` or `sell`), and integer `shares` (1–10,000,000); opens, adds, reduces or reverses at a fresh stock-session price, returns the portfolio. Not idempotent: inspect positions after an uncertain response before retrying |
| `POST /api/stocks/trade/preview` | The same body; dry-run decision (`ok` or reason code), `reason`, `price`, signed `cost` (negative receives), `current` and `after` account buying power, exposure and risk; `simulated: true`. Refusals leave projected holdings unchanged; nothing recorded |
| `POST /api/stocks/orders` | A working share order: `client_order_id`, `symbol`, `side`, `type` (`market`/`limit`/`stop`/`stop_limit`), integer `shares`, `time_in_force` (`day`/`gtc`, default day), `limit_price`, `stop_price`, `extended_hours` (limits only), `bracket` `{stop_loss?, take_profit?}` and an `oco` partner without a symbol or client ID of its own ([working share orders](#working-share-orders)); 201 returns `stock_order`, `related_orders` (partner and exits) and the `stock_fills` it made, 200 for a retry |
| `POST /api/stocks/orders/preview` | The same body; the `StockPreview` dry run, with `cost` at the limit or stop (the price for market orders) |
| `GET /api/stocks/orders` | Share orders, newest first; `status=open` restricts to working and armed ones. `GET /api/portfolio` lists the open ones as `stock_orders` |
| `DELETE /api/stocks/orders/{id}` | Cancel an open share order: 404 `UNKNOWN_ORDER`, 409 `ORDER_TERMINAL`, 422 `STOP_REQUIRED` |
| `POST /api/stocks/close` | `symbol` of shares held (`SPY`) and optional positive `shares`, all of them when left out; closes at the underlying's price in the regular session and returns the portfolio |
| `GET /api/portfolio` | `time`: the market time the publication is as of, the feed's latest even while the account is idle (as in `GET /api/risk` and `GET /api/account`). Account cash, equity, daily baseline/P&L, realised/unrealised, fees, completeness/quality flags, marked positions and Greeks, opened or delivered `stocks` (symbol, shares, average price, basis, mark and its time, market value, unrealised and realised P&L, fees, freshness and today's attribution), and today's `attribution` (`delta`, `gamma`, `vega`, `theta`, `other`, `costs`, `total` in dollars) for the account and each position (null until the position's next fill or rollover), and `liquidity_used`: the current quotes whose displayed size the account's orders have taken some of, with each side's size and what is left (`bid_left`, `ask_left`) until a new quote |
| `GET /api/orders?status=all` | All orders, newest first; `status=open` restricts to working, partially filled and armed orders |
| `POST /api/orders` | `client_order_id`, canonical `symbol`, `side` (`buy`/`sell`), `type` (`limit`/`market`), integer `quantity`, decimal-string `limit_price` for limits, `time_in_force` (`day`/`gtc`/`ioc`/`exto`/`gtc_exto`/`gtd`), `good_till` timestamp required only for GTD, optional `tags`, `note` and `walk` (see [walking limits](#walking-limits-f46)), optional `trigger` following the OpenAPI `Trigger` schema and the [conditional-orders](#conditional-and-bracket-orders) rules: `source` is `option`, `combo`, `underlying`, `study` or `time`; non-time triggers take `direction` and `level`; underlying and study triggers may name another underlying with `symbol`; study triggers add `study` (`iv30`, `iv7` or `term_ratio`); time triggers take `at` as `HH:MM` New York time; option and combo triggers may set `reference` (`bid_ask`, `mid` or `mark`); option, combo and own-underlying triggers may set `trail` (`amount`, `percent`, or `ticks` for option/combo only). `bracket` is `{stop_loss?, take_profit?}` and each exit takes a `trigger`, a `limit_price`, or both for a stop-limit. A multi-leg order replaces `symbol` and `side` with `legs` (two to four `{symbol, side, ratio?}`, ratio default 1, or up to eight for a roll), allows an entry bracket or a reducing trigger (combo or underlying), counts units in `quantity` and sets a signed net `limit_price` (negative for a credit); `exits_only: true` attaches a bracket to a held contract (with `symbol` and the closing `side`) or to held closing legs as described above; optional `group` joins what the order opens to a [whole trade](#whole-trades); optional `then` and `oco` chain further orders (see [order chains](#order-chains-one-cancels-other-and-one-triggers-other)); 201 returns version, order and its fills. Orders report `legs` (null for single-leg), with null `symbol` and `side` for multi-leg orders. Retrying with a `client_order_id` already used and the same terms is safe: it returns the first answer (200 with the order as it now stands, also after `PUT /api/orders/{id}` changed it, or the original rejection) and records nothing, while other terms under that ID, the changed ones included, are refused with 409 `DUPLICATE_CLIENT_ID` and record nothing. Client IDs are scoped to an attempt: after an account reset, earlier attempts' IDs name new orders |
| `POST /api/orders/preview` | The order body plus optional `floor_share` (default 0.5); 200 returns the dry-run decision, buying power, exposure change, labelled maximum loss, floor warnings, `max_units` with its buying-power and floor parts (null when unavailable), projected `breach`, and `execution`: what submitting now would fill at once and the full size's fill schedule, each leg's quote `liquidity`, and `warnings` about stops, targets and triggers already reached, a stop given only a limit price, or slippage that pushes a market order outside the band |
| `POST /api/orders/what-if` | `candidates`: one to six, each an optional `name` and one to four `orders` as submission takes them (client ID optional; `then`/`oco` use current-quote submission and contingent-child rules); 200 returns the account `current` and each candidate's `decision`, `reason`, per-order `orders` checks and the account `after` its orders fill in full (null when one cannot be projected): equity, buying power, exposure, grid max loss, floor flags, scenarios and breach ([what-if](#what-if)) |
| `GET /api/account/equity?from=&to=` | Persisted equity samples with optional inclusive UTC ISO bounds, plus any storage error |
| `DELETE /api/orders/{id}` | No body; 200 returns version and resulting order |
| `PUT /api/orders/{id}` | Any of integer `quantity`, decimal-string `limit_price` and `trigger_level`, DAY/GTC `time_in_force`, and optional `walk` (null removes it); 200 returns version, the changed order and its fills (see [changing orders](#changing-cancelling-and-flattening)) |
| `POST /api/orders/cancel` | Optional `underlying`, or `orders` (one to sixteen distinct order IDs, not with `underlying`); cancels every open order, that underlying's, or those listed that are still open, together in one transaction, and returns version and `cancelled_orders`. An unknown listed ID is 404 `UNKNOWN_ORDER` and a list with none open 409 `ORDER_TERMINAL`; neither cancels anything |
| `POST /api/positions/close` | Optional `underlying`; cancels the open orders in scope but the bracket exits and closes its positions at market with reduce-only orders that keep working until filled, returning version, `cancelled_orders`, the closing `orders` (each with its status and reason), their `fills`, the shares it closed (`stock_fills`) and those it could not (`kept_stocks`: symbol, shares and reason), and each position still open (`residuals`: symbol, underlying, signed `quantity`, the contracts still `working` and the `reason` the rest are not, or null). 422 with the reason, and nothing changed, when nothing in scope can close ([flattening](#changing-cancelling-and-flattening)) |
| `POST /api/positions/close/preview` | Optional `underlying`; the flatten's dry run on a private copy: `decision` and `reason`, `cancelled_orders`, the closing `orders` without IDs and their `fills`, `stock_fills`, `kept_stocks`, `remaining` and `remaining_shares` in scope, and the account `current` and `after`; `simulated: true`, nothing recorded ([flattening](#changing-cancelling-and-flattening)) |
| `GET /api/fills` | Version and fills, newest first, with pre-execution `context` and the `quote` each took: `observation`, `bid`, `ask`, `bid_size`, `ask_size`, `size_left` (displayed size still free for paper orders before the fill), `quoted_at` (when the quote was first given) and `age_seconds` (both null on older fills), `feed`, the feed whose quote priced it when a fallback is configured (null otherwise), `stress_ticks` (extra adverse ticks stop stress added) and `queued` (the queue model filled a resting limit at its limit) |
| `GET /api/trades.csv`, `GET /api/fills.csv` | CSV downloads with `account`, inclusive New York `from`/`to` dates, fixed columns and exact money; see [CSV downloads](#csv-downloads) |
| `PUT /api/days/{YYYY-MM-DD}/note` | Required `plan` and `review` strings replace the day note; returns version, `day` and `note`. Invalid text returns `INVALID_NOTE` (422); invalid dates return 400 |
| `GET /api/risk` | Version, active/pending limits and guardrails, guardrail progress, pending activation, daily loss, kill state, aggregate/underlying buckets, scenario matrices and `breach` |
| `PUT /api/risk/limits` | `expected_revision` string and complete `limits` object; tighter fields apply now, looser evaluation fields are pending until rollover; 200 returns the risk view, 409 `LIMITS_REVISION` if the revision changed (refetch it and retry) |
| `PUT /api/risk/guardrails` | `expected_revision` string and complete `guardrails`; tighter fields apply now, looser fields wait for rollover on all accounts; returns the risk view, or 409 `LIMITS_REVISION` as for limits |
| `POST /api/risk/kill` | `action` (`trip`/`reset`) and nonblank `reason`; returns version, kill state and cancelled order IDs. The kill state here and in `GET /api/risk` is `{latched, reason, reset_blocked, history}`: why the kill-switch reset could not clear the latch now (a decision, or null; account reset follows the separate guardrail persistence rule) and its last 50 trips, resets and releases (`{time, action, reason, previous, actor}`) |
| `GET /api/settlements` | Settlement references, cash, gross realised P&L, fee and nullable provenance, newest first across attempts; read scope |
| `POST /api/settlements` | Canonical `symbol` and decimal-string `value` for an expired AM position, or a PM one whose closing print never arrived (its `settle_by` is `manual`); returns version and `position_closed` |
| `GET /api/account` | Rules (including `phase`, `lock_balance`, `lock_at_start`, `profit_basis`, the daily loss limit, consistency, minimum days, `day_end` and `payouts`), personal guardrails and progress, `breach`, evaluation (attempt, status, starting balance, equity, `balance`, `scaling` (null or limit/held/closed-balance profit/next step), `size_scaling` (null or current/original/max/next size, periodic review progress and history), `marked`, profit, peak, floor, `floor_locked`, drawdown buffer, target equity/remaining (`0.00` once passed, though liquidating at the bid can leave equity just below the target; on the balance basis, measured on the balance), decision and `decision_code`, current day, finished `days[]` with `realised`, `qualifying`, `attribution`, equity low/high with times, `profit`, `profitable`, `executions` and `locked`, attempt closest-floor distance/time, `qualifying_days`, `cycle_started`, `payouts[]`, `objectives[]` (code, met, actual, required, message), `trading_days`, `profitable_days`, `best_day`, `best_trade`, `closed_trades`, `short_profit`, `consistency_target`, `daily_loss` (limit, basis, action, reference, level, room), `day_lock` and `day_locked_at`, `exit_equity` and `exit_cost`, and once a decided attempt is flat `liquidated_equity` and `liquidation_cost`), buying power, `payout` (the next payout's standing from `payout_quote`: `eligible`, `blocked`, number, flat/active, qualifying and required days, profit, withdrawable, cap, maximum, minimum, trader share and percentages, `consistency_percent`, `cycle_profit`, `best_day`, `consistency_needed` and `buffer_balance`; null outside the funded phase) and earlier `attempts[]` (including phase-bearing rules and starting fees); `next_plans` lists the current pass's unlocked preset IDs, and `costs` reports lifetime fees, reset allowance, trader payouts and net; absent rules give null floor/target |
| `GET /api/trades?status=open\|closed\|all&attempt=current\|all` | Round trips, newest first: direction, status, opened/closed/duration, quantities, average open/close, cost (entry premium), gross, fees, net, `return` (net / cost, closed only), mark/unrealised while open, `closure` (`settlement` at expiry, `exercise` for an early exercise, `assignment` for an early assignment, `abandon` for an abandoned long, `reset`, or null when fills closed it or it is open), fill IDs, attempt, and the trader's `note` (`""` for none) and `tags`, `entry_context`, `exit_context`, `review`, `strategy_id`, `strategy_review`, the whole trade it is in, `group`, and `buying_power`, `return_on_buying_power`, `strategy_buying_power` and `strategy_return_on_buying_power` (see [trade review](#trade-review)); `groups` lists the [whole trades](#whole-trades) with more than one entry. `day_notes` holds the account's daily plans and reviews. Defaults: all statuses of the current attempt. `stock_fills` lists every change in shares (`id`, `symbol`, signed `shares`, `price`, `time`, `source`, `option`, `buying_power_after`) and `dividends` every dividend paid (`symbol`, `ex_date`, `per_share`, signed `shares`, `amount`, `time`), oldest first, which the terminal announces when new. `buying_power_after` is the exact buying power immediately after automatic delivery on plans enforcing buying power; it is null for older records, other share fills and plans without that check. `share_trades` lists the shares' round trips the same way (`kind: "shares"`, `id` `s` + the opening stock fill, shares instead of contracts, no fees), with `opened_by`/`closed_by` (`expiry_exercise`, `assignment`, `early_exercise`, `trade`, `rule` or `reset`) and the `option`/`closing_option` that delivered them |
| `POST /api/trades/group`, `POST /api/trades/ungroup` | `trades`, round trips by trade ID: join their trades into one (a closed round trip can name a whole trade still holding an open one), or take each listed open round trip out of its trade (see [whole trades](#whole-trades)). Returns version and `groups`, the trade each named round trip is in now; `UNKNOWN_TRADE` (404), `INVALID_GROUP` (422) |
| `GET /api/alerts` | Version and the account's `alerts`, oldest first: `id`, `label`, `scope`, `metric`, `symbol`, `legs`, `direction`, `level`, `repeat`, `created_at`, `actor`, `armed` (waiting for its condition), `fired` (times), `fired_at` and `value` (when and at what it last fired) |
| `POST /api/alerts` | `scope`, `metric`, `direction` and decimal-string `level`, with `symbol` or `legs` as the scope needs and optional `label` (100 bytes) and `repeat`; 201 returns version and the alert (see [account alerts](#account-alerts)). Malformed terms are 400; an unregistered contract `UNKNOWN_CONTRACT` (404); a 101st alert `INVALID_ALERT` (422) |
| `DELETE /api/alerts/{id}` | No body; returns version and `deleted`, or `UNKNOWN_ALERT` (404) |
| `PUT /api/trades/{id}/note` | Optional `note` string and `tags` array replace the trade's (see [trade notes](#trade-notes-and-tags)); an empty note with no tags clears them. The `id` is a trade's, or a share trade's (`s` and its opening stock fill). Returns version, `trade`, `note` and `tags`; `UNKNOWN_TRADE` (404) if no trade opens with that fill, `INVALID_NOTE` (422) for text past the limits |
| `GET /api/plans` | Presets: `practice` (buying power only), `intraday-25k/50k/100k` (buy-only, 10% target, 5% intraday trailing), `eod-25k/50k/100k` (any side, 12% target, 6% end-of-day trailing), their `funded-*` accounts (`unlocked_by` names the evaluation), `static-25k/50k/100k` and `locking-25k/50k/100k` (see [plan presets with objectives](#plan-presets-with-objectives)); `two-step-*`, `two-step-verify-*` and `two-step-funded-*` at the same sizes (see [two-step programs](#two-step-programs-f27)). Each includes `phase`, `unlocked_by` and `unlocks`. Evaluation, verification and funded presets auto-close five minutes before the last trade (15:55 ET for SPXW, 16:10 for SPY) |
| `POST /api/account/reset` | Nonblank `reason` plus either a preset `plan` ID, or `initial_cash` and complete `rules` (optional `phase`, `lock_balance`, and `payouts` required exactly when funded); returns the new account view. Locked steps need a current passed matching prerequisite (`PLAN_LOCKED`); same-plan restarts charge `reset_fee` and obey `max_resets` (`RESET_LIMIT`, 409) |
| `POST /api/account/cash-flows` | `client_id`, signed whole-cent decimal `amount`; practice only; returns Account with `cash_flows` history. Same ID/amount is idempotent |
| `POST /api/replay/market/halt` | `symbol`, integer `minutes` (1–390); simulated stock/ETF halt at current replay time, capped at regular close; replay/admin scope |
| `POST /api/account/payout` | Decimal-string `amount` in whole cents; returns the account view with the recorded payout |
| `GET /api/accounts` | `accounts`: each account's `id`, `name`, `trading` status and `equity`, the main one first |
| `PATCH /api/accounts/{id}`, `DELETE /api/accounts/{id}` | Admin: rename/archive/unarchive or delete a named account; see [accounts](#accounts) |
| `POST /api/accounts` | `name` and a preset `plan`, or `initial_cash` and custom evaluation/verification `rules`; custom funded rules are refused because funded accounts start from an account that passed its evaluation. 201 returns the new account's `id`, `name`, version, plan and equity (see [accounts](#accounts)) |

Sandbox tokens default to their own account on all account reads and writes when
`account` is omitted. Explicit `?account=` is still checked and cannot select main
or another sandbox. This default applies only to issued sandbox tokens; named
`trade:ACCOUNT` tokens keep main as their default. The terminal and Python
`Client(url, token)` therefore work without specifying the sandbox ID. MCP account
tools also accept an omitted account; writes allow omission only with a sandbox
token. Other MCP writes still require an account, and replay controls name `main`.

Every route in this table except `/api/plans` and `/api/accounts` takes `account=ID`
in its query for an account other than the main one (see [accounts](#accounts)).
`/api/replay` and the routes under it serve a replay of a recording or the simulated
demo market; see [replaying in the terminal](runtime.md#replaying-in-the-terminal).

Rules JSON is `{plan, profit_target, max_drawdown, drawdown_mode, buy_only,
defined_risk, slippage_ticks, fill_latency_ms, impact_ticks, inside_fill_percent, fees, margin, account_type, house_margin_percent,
pm_vol_shock, buying_power, expiry_cutoff_seconds, lock_at_start, profit_basis, daily_loss_limit,
daily_loss_basis, daily_loss_action, consistency_percent, consistency_basis, min_trading_days,
trade_consistency_percent, min_trades, min_hold_seconds, microscalp_seconds, microscalp_percent,
min_profitable_days, profitable_day_profit, day_end, max_contracts_held, require_stop_loss,
max_trade_risk, max_trade_risk_percent, time_limit_days, inactivity_days, flat_time, no_overnight,
underlyings, trading_start, trading_end, scaling, size_scaling, events, news_before_minutes, news_after_minutes,
news_action, hold_restrictions, hold_cutoff, no_hedging, no_counter_positions, max_volume_percent}`.
`fees` is the optional [fee schedule](#fees). `scaling` defaults to `[]` and uses decimal-string profit thresholds; see [Scaling plan](#funded-accounts-and-payouts).
`defined_risk`, `slippage_ticks`, `fill_latency_ms`, `impact_ticks`, `inside_fill_percent`, `margin`, `account_type`,
`house_margin_percent`, `pm_vol_shock` and every field from `lock_at_start` on are optional when creating or
resetting an account: `defined_risk` and `lock_at_start` default to false, the execution settings, house margin,
vol shock, counts and percentages to 0, `margin` to `"strategy"`, `account_type` to `"margin"`, `profit_basis` to
`"equity"`, `daily_loss_basis` to `"equity"`, `daily_loss_action` to `"lock"`, `consistency_basis` to `"total"` and
`day_end` to `"17:00"`. Older journals missing these fields recover with the same defaults. Money is null for a
disabled target, drawdown, daily loss limit or profitable-day profit; `drawdown_mode` is `intraday`, `end_of_day` or
`static`; `profit_basis` `equity` or `balance`; `daily_loss_basis` `equity`, `balance`, `higher` or `peak`;
`daily_loss_action` `lock` or `fail`; `consistency_basis` `total` or `positive_days`; and `day_end` `HH:MM`
New York time from `16:15` to `24:00`. Portfolio adds
`buying_power: {available, reserved, short_requirement, requirement}` (`requirement`
is the same amount as `short_requirement`, named for both margin modes) and `margin`,
the requirement by underlying with what holds it (see buying power under Account rules); orders add `origin`
(`user` or `system`), `tags`, `note`, `exits_only` (false when absent in older journals), `status` `armed`, `trigger`, `triggered_at`, `bracket`, `role`
(`stop_loss`/`take_profit`/null), `parent`, `oco`, `stop_loss_order`,
`take_profit_order`, `ended_at`, `modified_at`, `changes` and `waiting` (see below); status and ticks add `trading.plan` and `trading.evaluation`
(`active`/`passed`/`failed`, null without a target or drawdown rule). `--plan ID`
chooses the rules for a new journal (default `practice`); `--paper-cash` then overrides
its starting balance. Recovery keeps the recorded rules. New latency, impact and inside-fill fields default to zero
when absent and are omitted from account responses and journals at zero. Pending
orders recover their acceptance/trigger clocks and consumed depth.
`POST /api/account/reset` and `POST /api/accounts` accept optional
`fill_model: "as_displayed" | "conservative" | "midpoint"` beside the plan or custom rules. It
overrides only latency, impact, slippage and inside fills for that account's new attempt. Optional
`fee_model: "flat" | "itemized"` chooses its [fees](#fees). They also accept `margin`,
`account_type`, `house_margin_percent` and `pm_vol_shock` beside the plan or custom
rules, overriding the account's margin the same way (the attempt still counts as the
plan's own). Invalid rule values return HTTP 400 `INVALID_RULES`. Journals omit the
account type, house margin and vol shock at their defaults, so existing journals keep
their bytes.

Money is an exact decimal string, quantities are integers, IDs/versions are strings,
and timestamps use the same UTC ISO format as `as_of`. Analytical values may be
null. Position Greeks expose per-unit delta/gamma/vega/theta plus signed position
dollar exposures. Incomplete scenario grids contain null P&Ls, never partial sums.
Limits contain `max_order_contracts`, `price_band_absolute`, `price_band_relative`,
`aggregate` and `per_underlying` (`dollar_delta`, `vega`), optional
`underlying_overrides: {"SPX": {"dollar_delta": 500000, "vega": 5000}}`, `max_daily_loss`,
`max_quote_age_seconds` and `max_valuation_age_seconds`.

Unknown fields, duplicate JSON keys, missing required fields, wrong types and
noncanonical OSIs return 400 `INVALID_REQUEST`. So does an order no market could make
valid, whichever field breaks it: an empty, overlong or control-character client ID,
a quantity below one, an unconditional market order that is not IOC, a single contract's limit that
is not positive, fewer than two or more than eight legs, two legs naming one contract,
or a ratio outside 1 to 10. Nothing is recorded and the client ID stays free. Business
rejections, which depend on the account and the market, return 422 and remain recorded
as rejected orders; unknown contracts/orders return 404,
terminal orders and a client ID this attempt already used with other terms return 409
(nothing recorded); an identical
retry returns 200 with the order as it now stands. Errors always have this shape:

```json
{"error":{"code":"DELTA_LIMIT","message":"...","actual":1250000,"limit":1000000,"scope":"SPX"}}
```

Unused `actual`, `limit` and `scope` are null. `scope` names an underlying or
`aggregate`: a check on one contract, such as `PRICE_BAND`, reports that contract's
underlying, while the journaled decision keeps the contract's OSI symbol, as the
order itself does. Rejected writes still consume their client ID for the attempt; GET
orders shows their resulting rejection reason, and `GET /api/orders?client_order_id=ID`
lists the orders that used an ID, newest first: one per attempt that used it. An order's `reason` has the same shape as an error,
`{code, message, actual, limit, scope}`: a `RISK_CHANGED` cancel keeps the original
check's code at the start of its message and its numbers (3 contracts against a new
limit of 2, a projected loss of 15.65 against 15.64), and so does a Flatten leg the
rules refused. The preview's `reason`, a payout's `blocked` and a flatten's
`kept_stocks[].reason` use it too. The Orders page and the flatten dialog show the
numbers beside the reason.

Orders also report `ended_at` (null while open), `modified_at` (when a change last
applied, or null), `changes` (each with `time`, `actor`, the requested `quantity`,
`limit_price` and `trigger_level`, null where kept, the `previous` terms, `applied`
and the refusal's `reason`) and, while open, `waiting` (`{code, message}` from the
table under [changing orders](#changing-cancelling-and-flattening), or null). The
Orders page's **Details** shows an order's history: accepted, triggered, each change
and refusal, and how it ended or what it waits for.

### Write protection

Every request, read or write, HTTP or WebSocket, must name the server in Host by an IP
address, `localhost` or a name under `.localhost`, the host of an `--allowed-origin`, or
a name given with `--allowed-host`; others get 403 `HOST_REJECTED`. DNS rebinding points
a name its attacker controls at the server, which makes that name's pages same-origin
with it, and that name is what their requests carry in Host.

Every POST/PUT/DELETE under `/api/` uses the same protection. POST and PUT require
`Content-Type: application/json` (an optional media-type parameter is accepted);
DELETE has no body. The body limit remains 64 KiB, except 4 MiB for
`POST /api/analysis/portfolios`, whose broker exports run larger. A present Origin must match Host
or an exact `--allowed-origin`; invalid or ambiguous security headers fail closed
with 403 `ORIGIN_REJECTED`. Non-browser clients may omit Origin.

`--write-token TOKEN` overrides `OPENPORT_WRITE_TOKEN`. When configured, all writes
require `Authorization: Bearer TOKEN`, checked with a constant-time digest comparison;
missing/incorrect credentials return 403 `WRITE_TOKEN_REQUIRED`. Without either,
`--write-token-file PATH` uses the token kept in PATH, creating a random one with
owner-only permissions when it is missing, and prints a link carrying it,
percent-encoded, `http://localhost:PORT/#token=TOKEN`, which the terminal saves in that browser tab and
drops from the address bar, also when opened in a tab already showing the terminal;
the Docker image keeps its token in its volume this way.
The terminal's "Enter write token" button saves a token by hand.
Without a token, only loopback binds allow writes. Non-loopback binds return 403
`WRITE_DISABLED`.

Status reports the result as `write`: `open` when a write without credentials is
accepted (a loopback bind with no legacy token, `--require-token` or `--sandboxes`;
named tokens alone leave it open), `token` when writes need a token, and `disabled`
when no credential can write (a non-loopback bind without tokens). A token sent to an
`open` server is still checked and names the actor.
Named tokens come from `--token-file FILE`, one `NAME SCOPES SECRET` per line.
Scopes are comma-separated. Blank lines are ignored and `#` starts a comment.
Malformed lines, duplicate names, secrets or scopes, unknown scopes and reserved
actor names fail startup. Diagnostics name the line, never its secret. The file reloads
when its mtime/size changes (checked on authenticated requests and once per second),
on `SIGHUP`, or with admin `POST /api/tokens/reload` and `{}`. The admin reply is
`{names: [...], count, loaded_at}` and never includes secrets. Without a configured
file the route returns 409 `TOKENS_UNCONFIGURED`. Invalid or unreadable replacements
return 400 `TOKEN_FILE_INVALID` on explicit reload and log a safe diagnostic;
the complete previous set remains active. A malformed line names only its line
number. Replace the file atomically to avoid exposing intermediate contents.
An empty file revokes every named token. Startup still requires a nonempty valid
file. `--require-token` and write policy never become open because of a reload.

Removing a token revokes its next authenticated request after the changed file is
observed. Public reads remain public without `--require-token`. Existing sockets
using removed tokens, changed secrets, names or scopes close before their next
queued tick; an already in-flight message or accepted command may finish. Legacy
write tokens and issued sandbox tokens are unaffected. A forced reload handles a
replacement with unchanged mtime/size. Status reports `tokens: {loaded_at, count}`
for the last successful load, without names. Protect the file as a credential.

| Scope | Permission |
| --- | --- |
| `read` | Every API GET, CSV export and WebSocket ticks |
| `trade:ACCOUNT` / `trade:*` | Orders and previews, cancels, flatten, exercise, stock closure, notes and sending or dismissing playbook stages on the named account / all live accounts; copied orders to account groups need `trade:*` |
| `replay` | Start, control and stop replays; trade, reset, set limits/guardrails and operate the kill switch on their isolated accounts |
| `admin` | Everything, including limits, guardrails, kill switch, resets, payouts, settlements, account creation/rename/archive/delete, account groups, playbook definitions and modes, replay history deletion and token reload |

`POST /api/replay/account/reset`, `PUT /api/replay/risk/limits`,
`PUT /api/replay/risk/guardrails` and `POST /api/replay/risk/kill` accept `replay`
or `admin`. Live counterparts still require admin. Normal validation, revision
checks and queued loosening apply inside replay too. Reset can apply pending
settings when the run has no later trading day.

The legacy write token has `admin` scope and actor name `legacy`. Writes always
check the token they carry: an unknown one gets 403 `WRITE_TOKEN_REQUIRED`, and a
valid one without the required scope gets 403 `SCOPE_REQUIRED`. Unrecognized route
families require `admin` for writes.

Without `--require-token`, reads remain public: any caller reads, and a read ignores
a token that matches nothing, such as a stale one saved in a browser tab. Loopback
writes without credentials remain open, including when a named-token file is loaded,
unless a legacy token, `--require-token` or `--sandboxes` closes them. Visitor
sandboxes close them because a public demo often sits behind a reverse proxy on the
same machine, where every visitor would arrive as a loopback writer; the operator
then trades with the legacy token (for example from `--write-token-file`) or a named
one. With `--require-token`, reads and writes require credentials even on
loopback, and reads need the `read` or `admin` scope; named tokens do not imply
`read`, so combine it with trade or replay scopes. Startup refuses the flag without
a configured token. Static terminal files remain public so the
browser can load the token entry screen. The terminal sends its saved token on
reads and CSV downloads. Browser WebSockets use the `openport` subprotocol plus
`openport.token.HEX`, where HEX encodes the token bytes; the server selects only
`openport`. Non-browser sockets can send the normal Authorization header. Tokens
never go in a URL query. Use HTTPS at your reverse proxy for remote credentials
and configure its public Origin with `--allowed-origin` when it rewrites Host.

### Visitor sandboxes

With `--provider demo --sandboxes N`, `POST /api/sandboxes` creates an isolated
practice account without the operator's token. Its temporary token has `read`
and `trade:ACCOUNT` scopes, restricted to that account and the simulated market.
Other clients cannot list or read it; authenticated admins can. It cannot use
admin routes, playbook actions, replays or backtests. The response returns the
secret once. Sandbox tokens, accounts and files expire after 24 hours unused and
are removed on restart. An expired sandbox token returns 403 `SANDBOX_EXPIRED`.
Creation returns 429 `SANDBOX_CAPACITY` when all account slots are occupied,
`SANDBOX_CLIENT_RATE` after 3 creations per client in a rolling hour, or
`SANDBOX_GLOBAL_RATE` after 30 globally. Order submissions, previews, what-if
requests, modifications, share trades and their previews share 60 requests per
account per rolling minute; excess requests return 429 `SANDBOX_ORDER_RATE`.
Cancels, flatten and share closes remain available at the order cap. Rate-limit
responses carry `Retry-After` seconds until the oldest counted request leaves
that bucket's window, rounded up to at least 1.
`SANDBOX_CAPACITY` omits the header because the next free time is unknown.
Creation may return 503 `SANDBOX_UNAVAILABLE` for an unavailable command inbox
(with `Retry-After: 1`) or trading storage (without the header). Notification
admin routes return 503 `NOTIFICATIONS_UNAVAILABLE` without `Retry-After` when
no notification service is available; sandbox tokens cannot use these routes.
The reducer and journal schema are unchanged. See [runtime](runtime.md#public-sandboxes)
for proxy headers, limits, storage and configuration.

### Actors

Each new journal transaction includes `actor` in its hash-protected payload: a
named token, `legacy`, `loopback`, or `system` for reducer actions such as market
matching, expiry, rules and assignment. The transport supplies the actor on the
queued `TradingCommand`; request JSON cannot choose it. Replay command provenance
also retains it, so verification uses the original actor.

Orders retain the actor that placed them. Fills and bracket children retain that
originating actor even when a later market transaction executes them. A cancel,
modification or note records its own caller on that transaction, without changing
the order's originating actor. Rule liquidation orders have actor `system`.
`GET /api/orders`, `GET /api/fills` and fills CSV expose actors; the Orders page and
trade review show them in small text. Earlier journals and orders default to
`unknown`; compaction preserves existing actor fields. Replay verification omits
the new fields when reproducing hashes from a run recorded before actors existed:
one whose first record has no actor and whose start input names no driver version.
That choice holds for the whole run, so a record whose actor was removed from an
attributed run is a differing transaction.

The [OpenAPI contract](openapi.yaml) describes response schemas, write bodies and
account selection. The [Python and MCP clients](../python/README.md) use these
same routes. All execution remains simulated.

### Durable startup and settlement sources

`--paper-journal PATH` defaults to `$HOME/.openport/paper-journal.jsonl`; containing
directories are created, and named accounts keep their journals in `accounts/` beside
it. `--paper-cash` defaults to `100000` and `--paper-fee` to
`0.65`. These seed new journals; recovery restores the recorded configuration. The
fee also seeds every new named account and visitor sandbox, whose cash and rules
come from their plan.
Existing files are verified, exclusively locked and resumed; an empty one holds no
transaction and starts the main account afresh (see
[Journal, recovery and failure handling](#journal-recovery-and-failure-handling)).
A corrupt, torn, locked
or unwritable journal disables its account's writes with 503 `TRADING_UNAVAILABLE`
and a status reason; other accounts carry on. They are never overwritten or silently replaced by an ephemeral
account. A runtime journal failure preserves the last committed account and
requires operator recovery. The Docker image journals in `/var/lib/openport`,
which is a declared volume; mount a persistent volume there.

At expiry, PM positions settle on the underlying’s **official close** when the
provider publishes one (`md::UnderlyingClose`): Cboe’s close field after the close,
which stops at the closing price while the price goes on with after-hours trades, and
which Cboe may revise within minutes (on 2026-09-24 SPY’s went from 767.27 to 767.18
ten minutes after the close, while the first print after 16:00 was 767.26). A revision
replaces the recorded close until the positions settle. Without one they settle on
the **first positive finite last print stamped at or after the regular close (16:00
ET, 13:00 early) on the expiry date**, in provider arrival order, the provider’s
closing print. ETF options that trade until 16:15 settle then, on that close, as OCC
exercises on the closing price. Bid/ask midpoints and next-day prices are not
substitutes.
Each account records the print in its journal (`record_close`, kept per underlying
and date) as soon as it holds a PM position expiring that day, so a restart before
ETF options expire at 16:15 still settles them on it. If none has arrived half an hour
of market time after the close, the underlying's last print before the close stands in,
provided it came within the close's last five minutes (a feed that stopped just before
the close); the engine keeps both prints in memory for a week of dates, so after a
restart only a print that arrives counts. Until then an expired PM position's
`settle_by` reads `closing_print`, as it does once the print is recorded; after it,
with neither, the position waits and its `settle_by` reads `manual`.

In demo and new replays (driver 5 and later), an AM position settles on the first valid
underlying print received at or after 09:30 ET on its expiry date. Premarket
prints and prints from other dates do not count; later prints do not replace the
first. Live demo prints must arrive within five minutes of 09:30 (through 09:35 ET),
so a late restart cannot mistake an intraday print for the open. Replay driver 5
keeps its recorded behavior; replays see the opening print from the beginning.
This is an approximation of the official special opening quotation, which
can differ materially from the underlying's opening print. Sources are
`demo_opening_print`, `scenario_opening_print` or `recorded_opening_print`, with
provider, underlying symbol and quote time. Waiting AM positions show
`settle_by: "opening_print"`. If no qualifying print arrives they continue to wait, with `settle_by: "manual"`
after 09:35 ET so the trader knows to import;
a manual import after expiry but before the print still settles exactly once.
Live real-provider AM positions settle by the explicit `POST /api/settlements` import
(the terminal's Settle button) with `settle_by: "manual"`, as their feed does not supply
the official special opening quotation, unless the server is started with
`--am-settlement cboe`. It then fetches the special opening quotations Cboe Global
Indices publishes for SPX (index `SET`), RUT (`RLS`), DJX (`DJS`) and VIX (`VRO`) from
their daily history
`https://cdn.cboe.com/api/global/us_indices/daily_prices/SET_History.csv`, at startup
and every 15 minutes (Cboe's CDN caches it that long), so a date's row is found soon
after Cboe publishes it and a restart still finds the last two weeks' values. The
endpoint was reachable without an account on 2026-10-21; the tests use responses
recorded then. NDX's quotation is Nasdaq's and is not fetched. A position settles only
on the history row for its exact expiry date. The index's delayed quote is not used:
its trade time cannot tell reliably whether the value it shows is the day's quotation,
computed once every component has opened, so it could settle a position early on a
wrong value or late. Settlement is journaled once: a row Cboe revises after a position
settled on it does not change that settlement. Waiting AM positions on those
underlyings show `settle_by: "soq"` while their expiry date is within the fetcher's
14-day lookback (counted in trading dates, which end at 17:00 ET) and settle once their
date's row arrives, with source `published_soq`, provider `cboe`, the `index`, `date`
and `url`. Past the lookback without one, or when the server runs without
`--am-settlement cboe`, they show
`manual`, and an import before the fetch still settles exactly once: manual import
remains the fallback for a fetch that fails or a value Cboe has not published. Demo and
replay accounts keep their opening print.

How long after the open Cboe posts a date's row in the history CSV has not been
measured: the endpoint was only checked on 2026-10-21, and the documented 15-minute
CDN cache adds to whatever the posting lag is. Treat the lag as unknown; a position can
wait from the open until the row appears. While any position is `AWAITING_SETTLEMENT`
it has no fresh mark, so the account's valuation is incomplete: new option and share
orders that need complete marks are refused with `STALE_QUOTE` (reduce-only closes of
other positions still run, see Flatten), and the trading-day rollover waits
(`Rollover requires complete marked equity`) until the position settles, by the
fetched value or a manual import. PM imports are
accepted only while no closing print is recorded. The same journal transaction
records the reference value, canonical definition and integration
`settlement_source`: `provider_official_close`, `provider_closing_print` or
`provider_last_print_before_close` with provider and time, `published_soq`,
`manual_am_import` or `manual_pm_import`. Preserve the official source used for an AM import
externally when an independent provenance audit is required. The first market
batch on a later trading date rolls the daily baseline on the finished day's closing
marks before its own quotes are applied. When a PM position of the finished day is
still to settle, or the close was not completely marked, the day instead rolls over
after that batch's quotes, once the position has settled and the marks are complete,
so the finished day's P&L by Greek and that night's assignments then follow those
quotes; the kill latch survives. All accounting uses effective market time, including
delayed feeds, rather than HTTP receipt time.

## Tests

`tests/support/scripted_market.hpp` supplies reproducible contract definitions,
market times, observations, quotes, valuations and requests for reuse by engine/API
tests. `tests/trading/` covers checked decimal arithmetic; long/short accounting and
basis residues; all execution/budget/priority/clock rules; open-order risk ranges;
loss/kill controls; scenarios; settlement; challenge, verification and funded-account rules; program fees, reset limits and payout net;
conditional and bracket orders; multi-leg orders, margin and buying power; order
changes, cancel-all and flattening; deterministic journal round trips, tampering,
torn suffixes, exclusive writers and injected write failures; state deltas (including
randomized round trips), checkpoints, damaged deltas, mixed-schema recovery and
compaction; overnight and curb sessions; trade notes and tags; P&L by Greek; exercise,
assignment and delivered shares. The CLI tests compact journals from earlier builds with `openportd`.

Engine and HTTP tests reuse that fixture for resting fills, cancellation, kill/limits,
JSON errors, write protection, restart recovery, AM/PM settlement, named accounts and
replays. Socket tests cover asynchronous POST/DELETE responses and shutdown of pending
commands. A dividend feed, trade-through matching and portfolio
margin remain outside v1.

Damaged main and named journals load a frozen read-only account at the last
verified transaction, without changing the journal or compacting its equity
sidecar. Account/status responses include `damaged` with the reason and last good
sequence/time. All account writes, including rename, archive and delete, return
409 `ACCOUNT_DAMAGED`. The terminal keeps portfolio, trades, fills and equity
readable and shows recovery instructions. A damaged first record exposes an empty
read-only account with sequence zero; its configured opening balance is not a
verified balance. Mid-file hash/JSON damage is never automatically removed.
Equity samples at/after the damaged boundary are discarded from the view and the
last verified marked equity is shown instead.

At 256 MiB or 100,000 verified records, `journal_size.warning` suggests backing
up and compacting live journals with the server stopped. Replay journals warn
that compaction prevents exact re-verification. Equity sidecars and replay
metadata also check the 64 MiB reserve before writing. Equity errors include the
failure wall time, sample market time and whether storage recovered; a successful
retry rewrites retained in-memory samples before marking recovery. The terminal
pages equity history in batches of 2,000; unpaged API reads remain available.

Backtest attempt report schema 2 reads daily evaluation rows from the carried
account's snapshots at input boundaries, without changing reducer commands or
journal bytes. Saved-run comparisons and combined independent daily P&L do not
share an account or its buying power, risk limits or drawdown floor. See
[batch backtests](playbooks.md#batch-backtests) for retention, pins and deletion.

### Direction rules (F65)

Custom plans may set `no_hedging` and `no_counter_positions` (both default false).
An order opens if any leg adds contracts or reverses beyond its holding; an order
that purely reduces held positions is exempt. Share trades follow the same rule.
Direction is the sign of dollar delta on one underlying: signed option quantity ×
current valuation delta × contract multiplier × spot, plus signed shares × price.
Compare the whole order's net delta, including closing legs, with held delta;
working orders do not count as held positions for `no_hedging`. The cross-account
rule also compares each other account’s working opening orders separately. Zero delta on either side does not
oppose a direction. Pure reductions and system exits are never refused by these rules.

`no_hedging` runs in the deterministic reducer, on acceptance and execution,
including changes and chained orders. An opening order opposing its own account's
held delta refuses with `HEDGING`. Missing, invalid, future or stale valuations
follow the exposure-limit policy (`MISSING_VALUATION`); unknown delta is never
assumed zero. Normal quote, risk and account checks still apply.

`no_counter_positions` checks **acceptance and each proposed fill**. It compares
opening delta with each other live main/named account
on the same server, excluding archived, replay and sandbox accounts. Only the
ordering account's setting counts: enable it on every account a trader uses.
A single-account server or replay has no other accounts to check. Each other
account’s held book and each working opening order (remaining quantity, including
armed entries and live OCO siblings) are separate comparisons; they cannot net
each other away. Purely reducing working orders and system exits do not reserve
direction. Unknown direction in a relevant other account refuses acceptance with
`MISSING_VALUATION` and holds fills until fresh valuations arrive.
The gate covers single/multi-leg entries, increased remaining order quantities,
share entries, playbook entries and their previews. Chained entries take the gate
when the chain is submitted and when children execute. A resting order that would
create a counter position cancels with `COUNTER_POSITION` before consuming liquidity,
including when a price-only change makes it executable. Checks use current marks
and the proposed fill quantity. Accounts run in deterministic desk order; earlier
fills in the batch are visible to later accounts.

A `COUNTER_POSITION` refusal creates no order or refusal transaction in the
account journal. Fill-time cancellations and the explicit direction inputs used
by fills are journaled; recovery never reads other accounts. Both direction reasons
include `actual` (order dollar delta), `limit` (the compared held or working dollar delta, a comparison
rather than a cap), `scope` (underlying), and structured `evidence` with
`underlying`, `order_dollar_delta`, `held_dollar_delta`, and, for a counter position,
`other_account`. Messages show signed whole dollars with thousands separators
(for example, `-$30,316` and `+$29,576`) in terminal tickets; numeric `actual`,
`limit`, and evidence fields retain full precision. Rule fields
are journaled only when true; plans with both off retain their journal bytes and hashes.

### Volume-share rule (F66)

`max_volume_percent` is a whole percent from 1 to 100; zero (the default)
disables it. Acceptance reads `ChainBook`’s `md::OptionVolume`; each proposed fill
rechecks explicit current-volume inputs supplied to the deterministic reducer. For every option leg of an opening
order, including combo ratios, the exact check is
`(abs(held contracts) + opening contracts) * 100 <= percent * traded volume`.
Opening contracts are `quantity × ratio` minus contracts closing an opposite
holding. A reversal counts its excess as opening; the formula still includes
the absolute existing holding. The order's full net delta is irrelevant to
this cap. Shares and purely reducing orders are exempt. Other working orders
are not reserved against volume. Increased-size modifications check the new
remaining quantity; chained children include their predecessors' hypothetical
holdings. Managed exits remain available.

Only finite, nonnegative whole contract totals, exactly represented by the
feed (up to 2^53−1), with a timestamp no later than market time and on the
**current market trading date** count. That date uses the market's evening
rollover, independent of a plan's custom day end. Missing volume, invalid values,
future observations and another trading date's volume refuse strictly; zero is
known zero and permits no opening contracts. A previous day's volume is not a
substitute for today's unknown volume.

`MAX_VOLUME_SHARE` includes `evidence: {contract, contracts, volume, percent}`;
`volume` is null when unknown/stale, `contracts` is held plus opening, and
`contract` is canonical OSI. `actual` is that contract count, `limit` is
`percent × volume / 100` unrounded (null without usable volume), so it may be
fractional; the order fits when held plus opening contracts are at most that value.
HTTP `scope` is the underlying.
The message includes these values for order and strategy tickets. Single-leg,
multi-leg, modification and playbook paths and their previews share the gate.
Acceptance checks today’s observation for the full requested quantity. Each fill
checks its proposed quantity against current holdings and volume, including earlier
fills in the same batch and partial fills of the same order. If the fill would
break the cap, the remaining order cancels with `MAX_VOLUME_SHARE`, retaining its
evidence and any previous fills. Unknown or stale volume also cancels at fill.
Acceptance refusals never reach the session journal; fill-time input facts and
cancellations do. Recovery needs no live feed. Older recorded runs without the
fill-check capability retain their acceptance-only behavior during verification;
the rule field remains omitted when zero.

Scenario backtests apply this same gate using the current generator revision,
which supplies current-date option volume from the opening snapshot. Both generated
scenario sets and explicit scenario days support the rule. Every requested recording
must contain at least one usable current-date volume observation or the request
returns HTTP 400; individual contracts without one still refuse at entry.
Demo, scenario replays and scenario backtests carry deterministic intraday volume,
with simulated opening activity at revision 6. Near-money contracts can trade under
a volume cap from the opening snapshot; thin contracts can still have zero volume.
Revisions 1–5 retain their original volume events, including a zero-volume opening.

### Terminal destination changes

**Back to live** selects the live feed and its selected paper account. Replay
silence alone does not trigger this change: the terminal verifies the run with
`GET /api/replay`, keeps unfinished runs selected, and shows “Reconnecting to the
replay…”. It returns to live if the run stopped or finished, or after two minutes
of failed server checks. A persistent banner names the resulting order destination;
**Back to replay** is available only while a run exists, and **OK** dismisses it.
Sandbox expiry and automatic account fallback are also announced.

An open ticket or write dialog keeps its original destination. A source, account,
or replay-run change blocks submission until the user explicitly confirms the
named new destination, or closes and reopens the action. Cancellation sends no
request. If the destination changes again during confirmation, the write remains
blocked. Account-scoped views that close during a switch must be reopened.
