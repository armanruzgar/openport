# Writing a provider adapter

An adapter translates a vendor feed into OpenPort's market-data events. Pricing,
analytics and paper trading consume those events without knowing the vendor.
Read the [architecture](architecture.md) for the data flow and the
[runtime notes](runtime.md) for provider options and shutdown behaviour.

## Live certification reports

Run a provider locally and attach the redacted reports to a
[provider report issue](https://github.com/armanruzgar/openport/issues/new?template=provider-report.yml).
Set credentials in the usual environment variables; never put them in the command.

```sh
# Polling: one complete cycle for every requested underlying, at most 60 seconds.
./build/apps/openport-probe --certify tradier SPY --expiries 2 --report-dir tradier-report
# Streaming: observe 30 seconds, checking at least 100 quotes per underlying.
./build/apps/openport-probe --certify tastytrade SPY --expiries 2 --seconds 30 --report-dir tastytrade-report
# Polling with a longer cycle deadline (rather than a timed multi-cycle run).
./build/apps/openport-probe --certify thetadata SPY --snapshot --seconds 120 --report-dir theta-report
```

The new directory (default `provider-report`) contains `conformance.json` and
`conformance.md`. Existing directories are refused so earlier evidence is not
overwritten. Without `--seconds`, polling stops after one cycle and streaming
runs for 60 seconds. `--seconds N` observes a duration for either kind of provider;
`--snapshot` selects one polling cycle with N as its deadline. A stream cannot
use `--snapshot`. `--quotes N` sets streaming readiness (default 100 per
underlying). Errors or end of replay can finish a run early. Stop joins provider
threads; system DNS can still delay cancellation as described in the runtime notes.
Demo recording preparation can consume a short observation window and delay
shutdown, yielding an honest `incomplete` report; elapsed time includes shutdown.

Reports count every publication before queue coalescing, list per-feed numeric
contract IDs with expiry/strike/right/settlement and activity coverage, and check
definitions before data. Underlying IDs follow the command-line subscription order. Market
clocks are checked for unknown/nonpositive values, future values and nondecreasing
order per event type and contract/underlying. Equal times and independent OI,
Greek and quote clocks are allowed. Operational `ProviderStatus` times are excluded.
Exact receipt-time matches are flagged for review. Timestamp values alone cannot
prove their origin: the report explicitly leaves wall-clock provenance unverified.
The ThetaData regression tests separately guard against the removed local-clock fallback.

IV, vega, theta and rho include presence, missing/invalid counts, ranges, target
units and scale flags. IV above 5 (500%) and negative vega are review hints.
Where spot is within 60 seconds and expiry/IV are usable, Greek magnitudes are
compared with BSM at zero rate/dividend; ratios outside [0.05, 20] are flagged.
The reference is derived, not observed market data, and cannot certify a vendor's
model. Missing comparison inputs never imply successful unit validation.

`fail` means a provider/startup error, undefined instrument, invalid activity
count/Greek, future timestamp or timestamp regression. `incomplete` means requested
readiness or actual definitions/quotes were absent. Both exit 1. `review` means
readiness was met but data is missing or a heuristic needs investigation;
`observed_checks_passed` means no checked anomaly was seen. Both exit 0, and
**neither proves live entitlement, timestamp provenance or unit correctness**.
Bad CLI flags or report output failures exit 2. Startup failures after entering
certification still produce reports when the directory is writable.

Redaction uses a structured allowlist: no configuration values, environment,
URLs, raw payloads, diagnostic text or symbol strings are exported. Keys, tokens
and account identifiers in those strings cannot escape through status or error
messages. Error counts/categories and degradation counts remain; contract and
underlying identities use numeric IDs. The directory is owner-only. Review the
files before sharing; do not attach raw recordings or licensed feed payloads.
Certification cannot be combined with `--record`, `--analyze` or `--compare-mfiv`.
Demo and replay are supported for offline checks and explicitly labelled simulated
or replayed; they never count as live provider validation. The report format is
local to the probe and adds no HTTP API, wire-event or recording fields.

## The interface

Implement [`md::Provider`](../include/openport/md/provider.hpp) in
`include/openport/providers/` and `src/providers/`. It exposes:

- `name()` and `capabilities()`, both `const noexcept`.
- `start(const md::Subscription&, md::EventSink&)`, which starts delivery on the
  adapter's threads and returns promptly. The sink must outlive the provider.
- `stop()`, which stops delivery and joins every thread. Repeated calls must be
  safe. Call it from the derived destructor before members used by workers die.

`EventSink::publish(md::Event)` takes an event by value. Production sinks must be
thread-safe. Use one ordered publication path when definitions and data could
otherwise race across workers. Do not retain a sink after stopping. The engine's
queue coalesces state while analytics is busy; an optional recording sink captures
events before that queue. The adapter must not change downstream state directly.

`Capabilities` describes what the adapter supplies: quotes, trades, open interest,
vendor Greeks, history, real-time data, poll interval and delay. Zero poll interval
means streaming. Set `realtime_plan_dependent` for plan-dependent entitlement and
report the actual `Live` or `Delayed` state per underlying; Massive is the example.
Do not advertise real-time data simply because requests succeed. A capability flag
alone does not implement a history endpoint; the current candle backfill is a
separate Cboe service, described in [price history](runtime.md#price-history).

`Subscription` names underlyings and optionally limits nearest expiries or the
fractional strike window around spot. Zero means unrestricted. Snapshot adapters
use `ChainFilter` for new contracts and continue updating contracts already
defined, even if they move outside the window. Otherwise positions could keep stale
prices. If the vendor cannot honour a filter, reject it explicitly: Databento
parent subscriptions send whole chains upstream and reject both filters.

Register the name in `provider_names()` and construct the adapter in
[`providers::make_provider`](../src/providers/factory.cpp). The factory takes
`md::ProviderConfig` (name, API key and string options). Allowlist option keys,
validate values with [`options.hpp`](../include/openport/providers/options.hpp),
and extend `validate_subscription()` if needed. Both
[daemon](../apps/openportd/main.cpp) and [probe](../apps/probe/main.cpp) look up
`<UPPERCASE_PROVIDER_NAME>_API_KEY`; the broker factory instead reads
`TRADIER_ACCESS_TOKEN`, tastytrade's and Schwab's OAuth variables or Alpaca's
`APCA_API_KEY_ID` and `APCA_API_SECRET_KEY`. Keep secrets out of URLs, exceptions and status
messages. Add source files to [src/CMakeLists.txt](../src/CMakeLists.txt) and tests
to [tests/CMakeLists.txt](../tests/CMakeLists.txt). Optional vendor SDKs must remain
behind a build option, as Databento does; Interactive Brokers sits behind
`OPENPORT_WITH_IBKR` in the same way, though it needs no SDK.

## Events and ordering

The complete vocabulary is in [`md/events.hpp`](../include/openport/md/events.hpp).
Contract conventions and OSI parsing live in
[`md/contract.hpp`](../include/openport/md/contract.hpp). Keep SPX and SPXW's AM and
PM settlement families separate, even on the same expiry date; preserve exercise
style, multiplier and the nonstandard-contract flag.

| Event | Meaning and obligations |
| --- | --- |
| `ContractDefinition` | Assign a stable, dense, per-feed `InstrumentId`. Publish its definition before any event referring to that ID. Definitions are ordering barriers in the queue. |
| `OptionQuote` | Best bid and offer, with sizes in contracts. A zero price means no quote on that side, not an executable free option. Preserve missing sides and sizes; never manufacture liquidity. |
| `OptionTrade` | A reported execution price and size, not an inferred mid-price trade, with the printing venue in the provider's own numbering when it says (Databento's OPRA publisher id, ThetaData's exchange code), zero otherwise. Recordings keep a known venue as an optional trailing field. Trades may be dropped under queue overload; quote state is retained. |
| `OptionVolume` | Cumulative contracts traded in the trading date of `ts`, not an increment. Publish only finite nonnegative counts with a known market timestamp. Snapshot deduplication retains equal totals across trading-date changes. |
| `OpenInterest` | The vendor's reported contract count. Missing OI is not zero OI: publish only what is known. |
| `VendorGreeks` | Comparison values, not OpenPort's pricing inputs. IV is decimal, gamma per dollar of underlying, vega per vol point, theta per calendar day and rho per 1% of rate, all per unit of underlying. Use NaN for an unpublished field or undocumented unit. |
| `UnderlyingQuote` | The underlying's bid, ask and/or last, on its own market clock. An index's old closing print must not acquire the options' newer overnight timestamp. If no underlying price is supplied, leave it absent; analytics can infer spot from parity. |
| `UnderlyingClose` | A vendor-published official close for a particular trading date, with the market-data time of publication. Send revisions when supplied. It supports circuit-breaker references and PM settlement; do not invent it from the last option quote. |
| `SnapshotComplete` | A complete snapshot of one underlying at its market time. It vouches for all defined contracts in that underlying, including unchanged quotes suppressed by deduplication. |
| `ProviderStatus` | `Connecting`, `Live`, `Delayed`, `Stale`, `Error` or `Stopped`, with a useful message and underlying. An empty underlying is reserved for provider-wide failures. |

A normal snapshot sequence is: status `Connecting`, underlying data if supplied,
definition before each contract's first data, quote/OI/Greek updates, retirement of
missing quotes, `SnapshotComplete`, then the underlying's healthy status. All
definitions need not precede the first quote; each definition must precede its own
data. Underlying events do not need contract IDs. A stream publishes definitions
before dependent updates, then reports each underlying live when its quotes arrive.
Connection metadata alone is not evidence that every subscription is live.

### Timestamps

`md::Timestamp` is signed Unix nanoseconds in UTC. Convert the vendor's documented
units and time zone with the helpers in [`md/time.hpp`](../include/openport/md/time.hpp).
Retain separate option, underlying and close clocks. A response received now may
describe a delayed market or yesterday's close. Market events and
`SnapshotComplete` must describe that market time, not the HTTP receipt time.
Unknown timestamps must remain unknown or cause an explicit error; do not make
old quotes fresh by stamping them with `md::now()`.

`ProviderStatus::ts` may use wall time for operational health. Network timeouts,
polling and retry waits also use local clocks; these are separate from the
simulator's market clock. Recording stores receipt time separately to pace replay
and preserves each event's market timestamp. Events of different kinds need not
have globally increasing timestamps: a newly reported OI value or official close
can describe an older market observation. Preserve source order and avoid
artificially advancing a snapshot's clock on a repeated cached response.

Cboe's session-aware delayed timestamp mapping is in `publish_chain()` in
[`cboe.cpp`](../src/providers/cboe.cpp). Databento's CBBO uses `ts_recv`, the vendor
record's observation time, while CMBP-1 uses `ts_event`; these are not the local
machine's receipt timestamp. ThetaData's `publish_chain()` uses the latest data
clock and retains a per-underlying high-water mark. Its current all-clocks-missing
fallback uses `md::now()`; do not copy that fallback into a new adapter. The
timestamped path is covered by `ASnapshotIsStampedWithItsDataNotTheWallClock`.

### Snapshots and streams

Reuse [`PollingProvider` and `SnapshotPublisher`](../include/openport/providers/snapshot.hpp)
for a snapshot API. Implement `poll()` and `healthy_state()`. `poll_once()` accepts
an injectable HTTP client and reports success or failure for the requested
underlying. The loop continues with other underlyings after a failure.

Fetch and validate every required page or root before publishing the completed
chain. `SnapshotPublisher::define()` emits each definition once; `quote()`,
`open_interest()`, `volume()` and `greeks()` suppress unchanged values. Volume is
republished on a trading-date change even when the count is the same. Track the IDs present
and call `finish(underlying, seen, market_time, sink)` only after a successful,
complete snapshot. It clears quotes absent from that snapshot for that underlying
and sends `SnapshotComplete`. A failed page must not retire the previous complete
chain or claim freshness. An empty successful snapshot and a failed request have
different meanings. Optional IV/OI endpoint failures can report degradation while
still publishing a complete quote snapshot, as ThetaData does.

Streaming adapters publish updates as received and do not manufacture
`SnapshotComplete` messages for heartbeats. On reconnect, restore subscriptions
and definitions before dependent data and reset health per underlying. See
`DatabentoMapper` and `DatabentoRecovery` for parsing separated from reconnection.
Bound retries and make waits interruptible. HTTP polling passes `cancellation()`
to requests and checks it between pages and endpoints. System DNS resolution
remains the shutdown exception documented in [runtime](runtime.md).

## Existing vendor mappings

These are implementation maps, not live certification. As the
[configuration notes](configuration.md#providers) state, Databento, Massive,
ThetaData, Tradier, tastytrade, Schwab, Alpaca and Interactive Brokers have
sample-response tests but have not yet been run live with credentials.

| Adapter | Mapping and code to read |
| --- | --- |
| [Cboe](../src/providers/cboe.cpp) | `parse_cboe_chain()` reads chain JSON; `cboe_page_chain()` extracts the embedded chain from quote pages when data files fall behind. Index file names use an underscore. OSI symbols become contract definitions, quote/OI/Greek fields become events, and the underlying retains `last_trade_time`. `publish_chain()` applies the delay and each product's sessions, publishes official closes and revisions, then completes the snapshot. |
| [Massive](../src/providers/massive.cpp) | `parse_massive_chain_page()` reads `/v3/snapshot/options/{underlying}`, strips `O:` from option tickers and follows `next_url`. Index requests use `I:`. `publish_chain()` uses quote `last_updated` and the underlying's separate timestamp; quote `timeframe` determines entitlement. Published Greeks use the normalised units; rho is NaN because it is not supplied. All pages are collected before publication. |
| [ThetaData](../src/providers/thetadata.cpp) | `parse_theta_rows()` reads v3 NDJSON. The adapter requests `quote`, `greeks/first_order` (falling back to `greeks/implied_volatility` on failure) and periodically `open_interest` for each option root, then joins by root, expiry, strike and right. Times without a zone are New York times. The latest timestamped Greek row supplies spot. IV and delta are copied; vega/rho are divided by 100. Theta stays NaN because its time basis is unspecified; first_order supplies no gamma. Unknown row and snapshot clocks remain zero. Auxiliary failures appear in status. |
| [Databento](../src/providers/databento.cpp) | OPRA parent symbols cover every root (`.OPT`). `DatabentoMapper::on_record()` maps definitions, CBBO/CMBP-1 quotes, trades and OI statistics. It converts fixed-point prices, handles undefined sentinels and ignores data for unknown instruments. OPRA provides neither underlying prices nor vendor Greeks here. Definitions and statistics are subscribed with replay before the current quote stream. |
| [Tradier](../src/providers/tradier.cpp) | Expirations and lookup accept both documented shapes and object/array/null lists. Lookup resolves root/date pairs, including SPXW; every selected chain is fetched before publication. Compact OCC symbols become canonical padded OSI. Quotes retain millisecond clocks; underlying last keeps its own clock. OI and ORATS Greeks are mapped; chain volume uses its own `trade_date`. Greek update times without a zone stay unknown. Option sizes default to contracts with an explicit override. `TradierBudget` accounts for each request and rate-limit headers. Production index timing remains unconfirmed. |
| [tastytrade](../src/providers/tastytrade.cpp) | OAuth refresh uses JSON and environment credentials. Nested chains supply canonical OSI definitions and streamer-symbol mappings; instrument responses supply index streamer names. `TastytradeMapper` applies filters and maps Quote, Greeks, Summary OI and Trade session volume/underlying prices. Unused Profile subscriptions are omitted. `DxlinkProtocol` implements the handshake, accepted COMPACT field order, keepalive and paced add/remove subscriptions independently of `net::WebSocket`. Reconnect repeats the handshake with the retained subscription set. Summary uses a supplied `eventTime`, otherwise zero. DXLink Greeks use the documented per-point/day units; the timestamp option can withhold clocks if their millisecond units are unconfirmed. No complete snapshots or inferred trades are emitted. |
| [Charles Schwab](../src/providers/schwab.cpp) | `SchwabApi` refreshes OAuth with a form body and Basic client credentials, keeps a rotated refresh token in memory and retries a 401 once. `parse_schwab_chain()` walks `callExpDateMap`/`putExpDateMap`, checks each padded OSI symbol against its side, strike and expiry key, keeps `quoteTimeInLong` and the underlying's own clocks, reads an option's prices from the published schema's `bidPrice`/`askPrice` (falling back to `bid`/`ask`, which some clients read; sizes, `totalVolume`, `openInterest` and the underlying's fields already match it), and maps OI, `totalVolume`, delta and gamma (`-999` is unavailable). `/expirationchain` selects the nearest dates for `fromDate`/`toDate`. `isDelayed` decides `Live`, `Delayed` or, when absent, `Stale`. |
| [Alpaca](../src/providers/alpaca.cpp) | `parse_alpaca_option_page()` reads one page of option snapshots keyed by compact OCC symbol; `parse_alpaca_stock_snapshot()` the underlying's latest trade or quote. RFC 3339 times must carry a zone. Pages are followed by `next_page_token` before publication; the strike window becomes request bounds widened to defined strikes. Index underlyings are rejected. The indicative feed reports `Delayed`; OPRA `Live`. |
| [Interactive Brokers](../src/providers/ibkr.cpp) | Client Portal Web API via the local gateway. `/tickle` keeps the session and reports its authentication; search, strikes and info map conids to OSI from trading class, maturity, right and strike, cached for the day. `parse_ibkr_snapshot()` reads display strings (`1.2K`, `C` closes) and `_updated` clocks, and the day's volume (7762) only from real-time rows; first snapshots of a contract only warm its line. One `RequestBudget` paces requests; market-data lines are counted across underlyings. |

The broker adapters allowlist their REST paths and use non-redirecting HTTP calls.
OAuth refreshes and the Interactive Brokers keepalive are the only POSTs.
[`RequestBudget`](../include/openport/providers/request_budget.hpp) is a rolling
request window with exponential backoff for adapters without Tradier's headers. Error messages exclude server bodies and tokens. Broker
rate timers use operational time, independently of market-event clocks. See
[runtime limitations](runtime.md#broker-market-data) for unverified units and the
consequences for paper fills.

## Vendor units and source references

The mappings below are **not yet validated live**. Public API definitions and
sample-shaped tests establish the conversion, not entitlement or the correctness
of a particular account's data. Every Greek is per unit of underlying, never
multiplied by the contract multiplier. NaN remains missing downstream.

| Provider | IV | Vega | Theta | Rho | Greek clock |
| --- | --- | --- | --- | --- | --- |
| Tradier / ORATS | `mid_iv`, decimal | `vega`, per vol point | `theta`, per day | `rho`, per rate percentage point | Unzoned `updated_at` remains unknown |
| tastytrade / dxFeed | `volatility`, Black-Scholes decimal | `vega`, per vol point | `theta`, per day | `rho`, per rate percentage point | `time`, epoch milliseconds |
| ThetaData | `implied_vol`, decimal | `vega / 100` | NaN: daily versus annual basis not specified by the endpoint | `rho / 100` | `timestamp`, New York when unzoned |

Tradier documents [chain Greeks as ORATS data](https://docs.tradier.com/reference/brokerage-api-markets-get-options-chains).
Its [quote reference](https://docs.tradier.com/docs/quotes) shows decimal IV fields;
ORATS defines [theta, vega and rho](https://orats.com/near-eod-data) and
[percentage-point vega](https://orats.com/university/option-scanning).
dxFeed defines [Greek derivatives and time](https://docs.dxfeed.com/dxfeed/api/com/dxfeed/event/option/Greeks.html),
including vega/rho against percentage volatility/rate and theta against days.
ThetaData's [Greek calculation notes](https://thetadata.net/docs/Articles/Data-And-Requests/Option-Greeks.html)
explicitly require dividing vega and rho by 100. Its
[first-order snapshot](https://thetadata.net/docs/operations/option_snapshot_greeks_first_order.html)
does not state theta's time basis, so OpenPort does not guess a conversion. If that
route is unavailable, the IV route still supplies the previous IV-only coverage.

Tradier [chain volume](https://docs.tradier.com/reference/brokerage-api-markets-get-options-chains)
is a cumulative count, stamped with the option's `trade_date`, never the quote or
request time. Missing trade clocks withhold volume. DXLink
[Trade.dayVolume](https://docs.dxfeed.com/dxfeed/api/com/dxfeed/event/market/TradeBase.html)
is cumulative, not an individual execution. Its `dayId`, when supplied, must match
the timestamp's trading date. The standard
[Summary](https://docs.dxfeed.com/dxfeed/api/com/dxfeed/event/market/Summary.html)
has `prevDayVolume`, not `dayVolume`: OpenPort never substitutes yesterday's count.
A negotiated Summary extension carrying `dayVolume` is accepted with `eventTime`.
Summary OI uses [embedded event creation time](https://docs.dxfeed.com/dxfeed/api/com/dxfeed/event/market/MarketEvent.html#getEventTime--)
when present; this is not an OI effective-date claim. Network events may supply
zero; OpenPort keeps it unknown. Interactive Brokers'
[field 7762](https://www.interactivebrokers.com/docs/web-api/ws/streaming-data) is
the day's volume at full precision; field 87 rounds the same count to `K`/`M`. Only a
real-time row's count is stamped with its `_updated` time: delayed rows arrive 15-20
minutes late and frozen rows hold the last close's values. A positive count beside a
`C` (previous close) or halted last price, or with no last, stays unknown, since it
may be an earlier session's. All these volume mappings still need live validation.

## Failover

[`FailoverProvider`](../src/providers/failover.cpp) wraps two adapters for
`--fallback-provider`. It is itself an `md::Provider`: each child publishes into its
own sink, definitions are remapped by OSI symbol into one id space, and per-underlying
routes decide which child's events pass. `ProviderStatus::feed` names the feed in
charge; it is not recorded. The wrapper reports trade venues when either feed
does, so a recording's header admits the venued prints of whichever is serving. A new adapter needs nothing extra to serve as either
feed, but its health matters: `Error` and `Stopped` count as failing; a `Live` or
`Delayed` status, or a `SnapshotComplete` whose market time advanced, as fresh, so
`Stale` with advancing snapshots keeps the primary. Tests drive two hand-made feeds and an injected clock
through `tick()` ([tests](../tests/providers/failover_test.cpp)).

## Historical day imports

Historical imports use `HistorySource` in
[`history.hpp`](../include/openport/providers/history.hpp), separate from the live
provider threads. `openportd --import-day` validates the trading date before opening
a transport. The common writer filters chains, orders bounded time windows, assigns
dense IDs and writes through `RecordingSink`. It publishes the final file only after
successful completion. The recording's imported flag defaults to false for older files.

`databento_history.cpp` calls the vendored client's `TimeseriesGetRange` for
`OPRA.PILLAR`, using parent symbology for every option root. Definitions and daily
statistics are read first for each UTC date, with the preceding business date's OI
as the initial baseline. OI is released only when its observation and capture times are reached.
Vendor IDs are rebuilt on a UTC date change; recording IDs are
retained by OSI symbol. This also covers holiday GTH spanning several UTC dates.
CBBO uses the same `ts_recv` and fixed-price conversion as `DatabentoMapper`;
trades keep `ts_event` while `ts_recv` orders their delivery, including late prints.
`HistorySource::receipt_time` supplies the original capture clock during publication;
sources without it use event time. The callback is injectable for saved SDK record tests.
No other dataset or underlying price is assumed.

`theta_history.cpp` uses Theta Terminal v3 NDJSON: `/option/list/expirations`,
`/option/list/strikes`, `/option/list/contracts/quote`, `/option/history/quote`, `/option/history/trade`,
`/option/history/open_interest`, `/index/history/price` and `/stock/history/quote`.
It uses date and time ranges, expiration, strike and right selectors, and `1s`
quote/price intervals. New York timestamps and optional fields are checked before
publication. Its HTTP client and rate wait are injectable; failures do not print
response bodies. There is no historical IV request or invented underlying last price.
The expiration and strike lists have no date parameter; the date-specific contracts
list restricts them to series that were quoted on the imported sessions' dates.

`massive_history.cpp` uses the live adapter's `https://api.massive.com` origin
and bearer authentication with `MASSIVE_API_KEY`. Historical contracts come from
`/v3/reference/options/contracts` with `as_of` and expiration filters, including
contracts that have since expired. Options quotes and trades use `/v3/quotes/O:...`
and `/v3/trades/O:...`, ascending nanosecond timestamp bounds and same-origin
pagination. One page per contract/stream is retained across import windows, so
quiet periods do not trigger repeated empty requests. Pages request Massive's
50,000-row maximum, reduced so that one page per stream fits the one-million-record
buffer (never below 100 rows); a chain of 20 or fewer streams uses full pages. SIP timestamps order delivery;
trades preserve an exchange timestamp when supplied, otherwise their SIP time.
No current snapshots, OI, vendor Greeks or underlying prices enter the history.
Nonstandard deliverables are excluded; corrected/cancelled trades fail explicitly
because the normalized event model cannot undo their volume. HTTP/wait injection
supports offline tests, and errors never include response bodies or credentials.

All three sources use the same range runner: `--import-days START..END` and repeated
`--date`/`--import-days` flags expand into sorted unique dates. The session calendar
skips closed dates. Existing files resume only after their provider, trading date,
symbols, filters and clean end are checked. A failure keeps earlier complete days.

API references: [Databento ranges and limits](https://databento.com/docs/api-reference-historical/basics),
[Databento instrument identifiers](https://databento.com/docs/standards-and-conventions/common-fields-enums-types),
[ThetaData historical quotes](https://thetadata.net/docs/operations/option_history_quote.html),
[ThetaData historical contracts](https://thetadata.net/docs/operations/option_list_contracts.html),
[Massive contracts](https://massive.com/docs/rest/options/contracts/all-contracts),
[Massive quotes](https://massive.com/docs/rest/options/trades-quotes/quotes),
[Massive trades](https://massive.com/docs/rest/options/trades-quotes/trades)
and [ThetaData concurrency](https://thetadata.net/docs/Articles/Data-And-Requests/Concurrent-Requests.html).

The importers follow the documented Databento Historical, Massive and Theta Terminal APIs, are tested
against saved response shapes in `tests/data/history`, and have not yet been run
live with a key. Fixtures contain generated values, not licensed market data.
Provider data stays on the user's machine; never bundle, commit or upload it.
See [historical import operation](runtime.md#importing-a-historical-day) for
credentials, request limits, filtering, session coverage and failure handling.

## Simulated feed

[`DemoProvider`](../src/providers/demo_feed.cpp) registers as `demo`. It rotates
built-in regular scenarios, generates the next recording while the current one
plays, and uses `ReplayProvider` pacing and complete batches. Contract IDs are
mapped by OSI across recordings; omitted quotes are retired before the next
snapshot. The engine uses the simulated receipt clock for freshness while keeping
live paper accounts. Capabilities declare 15-second snapshots and open interest;
status names the current simulated day. No end-of-recording stop escapes between
days. [Options, dates and storage](runtime.md#demo-feed).

## Test without the network

Use saved, hand-written response shapes with generated values. Do not commit a
downloaded market-data response or recording: the [contribution policy](../CONTRIBUTING.md#data)
prohibits redistributing provider data. Tests must run without keys, a local vendor
terminal or network access.

Start with parser tests using string literals, then exercise publication through
a collecting `EventSink`. A vector-backed collector is sufficient when the test
calls `publish_chain()` or `poll_once()` synchronously; protect it if testing worker
threads. Check event order as well as field values and units.

[`tests/support/http_stub.hpp`](../tests/support/http_stub.hpp) provides
`test::HttpStub`: set `respond` to return a `net::HttpResponse` for each requested
URL and inspect `urls` afterwards. Pass the stub to `poll_once()` without starting
the background loop. This exercises pagination, status, failures and cancellation
without opening a socket. For Databento, construct SDK record structs and call
`DatabentoMapper::on_record()` directly; no live client is needed.

Useful examples:

- [Cboe tests](../tests/providers/cboe_test.cpp): definitions before data,
  deduplication, per-product clocks, filter drift, close revisions and page fallback.
- [Massive tests](../tests/providers/massive_test.cpp): reordered fields, null
  nested objects, missing quotes, plan entitlement and failed pagination.
- [ThetaData tests](../tests/providers/thetadata_test.cpp): endpoint joins,
  per-underlying OI refresh, auxiliary failures and a failed required quote root.
- [Databento tests](../tests/providers/databento_test.cpp): record conversion,
  undefined values, reconnect ordering and per-underlying live state.
- [Broker tests](../tests/providers/tradier_test.cpp): complete root-aware polls,
  missing fields, clocks, filter drift, cancellation, budgets and backoff.
  [tastytrade tests](../tests/providers/tastytrade_test.cpp) cover OAuth and mapping;
  [DXLink tests](../tests/providers/dxlink_test.cpp) use a fake transport and clock
  for recorded message shapes, keepalive, subscription caps and resubscription.
  [Schwab](../tests/providers/schwab_test.cpp), [Alpaca](../tests/providers/alpaca_test.cpp)
  and [Interactive Brokers](../tests/providers/ibkr_test.cpp) tests cover OAuth,
  pagination, conid mapping, warming lines, keepalive, pacing and line limits.
- [Runtime tests](../tests/providers/runtime_test.cpp): cancellation between
  requests, option validation and probe readiness.

After building, run the focused cases and then the full suite:

```sh
./build/tests/openport_tests --gtest_filter='Cboe*:Massive*:ThetaData*:Databento*:Tradier*:Tastytrade*:Dxlink*:Schwab*:Alpaca*:Ibkr*:RequestBudget*:BrokerJson*:BrokerFactory*:SnapshotPublisher*:PollingProvider*:ProviderOptions*:Replay*:Cli*'
./build/tests/openport_tests
```

Add the new test suite to the filter while iterating. CI uses GCC 13 and Apple
Clang with warnings as errors. If the adapter needs terminal changes, add Vitest
coverage too; see [Contributing](../CONTRIBUTING.md#building-and-testing).

## Record a session for replay

When you have authorised access to a feed, run this from a checkout. Use paths
inside your workspace to keep the recording, account and candle files together:

```sh
mkdir -p var/provider-check
./build/apps/openportd --provider cboe --symbols SPX,SPY \
  --paper-journal var/provider-check/paper.jsonl \
  --candle-dir var/provider-check/candles --no-history --no-cboe-holidays \
  --record-dir var/provider-check/recordings --web-root web/dist
```

Substitute the new provider and its documented options to test it. Stop normally
to write the clean-end marker. The Replay page lists the recorded sessions and
opens one beside the live feed in its own practice account. To inspect a file
without connecting a live provider, replace `FILE.oprec` with the generated name:

```sh
./build/apps/openport-probe replay SPX \
  --option file=var/provider-check/recordings/FILE.oprec \
  --option speed=max --analyze
```

`--record FILE` is an alternative for a single named recording. Files are created
exclusively, never overwritten; full chains can be large. Recordings contain the
normalised events, original capabilities and subscription, not raw vendor JSON.
Keys and provider options are excluded, but status messages are saved verbatim.
Keep recordings private and check the vendor's terms before sharing. See
[recording and replay](runtime.md#recording-and-replay) for timing and recovery.

## Pull-request checklist

- [ ] Register the factory name, validated options, build sources and tests.
- [ ] Document required plan, key environment variable, defaults and supported
  filters in the README's provider table, [configuration](configuration.md)'s
  Providers and flags, and the runtime notes.
- [ ] Check definitions before data, stable IDs, contract conventions, price and
  size units, missing values, timestamps and delayed/real-time entitlement.
- [ ] Test multiple underlyings, unchanged snapshots, disappearing contracts,
  filter drift, partial failures and per-underlying status.
- [ ] Test malformed responses, pagination or stream recovery, cancellation and
  repeated `stop()`. No keys, sleeps waiting for a market, or external services.
- [ ] Verify record/replay preserves the mapped events. Prefer the existing event
  vocabulary; new fields or formats need explicit compatibility tests for older
  recordings or journals they affect.
- [ ] Report whether validation used fixtures or a live account. Never present
  generated quotes as observed data or promise entitlement you have not checked.
- [ ] Run the full C++ suite and relevant web checks, and include the results.
