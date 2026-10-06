# Runtime and provider operation

`Engine` and `WebServer` permit one `start()` attempt per instance. To restart,
construct a new instance. Engine startup failure stops the provider before its
queue is destroyed. Server shutdown closes the listener and all HTTP/WebSocket
sessions on their executors and drains cancellation handlers before joining.

HTTP request reads have a 60-second deadline, including idle time between requests
on a keep-alive connection. Once a request is fully read, that deadline is cleared
while the engine prepares its response, including long replay operations. Each
response write gets a fresh 60-second deadline; a successful keep-alive response
starts a new request read deadline. `WebServer` callers can override `read_timeout`
at construction without changing the response write deadline.

Polling shutdown checks a shared atomic cancellation flag every 25 ms while
connecting, handshaking TLS, writing, and reading. Pagination, ThetaData endpoint
sequences, and open-interest refreshes check cancellation between requests.
System DNS resolution uses synchronous `getaddrinfo`; shutdown cannot interrupt
it until the operating system returns.

Account margin settings live in each paper account's rules and journal. The terminal's
new-attempt and new-account dialogs, or the corresponding reset/create API requests,
select strategy or portfolio margin, margin/cash/IRA account type, house margin and
portfolio IV shock. These settings preserve a preset's evaluation identity; cash and
IRA accounts require strategy margin and buying-power enforcement. See
[account rules](paper-trading.md#account-rules-and-evaluations) for their limits and delivery behavior.

The event queue has a default capacity of 65,536 retained events. Repeated option
quotes, underlying quotes, vendor Greeks, volume and open interest replace an unconsumed
update of the same kind in place. A contract definition prevents option updates,
including volume and open interest, from coalescing across that definition. Latest-value
state is never dropped: snapshot providers may deduplicate unchanged values and
never resend them, ending each poll with `md::SnapshotComplete` to vouch for them
(see [paper trading](paper-trading.md)). Definitions, provider status and complete
snapshots are also always retained.

Capacity limits the trade backlog: at capacity, incoming trades are dropped and
incoming state evicts queued trades first. With no trades to evict, state is
retained beyond capacity. Coalescing indexes use drain generations, so ordinary
drains invalidate them without scanning every known instrument or underlying.

Both status and tick JSON expose `engine.queue_depth`, `coalesced_events`,
`dropped_events`, and `overloaded`. Counters are cumulative for the engine's
lifetime; only trades contribute to `dropped_events`. `overloaded` means depth
exceeds capacity or trades have been dropped since the last drain. WebSocket
clients retain the active write and only the newest pending tick, since ticks
contain complete snapshots. The server holds at most 256 `/ws` sessions at once,
handshakes included; the next upgrade gets 503 `WebSocket session limit reached` with
`Retry-After: 5`, and a slot frees as soon as another session closes. Paper commands
queue in a bounded inbox of 256; a full one answers 503 `TRADING_UNAVAILABLE` with
`Retry-After: 1` (see [paper trading](paper-trading.md#commands-and-views)).

The engine publishes health state and message changes after the batch that
contains them. Otherwise it publishes receipt and error timestamps at most once
every 100 ms, including pending updates when the feed becomes idle. Timestamp-only
updates reuse the published health map and strings.

The market-wide circuit breakers publish their own snapshot in status and ticks: the
watched symbol, the previous close, the day's level and its halts (see
[the paper API](paper-trading.md#engine-integration-and-http-api)). With paper trading,
`market-halts.json` beside the main journal keeps them across restarts, read and
written only once the main journal has opened; replays and `--no-paper` keep them in
memory. A read or write failure appears in `circuit_breaker.error` and the daemon log,
and everything else carries on.
`inactive_reason: "MISSING_PREVIOUS_CLOSE"` explains a missing breaker reference;
null means the reference is available. Authored scenario halts are independent,
reported as level 0 and shown with their resume time in the terminal.

## Homebrew and Docker Compose

`Formula/openport.rb` installs the self-contained Apple Silicon release archive,
including `bin/`, `share/openport/web` and documentation. It needs no source build.
Use Docker on Intel Macs and Linux. [Installation](install.md) has the commands, and
[CONTRIBUTING](../CONTRIBUTING.md#releasing) the release step that fills in the
formula's checksum.

`brew services start openport` binds to `127.0.0.1:8080`. The service explicitly puts
the paper journal, other accounts, replay journals, candles and volatility history
under Homebrew's `var/openport`. It keeps its write token at `var/openport/write-token`
and stdout/stderr at `var/log/openport.log` and `var/log/openport.error.log`.
Open the printed `http://localhost:8080/#token=…` link to save the write token in that
browser tab. If stdout is buffered, append the token file's contents to
`http://localhost:8080/#token=` yourself. Protect both logs and token files.
The service does not record the feed by default; the usual recording flags apply
when running the daemon yourself.

`docker compose up -d` uses `ghcr.io/38st/openport:latest`, publishes port 8080 only
on `127.0.0.1`, and keeps data in the named `openport` volume at `/var/lib/openport`.
`docker compose logs openport` shows the token link. Provider environment variables
and an example provider command are commented in `docker-compose.yml`. The restart
policy is `unless-stopped`. The healthcheck sends a local HTTP GET to `/api/status`
and requires HTTP 200; it checks server availability, not market-data freshness.
Bash's TCP support is used because the published image has no curl or wget.

Update with `docker compose pull && docker compose up -d`. Stopping with
`docker compose down` preserves the volume. `docker compose down --volumes` deletes
it. Stop other instances before sharing the volume or port.

## Update notices

Enable **Check for updates** under **Status → Updates** in the terminal. The setting
is saved in browser storage, separately from server configuration and journals.

Update checks are opt-in and off by default. When on, the web terminal itself, not
the server, asks `https://api.github.com/repos/38st/openport/releases/latest` at most
once a day. It compares that release with the running version from `GET /api/status`,
and shows a small dismissible notice with a link to the release when it is newer.
Nothing else is sent. A network failure stays silent.

The request has no application credentials, referrer, running version, market data
or account data. Attempts, including failures, are cached for 24 hours across tabs
and reloads at the same browser origin. Clearing browser storage clears that cache
and turns checks off. The browser must support persistent storage and Web Locks;
otherwise checks are skipped. Localhost and HTTPS support the secure browser context
needed for locks. Leaving the terminal open permits another check once the day expires.
Dismissal hides that release, including after reload; a later release can appear.
No update is installed automatically. Older servers without `status.version` are
supported and cause no release request.

## Command-line validation

`openportd` defaults to Cboe delayed data for SPX, SPY, QQQ, IWM and DIA, as does
the Docker image. `--symbols` replaces that subscription. At revision 3 and later,
the demo feed uses SPX, SPY, QQQ, XSP, NDX, RUT and VIX.

Ports must be integers from 1 through 65,535. Poll intervals and probe timeout
seconds must be positive integers. Expiry counts must be nonnegative integers;
strike windows must be finite numbers in [0, 1]. Numeric suffixes, unknown flags,
and unknown provider option keys are errors. Startup failures print a reason and
exit with code 2, with the usage on stderr. `openportd --help` prints the usage to
stdout and exits 0; `openportd --version` prints the version and exits;
`openportd --compact-journals` rewrites older paper journals and exits 1 if any was
left as it was (see [compacting](paper-trading.md#compacting-older-journals)).

Help derives plan IDs and prerequisites from the preset catalogue, including
static, locking, two-step and funded-scaling plans ([full list](configuration.md#command-line-flags)).
`--plan` seeds any preset directly in a new or empty journal, including verification
and funded steps. A recovered journal keeps its saved rules; `--plan` does not change
them. Prerequisites marked `after X` in help apply to HTTP account creation and resets.
HTTP account creation cannot seed locked presets; HTTP resets enforce prerequisites
with `PLAN_LOCKED`. Pass the prerequisite in the same account before resetting into
the next step.

Supported `openportd --option KEY=VALUE` keys:

| Provider | Keys |
| --- | --- |
| Cboe | `poll_seconds` |
| Massive | `poll_seconds`, `base_url` |
| ThetaData | `poll_seconds`, `base_url` |
| Tradier | `sandbox=true` or `false` (default), `poll_seconds` (default: calculated from the cycle request count), `option_size_unit=contracts` (default), `hundreds` or `unknown` |
| tastytrade | `sandbox=false` only; `dxlink_time_unit=milliseconds` (default) or `unknown` |
| Databento | `quotes=cbbo-1s` or `quotes=cmbp-1`, `trades=on` or `trades=off` |
| Replay | `file=PATH` (required), `speed=1`, `10`, `60` or `max`, `loop=on` or `off` |
| Demo | `days=ID,ID,...`, `speed=1`, `2`, `5`, `10`, `30`, `60`, `120` or `300` (default 1), `revision=1..8` (default 8) |

Databento parent subscriptions stream the entire option chain upstream. Both
CLIs reject nonzero `--expiries` or `--window` with that provider; those filters
cannot reduce upstream traffic.

## Broker market data

Both broker adapters are data only. Neither reads accounts nor sends orders.
Only the documented market-data paths and tastytrade's OAuth refresh POST are
allowed; broker requests do not follow redirects. Credentials are environment
variables, not `--option` values or command-line flags:

| Provider | Environment |
| --- | --- |
| Tradier | `TRADIER_ACCESS_TOKEN` |
| tastytrade | `TASTYTRADE_CLIENT_SECRET`, `TASTYTRADE_REFRESH_TOKEN`; `TASTYTRADE_CLIENT_ID` is optional and must match the grant if supplied |

Set those variables in your shell or service environment, then run, for example:

```sh
./build/apps/openportd --provider tradier --symbols SPX,SPY --expiries 2 --window 0.1
./build/apps/openportd --provider tradier --symbols SPY --option sandbox=true
./build/apps/openportd --provider tastytrade --symbols SPX,SPY --expiries 2 --window 0.1
```

Tradier production uses `https://api.tradier.com/v1`; sandbox uses
`https://sandbox.tradier.com/v1` and reports a 15-minute delay. Production equity
and option quotes are real-time. Tradier's sources conflict on index freshness,
and index values may be derived. Index subscriptions report `Stale` with timing
unconfirmed rather than promising real-time index prices. There is no separate
unknown-timing state in the event vocabulary.

Each Tradier cycle requests expirations with `includeAllRoots=true`, an option
lookup, the underlying quote, and chains with `greeks=true` for selected root/date
pairs. Looking up roots avoids assuming an SPX chain contains SPXW. The nearest
expiry and strike filters limit new definitions; previously defined contracts
keep receiving updates when spot moves. All required responses must succeed
before a complete snapshot is published.

The default poll interval is the cycle's request count × 60 / 120 seconds, rounded
up (60 requests/minute in sandbox), across all subscribed underlyings. Discovery
adjusts the estimate as each underlying is fetched. An explicit `--poll-seconds`
is checked after discovery and reports an error if too short. A rolling request
budget also paces cycles larger than one minute and honours available/used/allowed
headers. Zero availability waits at least a minute. Expiry headers have unconfirmed
units; plausible epoch seconds or milliseconds may extend that wait. HTTP 429 and
quota/rate-limit messages cause exponential waits capped at five minutes; explicit
reset or Retry-After headers can extend the wait. Budgets are local to one adapter;
headers account for other users of the same token.

Tradier's field reference describes quote sizes as "in hundreds", which fits its
stock quotes; the sizes in its option-chain examples read as contracts, the unit OPRA
publishes, so option sizes are taken as contracts. `--option option_size_unit=hundreds`
multiplies them by 100, and `unknown` withholds them, which stops paper fills that need
displayed size. No sizes are invented for missing fields.
IV, vega, theta and rho scaling are unconfirmed and remain NaN; delta and gamma are
mapped as price derivatives. The unzoned Greek update time stays unknown. Quote
clocks are milliseconds; conflicting seconds-shaped examples are rejected when
they would give a priced side an unknown clock. This adapter does not yet emit session volume; cumulative volume is not
represented as trades.

tastytrade requires a funded production account and OAuth credentials created in
the broker's own application. `sandbox=true` is rejected because the sandbox has
no market data. The adapter uses one TLS WebSocket per provider, verifies its
certificate and host, and sends `openport/<version>` on HTTP and WebSocket requests.
OAuth access tokens refresh before `expires_in` (15 minutes if absent). Quote tokens
refresh before their explicit expiry or after 23 hours when expiry is absent.
Quote tokens are cached across reconnects and re-fetched if rejected.
Chains refresh on an hourly reconnect; all subscriptions are restored after a
connection failure. Missing or expired instruments are unsubscribed after a
complete chain refresh, and their old quotes are cleared with an unknown timestamp.
Eight consecutive failed connection attempts stop the feed;
a minute of connected service resets that count. Retry waits grow to 60 seconds.
Shutdown interrupts waits and I/O; system DNS remains the exception described above.

DXLink requests Quote, Greeks, Summary, Trade and Profile. Underlying/index streamer
symbols come from instrument responses, and option streamer symbols come from
nested chains. Strike filtering waits for an actual underlying price; it does not
invent spot. A subscription is an event type plus symbol: four per option, three
per underlying. The adapter refuses more than 25,000, sends at most 10,000 changes
per minute and opens at most five sessions in one process. Other programs using the
same credentials also count toward the broker's five-session limit. Large initial
subscriptions therefore take multiple minutes to establish.

DXLink event times follow dxFeed's event model, where a Quote's `bidTime` and
`askTime` are milliseconds since the epoch, so they become market times.
`--option dxlink_time_unit=unknown` leaves market times at zero instead (health then
reports `Stale` and paper fills wait), for a feed whose units turn out otherwise; the
adapter has not yet run against a live account. IV, vega, theta and rho remain NaN because their scaling is
unconfirmed; delta and gamma are mapped. Summary supplies open interest with an
unknown timestamp. Trade is a last-trade snapshot, not a distinct execution tape;
it supplies underlying last prices but emits no option trades. Underlying events
are not requested because entitlement to that DXLink event type is unverified;
Quote and Trade supply the underlying instead. Profile is requested but currently
has no corresponding normalised event. Streaming emits no `SnapshotComplete`.

Both adapters are tested with generated fixtures shaped like broker documentation,
not live accounts. Empty response shapes, index symbols, entitlement and the noted
units still need account validation. No new journal or recording fields are added.

## Chain volume and liquidity

`md::OptionVolume` carries cumulative traded contracts for the trading date of its
market timestamp. It is latest-value state in the queue, so dropped trades do not
lose an already computed total. `ChainBook` retains the value and timestamp;
analytics omits totals from another trading date. Chain sides expose `volume` as
a number or null, including a reported zero. Expiry and underlying coverage count
contracts with current-date volume. Unknown values do not become zero.

Cboe publishes its row volume. Massive uses `day.volume` with `day.last_updated`;
without that clock the count stays unknown. ThetaData maps volume only if a row
supplies it; the current saved quote, IV and OI samples have none. Databento sums
received trade sizes per contract, resetting at the trading-date roll (including
the evening session). Its total covers trades received since connection; it does
not backfill earlier trades, repair reconnect gaps or apply trade corrections.
With `trades=off`, volume is unknown. Tradier and tastytrade do not yet emit volume.

The chain shows volume, `(ask - bid) / ((ask + bid) / 2)` and a liquidity cue per
side. Hover volume for volume/OI; zero or unknown OI leaves the ratio unknown.
The Liquidity columns button hides these three columns, alongside the existing
Quotes/Greeks switch. Smile and Exposure keep their existing displays.

The thresholds live in `web/src/lib/liquidity.ts`. No valid two-sided quote
(missing, crossed or no positive bid) is **none**. A spread above 20% of mid, or
both volume below 100 and OI below 500, is **thin**. Otherwise the cue is **good**.
The activity check requires both numbers; unknown activity is labelled unknown
in the detail and is not evidence of low activity. Good describes these checks,
not a guarantee of available size or a fill. Locked positive quotes have zero spread.
Tickets warn on thin or absent liquidity for each leg without blocking submission.
Market tickets show the requested contracts and displayed executable-side size
per leg, and state that fills are simulated against displayed quotes and sizes only.

Unlike the cue, the server's `executable` flag on each chain side is the simulator's
own test: a positive, uncrossed bid and ask with a whole contract of displayed size on
each side, as `trading::valid_quote` takes the quote the Desk offers. A quote that is
not executable shows faded on the chain, with `quote_issue` (`no_quote`, `no_bid`,
`no_ask`, `crossed` or `zero_size`) in its tooltip, and tickets name the problem.
Paper orders share a quote's displayed size until a new observation refreshes it; the
portfolio's `liquidity_used` lists the current quotes the account's orders have taken
some of, which the chain marks as "N left" and the ticket shows beside the size, and the
preview's per-leg `liquidity` says how much of the order can fill on the current quote.

## Recording and replay

### Importing a historical day

```sh
openportd --import-day databento --date 2026-09-22 --symbols SPX,SPY \
  --expiries 2 --window 0.10 --out ./recordings
openportd --import-day thetadata --date 2026-09-22 --symbols SPY \
  --expiries 2 --window 0.10 --out ./recordings
```

Import reads one completed trading date, writes a `.oprec`, and exits without
starting a feed, HTTP server or paper account. `--out` defaults to `./recordings`.
Point the server's `--record-dir` at the same directory. Replay lists the source
provider and `imported`; replay runs use the usual journals and `--verify-run`.
Databento reads `DATABENTO_API_KEY`. ThetaData uses the logged-in Theta Terminal
v3 at `http://127.0.0.1:25503`, just as the live adapter does. There are no key flags.
Historical access needs the provider's entitlement; Databento requests can incur
usage charges. These importers follow the documented APIs, are tested against
saved response shapes with generated values, and have not yet been run live with a key.

The calendar rejects holidays, weekends, today and future dates before making
requests. Dates before 2022 are refused because the built-in holiday calendar is
incomplete there. Product sessions use `md::trading_session`, including GTH assigned
to the requested trading date over a holiday, regular hours, early closes and curb.
Coverage depends on the provider and subscription. A root without overnight data
has no invented overnight quotes. Calendar overrides are not fetched during import.
Databento imports require 2023-03-28 or later, when OPRA's one-second CBBO history
became available; the older minute-sampled dataset is not used.

The importers merge fifteen-second windows in original capture-time order when
available, otherwise in event-time order, preserving source order at equal times.
A contract's definition precedes its first event.
Quotes keep their provider clock; daily OI keeps its original observation time.
Databento trades keep `ts_event` and use `ts_recv` for ordering, including late prints
within a window and across windows. OI is withheld until both its observation and
capture times have been reached. Receipt times are the monotonic market-time watermark, at least the current window's
start, not download time. Cumulative volume keeps a monotonic market clock when a
late trade's event time goes backwards. Historical
quotes are an event stream, with replay's fixed market-second batches; imports do
not claim that tick history was a complete live polling snapshot.

Zero expiry and window limits mean unrestricted, as for live feeds. Nearest expiry
dates are selected across all roots, keeping AM and PM contracts distinct. The
shared chain filter uses the reported underlying price at each market timestamp, or an inferred
near-expiry parity forward when the provider supplies no underlying. A previously
admitted contract continues receiving updates when prices move outside the window.
A nonzero window with neither a price nor a parity estimate admits no new contracts.
Selection never uses a later price from the same download window.
Historical Databento filters are local; its parent queries still fetch whole chains.

Databento imports OPRA definitions, one-second consolidated BBO, trades and OI
statistics through the linked Historical C++ client. The preceding business date's
OI initializes the overnight session until a newer observation arrives. OPRA supplies no underlying
prices or vendor Greeks here. ThetaData uses expiration and strike lists, historical
one-second option quotes, tick trades and daily OI, plus index prices or stock quotes
on their own timestamps. Missing OI remains missing. Session volume sums reported
trades; it is not invented from quote sizes. Neither importer supplies official
underlying closes or vendor Greeks.

Requests are serial, with at least 100 ms between starts. ThetaData retries HTTP
429 at most three times, honouring integer `Retry-After` values up to 60 seconds;
longer or malformed values stop the import. Other errors stop it. Databento streams
responses through SDK callbacks and does not retry a failed billable request.
The merger caps a window at one million events and the chain at 500,000 contracts.
Databento pending OI is capped at one million observations. ThetaData responses are
capped at 32 MiB and 250,000 rows after the HTTP client's bounded download. Exceeding
a cap fails explicitly; reduce symbols or, for ThetaData, expiries and retry. Memory grows with the
chain and one window, not a day's quotes. ThetaData requests each selected expiry
separately, so unrestricted chains can take many requests. Progress reports completed
windows and written events. Disk space must accommodate the full recording.

The file is private (mode 0600) and published under
`PROVIDER-YYYY-MM-DD-imported.oprec` only after every requested symbol has quotes
and the clean end is written. Existing files are never overwritten. Errors remove
the temporary import; a process crash can leave a hidden `.import-*` directory,
which can be removed after that importer has stopped. No partial import is listed
in Replay. All data stays on the user's machine. Never bundle, commit or upload
provider responses or recordings. Provider licensing still applies.

### Recording a live feed

Record any provider before the engine's coalescing queue:

```sh
./build/apps/openportd --provider cboe --symbols SPX,SPY --record session.oprec
./build/apps/openportd --provider replay --symbols SPX \
  --option file=session.oprec --option speed=10 --option loop=off
./build/apps/openport-probe replay SPX --option file=session.oprec --option speed=max --analyze
```

Both CLIs accept `--record FILE`; the probe also accepts repeated `--option KEY=VALUE`.
`openportd --record-dir DIR` records each run to its own file there, named by provider
and UTC start time (`cboe-2026-09-23T184820Z.oprec`), creating the directory if needed.

Full chains are large. Cboe's first poll of the whole SPX and SPY chains, about 164,000
events (definitions, quotes, open interest and Greeks), recorded as 2.7 MB, about 16
bytes an event. While the market trades, most quotes change on every 15-second poll,
so a full day of SPX, SPY and QQQ can run to gigabytes; `--expiries` and `--window`
narrow what is recorded.

Files are created exclusively with mode 0600: an existing pathname, including a
symlink, fails at startup and is never overwritten. For `--record FILE`, parent
directories must exist. The original provider name, capabilities, subscription and
engine start wall time are saved; API keys and provider configuration options are
not stored. Provider status text is recorded verbatim, so recordings inherit
anything a provider puts in those messages.

Replay defaults to `speed=1` and `loop=off`. The first event starts immediately;
subsequent receipt-time gaps use absolute monotonic deadlines, divided by the speed
(1, 10 or 60 from the command line; 1, 2, 5, 10, 30, 60, 120 or 300 in the terminal),
carrying fractional nanoseconds so rounding does not accumulate. `max` skips waits.
A running replay can change speed, rescaling the wait in progress; pause, where the
paused time does not count against the gap; and skip, which plays the next event at
once. A backward receipt-clock step contributes zero delay; positive subsequent
gaps still count. Waiting is interruptible at every speed. Filtering keeps gaps
across omitted symbols and retains provider-wide status. Unknown symbols fail
synchronously and list the subscription in the header; the header declares what
was subscribed, not whether every symbol ultimately produced quotes. New expiry
or strike-window filters are rejected: the file already reflects the recording's
original filters.

The provider is named `replay (<original provider>)` and exposes the original
capabilities. Every selected event keeps its original fields and market timestamp;
analytics uses the recorded market time. A clean end adds `Stopped`, allowing the
engine to finish its analytics and keep the result visible. Trading replays require
`loop=off`; repeat a day as a fresh run with a new journal. Probe-only looping rewinds the same open file, resends definitions and repeats the recorded events
and their gaps, with no extra gap between passes. Empty recordings stop even with
loop enabled. A truncated recording stops with its recovery diagnostic and never
loops; malformed data produces `Error`. Neither the daemon nor its web terminal
exits automatically at replay EOF. The probe retains its readiness/timeout rules
and stops waiting at EOF if it has not become ready.

**Format v4.** The demo feed (`--provider demo`, including explicit `--option days=…`)
and generated scenarios with stalls or halts use version 4. Other live providers
keep version 2. Re-recording a replay uses version 4 only when its source header
enables market controls; ordinary replay sources keep version 2. Writers choose
the format from the provider before recording begins. A heartbeat or halt reaching
a sink without `market_controls` stops recording with a visible error instead of
writing an unsupported tag; downstream delivery continues.
Readers accept versions 1–4; older builds refuse v4. Version 4 retains the v3 header
and adds tag 10 (`SnapshotHeartbeat`: underlying string, timestamp i64), and tag 11
(`TradingHalt`: start and exclusive end i64 timestamps). A heartbeat is a batch
boundary advancing market time without confirming quotes. Halt intervals are
market-wide. Ordinary generated scenarios still write version 2; imports use v3. Version 3 appends one `u8` boolean, `imported`,
to the header. Older headers default it to false. Event tags are unchanged. Version 2
adds tag 9 for option volume. The header and tags 0–8 are unchanged, so v1 files
play as before, with volume unknown.

All integers are fixed-width little-endian; signed integers use
two's complement. Strings are a `u32` byte length followed by those bytes (no
terminator or encoding conversion). Doubles are IEEE-754 binary64 bits, preserving
NaN payloads, infinities, subnormals and signed zero. There is no native struct
padding, locale-dependent text, or lossy numeric conversion.

The uncompressed prefix is eight magic bytes `OPREC\r\n\0`, a `u32` version
(2 for ordinary live/scenario/replay recordings, 3 for imports, 4 for demo-feed and
market-control recordings), and a `u32` header-payload length. The payload, in order, is:

| Field | Encoding |
| --- | --- |
| Provider | String |
| `realtime`, `realtime_plan_dependent` | Two `u8` booleans, 0 or 1 |
| Poll interval, delay | Two `i64` counts of seconds |
| Quotes, trades, open interest, vendor Greeks, history | Five `u8` booleans |
| Subscription | `u32` symbol count, then strings, `i32` max expiries, binary64 strike window |
| Start wall time | `i64` Unix nanoseconds |
| Imported (v3 and later) | `u8` boolean; imported start time is the first scheduled session's market time |

The rest is concatenated independent zstd frames (level 1, content checksum).
Their decompressed contents are a stream of `u32` record-payload lengths followed
by payloads. A payload begins with `i64` local receipt Unix nanoseconds, then a
`u8` tag and the fields below. `id` is `u32`, `ts` is `i64` Unix nanoseconds,
prices/sizes/Greeks are binary64, and strings use the encoding above.

| Tag | Event | Fields, in order |
| --- | --- | --- |
| 0 | Contract definition | id; root; underlying; expiry year, month, day (three `i32`); strike; type (`u8`: call=0, put=1); style (`u8`: European=0, American=1); settlement (`u8`: AM=0, PM=1); multiplier; standard (`u8` boolean) |
| 1 | Option quote | id, ts, bid, ask, bid size, ask size |
| 2 | Option trade | id, ts, price, size |
| 3 | Open interest | id, ts, contracts |
| 4 | Vendor Greeks | id, ts, iv, delta, gamma, vega, theta, rho |
| 5 | Underlying quote | symbol, ts, bid, ask, last |
| 6 | Provider status | ts; state (`u8`: Connecting=0, Live=1, Delayed=2, Stale=3, Error=4, Stopped=5); message; underlying |
| 7 | Underlying official close | symbol; ts; trading year, month, day (three `i32`); price |
| 8 | Snapshot complete | underlying; ts |
| 9 | Option volume (v2 and later) | id, ts, cumulative contracts |
| 10 | Snapshot heartbeat (v4) | underlying; ts; batch boundary without quote confirmation |
| 11 | Trading halt (v4) | ts; end (exclusive); both `i64` nanosecond timestamps |

A zero record length is the clean-end marker, written only after all publishers
stop and all accepted events drain. It must finish the final frame; bytes after
that frame are rejected. Headers and record payloads are limited to 1 MiB;
decoder zstd windows are limited to 16 MiB. Unknown versions/tags, invalid enums,
invalid lengths, extra fields and corrupt/checksum-failed frames are explicit
errors. The reader streams with bounded buffers rather than loading the file.
Physical EOF without a clean marker, including a torn final compressed frame,
returns all recoverable complete records and a `recording truncated` diagnostic.
A partial final record is discarded; a recovered incomplete frame cannot have
its checksum verified. Even a crash exactly between frames is reported as
truncation rather than mistaken for successful shutdown.

**Ordering, durability and limits.** One publisher lock establishes the same
order for receipt timestamps, recording and downstream delivery, including
concurrent publishers. Recording never coalesces events or drops trades.
Serialization appends to a reusable buffer. A worker seals/writes a frame every
second (including idle feeds) or at 4 MiB of raw records, whichever comes first;
a frame can exceed that threshold by one record. Publications backpressure while
a sealed frame is being compressed/written, keeping at most one outstanding
frame. A process crash can therefore lose at most that outstanding frame, with
partial recovery possible. Slow or blocked storage stalls publication instead of
silently losing data. These are process-crash guarantees for completed OS writes,
not power-loss durability: there is no per-frame `fsync`, and OS scheduling or
blocked storage can delay a flush. Recording memory stays bounded independent of
session duration; the downstream book and replay's contract-to-symbol map still
grow with the number of defined contracts.

A serialization, write or close failure stops recording, reports its explicit
reason through provider status, and remains an `Error` in engine status even after
healthy feed updates. Live analytics continues; the daemon logs the reason and
returns 1 on shutdown. The saved file is a recoverable prefix, not a complete
session. Startup failures return 2. Clean shutdown stops the provider first,
drains/closes the recording, and drains the engine before its final analytics.
The daemon prints accepted events, bytes, average sink nanoseconds/event and
bytes/event on shutdown. Counters include accepted but not durable events if
storage failed.

Trading replays bypass the engine's latest-value queue. The replay driver applies
each market time's complete snapshots, every underlying's together, or fixed
one-second market-time batches for streams and old recordings without snapshot
markers. Analytics and trading settle at each boundary.
Speed changes, fast-forward and consumer scheduling cannot change those batches.
Live feeds and the probe retain their throughput-oriented coalescing queues.
Replay stop interrupts pacing and consumer waits; it cannot interrupt a blocked OS
file read. [Architecture](architecture.md#reproducible-runs) describes the two drivers.

**Measured synthetic cost.** On this Apple M2 Max, Release/Apple Clang, the
`RecordingPerformance.SyntheticChain` test publishes a 30,000-contract SPXW chain
(60 expiries × 250 strikes × two sides), then 30 quotes per contract: 930,001
events including spot. A measured run wrote 3,108,528 bytes (2.96 MiB), or **3.342
bytes/event**, including header, checksums and end marker. Publisher sink time
was **231 ns/event**, including queue delivery and backpressure; the complete
publish-and-close run was 262 ns/event versus 18 ns/event for the same events
through a queue alone, approximately **244 ns/event added**. This regular,
repetitive synthetic chain compresses well; real market prices, timestamps,
trade mixes and status strings can require substantially more space. Receipt
timestamps come from the real local clock, so compressed sizes vary between
runs. These averages include compression/write stalls but are not tail latency
or stable-storage benchmarks.

Reproduce without network access:

```sh
./build/tests/openport_tests --gtest_filter=RecordingPerformance.SyntheticChain
```

Recording uses the existing system zstd dependency, including when the Databento
adapter is disabled; no additional Docker packages are required.

### Replaying in the terminal

`GET /api/replay` lists recordings from `--record-dir` (default
`~/.openport/recordings`), the scenario library (`demos`) and finished runs
(`history`). A replay runs beside the live feed with its own engine, account and
chart history. The terminal routes `/api/X` to `/api/replay/X` while showing it;
“Back to live” returns to the live accounts, which keep running meanwhile.
Recording entries and active replay state expose `imported`, false for older
recordings. Imported days appear beside live recordings with their provider name.

`POST /api/replay` starts `{file: NAME}` or `{scenario: ID}`; `demo: ID` and
`demo: true` remain accepted, and `scenario: true`, like `demo: true`, plays the
default scenario. Optional fields are `plan` (default `practice`),
`speed` (0, 1, 2, 5, 10, 30, 60, 120 or 300), `start_at` (`HH:MM[:SS]` New York, or a
date and time such as `2026-09-17T10:30`) and `paused`. Scenarios also accept a trading `date` and `seed`: omit it for a fresh
seed, use `"scenario"` for the file's seed, or supply a uint64 decimal string.
`PUT` changes speed or pause, skips a gap, or advances through `until`; `DELETE` stops playback.

A drill fast-forwards to the complete receipt group at or after `start_at`, then
waits for the engine's book, analytics, candles and account publication. The tick
reports progress and writes return `REPLAY_FAST_FORWARD` until ready. `paused:
true` then permits trading at that prepared state. Evening overnight times are
on the calendar date before the session date; morning times are on the session
date. Recorded feeds retain their original delay and market timestamps.

The demo market has twenty built-in scenarios, compiled from `scenarios/*.json`,
plus `--scenario-dir` additions and overrides. No built-in files need installing
or locating at runtime. They include the
five original days, gaps, crushes, pins and reversals, and four runs of several
sessions: held through the curb and overnight sessions, over a weekend, across three
days and from an overnight session into the open. A run of several sessions plays
on one account, with the rollover, end-of-day ratchet, GTC carry, gaps, early
assignment and dividends between them, and its state lists its `sessions`. All are simulations, never
historical reconstructions. Status keeps `provider.simulated`; the terminal shows
the scenario and seed with the simulated label. Generated recordings are cached by
scenario, date and seed, with four completed entries retained. Generation uses
fixed version 1, at output revision 8 (see [scenarios](scenarios.md)); unsupported
versions are rejected. Optional [stress fields](scenarios.md#events) author abnormal
books, quote stalls, market-wide halt windows, previous-close references and wider
strike ranges. `stress-rehearsal` is a built-in drill. At revision 5, regular event times and window
endpoints scale from the full session into early closes, rounded down to minutes
(minimum one-minute windows); `afternoon-waterfall` works on 2026-11-27. Invalid
event-time diagnostics name the event index, type and authored time.

Each run's account journal is retained in `replays/` beside `--paper-journal`, on
the chosen plan or practice. EOF and stopped runs are read-only in the terminal's
Journal and Dashboard. Their stats stay separate from live accounts. A confirmed
delete removes a finished journal. The existing repair and compaction commands
include replay journals. `--no-paper` creates no replay journals; disabled write
access allows history reads but refuses starts, controls and deletes.

History listings cache each journal's result, P&L, plan, time and valuation
completeness until its modified time or size changes. These summaries have no
entry limit; opening full archives uses a separate cache of sixteen accounts.

Recording names must be plain file names inside the configured directory. Without
an active replay, `/api/replay/X` returns `NO_REPLAY`; a file that cannot be read
returns `REPLAY_FAILED`. Archive routes remain readable after stopping and after
restart. See [scenarios](scenarios.md) for the JSON format, events, timing semantics,
seed guarantees and history routes.

### Lockstep stepping

`PUT /api/replay {"until":"10:30:00"}` runs as fast as possible through that New York
time on the session date, then pauses. A date and time such as `2026-09-17T10:30`
(seconds optional) is New York's unless it ends in `Z` or a UTC offset.
For overnight sessions, evening times refer to the preceding calendar date. In a
scenario of several sessions a bare time is its next occurrence at or after the replay's
time in the run's sessions, and a time that falls between them returns 400; give a date
and time to step to a later day. `until`
is the only field in that control request. Streams require a whole-second target;
a fractional ISO target is rejected rather than reporting an unsettled partial batch.
It cannot move backwards. A target past the recording's last batch returns 400
before anything plays, naming where the recording ends, so the replay, its working
orders and its evaluation stay where they were. The response waits for every
complete input boundary through the target, analytics, trading and publication, and
includes `settled_through` both at the top level and in replay state. A market time's snapshots are applied as one
complete batch. `until` also accepts `+15s`, `+5m`, `+1h` (positive whole units
relative to max(settled_through, market_time)) and `next` (the next complete snapshot
or market-second batch). Relative targets past EOF are refused before playing.

A paused skip is explicit: `PUT {"skip":true}` replies that it is queued for the
next resume and state/ticks expose `skip_pending:true`. `PUT {"skip":false}` cancels it.

```sh
curl -X PUT http://localhost:8080/api/replay \
  -H 'Content-Type: application/json' \
  -d '{"until":"10:30"}'
```

Add the configured write token as usual. Commands submitted after the response use
the paused market time. While the step plays, replay state reports `stepping: true`
(`fast_forwarding` stays false: it means the start state is still being prepared),
and orders and other writes to the replay account return 409 `REPLAY_STEPPING`,
since they would land at whatever market time the step had reached. Other starts,
controls and history deletes wait their turn on the replay host's own thread, except
`PUT {"abort":true}`, `PUT {"paused":true}` and `DELETE /api/replay`, which interrupt
the step at its next complete batch and wait for analytics, account and journal sync.
The interrupted step returns `aborted:true` with its actual `settled_through`; writes
stay `REPLAY_STEPPING` until that barrier completes. Invalid controls do not interrupt; listings, reads, WebSocket ticks and the live accounts keep answering. Stepping through intermediate times adds no account
transactions; with the same commands at the same times it gives the same journal
as continuous playback. A step plays unpaced and leaves the replay paused; resuming
waits one receipt-time gap from the resume, however many steps came before.
`PUT {"play_until":"14:30:15","speed":60}` watches the same advance at a chosen
speed. It returns immediately; state and ticks show `pause_at` until all complete
batches through the target settle and playback pauses, at the same market time as
`until`. It accepts the same absolute, relative and `next` targets. Omit `speed` to
keep the current speed (including Max). A manual speed change, pause/resume, abort,
`until` or stop clears it; another `play_until` replaces it. Skip retains it. Invalid
or past-EOF targets change neither speed nor pause state. Commands during watched
playback are allowed; wait for `paused:true` and `pause_at:null` to trade at the target.
A target between batches settles through T without consuming the following batch.

`start_at` retains its receipt-time semantics for delayed recordings; `until` uses
market time.

AM positions in the real-time demo and in driver-5 replays settle on the first
underlying print at or after 09:30 ET of their expiry date. These are approximations
of the official special opening quotation, labeled `demo_opening_print`,
`scenario_opening_print` or `recorded_opening_print`. Live real providers still
require an imported AM value. Waiting positions say `settle_by: opening_print`
or `manual` accordingly. References and provenance remain available through
`GET /api/settlements`, its replay mirror, and finished-run history.

Finished, stopped and replaced replay runs save `journal: {transactions, head, bytes}`
in their `.json` sidecar. Resuming an interrupted run replaces that final checkpoint
when it ends again. Older sidecars without a checkpoint remain readable.
`GET /api/replay/history` (also `GET /api/replay`'s `history`) and archived account
reads expose `journal_found` alongside the checkpoint. `torn: true`, `bytes_cut`
and `integrity_message` identify an incomplete final line and point to
`--repair-journals` with the server stopped. `mismatch` and `truncated` flag a changed
head/count/size, including records removed at a complete line boundary; the message
names expected and found counts, heads and bytes for the fields that differ.
Summaries are cached by journal size and
modification time. Repairing a torn line does not restore missing records or replace
the saved final checkpoint.

### Restarting with the same commands

`POST /api/replay {"restart":"RUN_ID","at":"2026-09-17T10:30:15"}` creates a new
journal with the source's commands through that market time, inclusive, then pauses.
It keeps the source's settings and inputs, and verifies the copied prefix while
rebuilding the market state. `at` omitted uses the source's start; optional `speed`
overrides the source speed and `paused:false` continues afterwards. The source may
be active, stopped or finished; its journal is kept. State and history carry the
optional `restarted_from:{id,at}`. See [restarting a drill](scenarios.md#restarting-a-drill-with-its-commands)
for time bounds and refusal conditions. Ordinary starts and playback controls keep
their existing journal bytes; restart creates a byte-identical prefix on driver 6.

### Verifying a run

The terminal's **Finished runs → Verify** starts the same verifier in the background:
`POST /api/replay/history/ID/verify` returns 202 and `status: "running"`.
It needs replay or admin scope and enabled writes; disabled access returns
403 `WRITE_DISABLED`. An active run returns 409 `REPLAY_RUNNING`. Only one
verification runs per server; another start returns 409 `VERIFICATION_RUNNING`.
The target cannot be deleted or resumed until verification ends. Other replay and
live engines keep running, with their own calendars and clocks. Verification opens
no provider connection. Server shutdown cancels between replay operations and joins
the worker (an in-progress input read or scenario generation completes first).

`GET /api/replay/history/ID/verify` needs read access and returns `status`
(`idle`, `running`, `passed`, `failed`), `message`, and progress when available.
Terminal results include `transactions`, decimal-string `equity`, `head`, verifying
`build`, final market `time`, wall-clock `finished_at`, and `run` identity/inputs.
Equity/head may be null on failure. The latest result is saved in the metadata
sidecar and included as `verification` in history/account reads. Changing journal
size or modification time invalidates it to `idle`; a verification interrupted by
server shutdown/restart can be started again.

GET the same route with `?format=receipt` to download a small JSON receipt of a
passed or failed result (409 `VERIFICATION_UNAVAILABLE` before one exists or after
invalidation). It names the run and plan, recording SHA-256 or scenario source hash,
id, revision, seed and date, final equity/count/head, build and both times, without
absolute paths. This is an unsigned local verification report, not independently
authenticated proof; share the journal and original inputs for independent checking.

The command-line equivalent is:

```sh
openportd --verify-run /path/to/replays/run.jsonl
```

This mode opens no provider connection or HTTP listener and does not touch the main
paper account. It verifies the journal chain, opens the original recording or
regenerates the scenario, repeats its boundaries and commands, and checks every
transaction hash, final equity and head hash. Exit 0 means a match. Exit 1 names the
first differing transaction, or reports a damaged journal or missing/changed input.
Exit 2 is a command-line usage/startup error. A size-based cost estimate is printed
first on stderr. Every verification report starts with
the run id/file, scenario or recording and date, scenario seed/revision, and plan.
When the sidecar has a final checkpoint, the report prints its head/count and checks
that the journal agrees; a cleanly shortened journal fails instead of verifying only
the remaining prefix. No checkpoint means legacy prefix verification.
Replay driver 5 adds AM opening-print settlement. Verification
of drivers 1–4 keeps their manual AM behavior and original journal bytes. Driver 5
changes the start input for every new run and adds settlement transactions when an
AM position is held into an expiry opening print. Resuming requires the current driver (6, below);
older runs remain readable and verifiable. The scenario generator revision is 8; older recorded revisions regenerate as before.
A stopped run verifies through its recorded prefix; it need not have reached EOF.
So does a run a crash cut off, whichever record its journal ends at: each input and
each transaction is its own append, and new runs record a command's input before the
transactions it causes. When the journal ends after a batch's transactions but before
the boundary input that follows them, the recording's next batch must reproduce them.
The report then adds that the journal ends part way through its last operation. Runs
recorded before this recorded a command's input after its transactions, so one cut
between them names the first transaction no recorded input explains.

The journal carries recording name, absolute path, byte size and SHA-256, or scenario
id, source hash, generator version and output revision, date and seed. Built-ins are checked against the
embedded source; user scenarios are reopened at their recorded path. Keep original
recordings and user scenario files there. Generated cache files are unnecessary for
scenario verification. The initial plan, analytics settings, dividends (including the
revision 4 scenario calendar, overridden by explicit session entries), calendar
and command times are recorded too. Older account journals still load but have no
run inputs to verify. New replay runs require an unused journal path.

Scaling plans are opt-in. Their `scaling` rule and `Evaluation::scaling_limit`
state are omitted at disabled defaults; existing journals retain their bytes and
re-verify without a driver or scenario revision change. Recovery keeps the active
session limit, even when current profit would select another step. Only rollover
changes it and emits a `scaling_limit` event.

Funded account-size scaling (F63) adds optional `size_scaling` rules and evaluation
review state, omitted when disabled with the existing `added_field` compatibility
pattern. One `account_scaled` event records each capital credit with old/new size,
increase and review dates. The state delta records cash, size, lifted floor/peak,
effective loss limits, original loss amounts and review/history state. Recovery
restores the same review and continues deterministically; credits never touch
realised P&L or fills. No journal schema or scenario revision change is needed.

Inside-fill and walking-limit settings are opt-in. Walk steps use market time,
including replay clock advances, and are retained as order changes. Recovery
preserves their last scheduled step. Their added journal fields are omitted when
unused, so runs from before walking limits and inside fills still verify on the new build on the same platform.

Strategy margin's share covers and Reg T straddles (F49), and their joint pairing
with verticals and bounded joint expiry-pool allocation, lower the requirement for
some books. Existing journals still load; the allocator adds no journal fields.
Buying power in fill contexts therefore differs, and `--verify-run` of affected runs recorded by earlier builds
reports the first difference. Account type, house margin and portfolio vol shock are
journaled only when not at their defaults, so other journals keep their bytes.

Driver 6 also labels automatic playbook entry/exit cancellations
`PLAYBOOK_TIME_STOP`. Drivers 1–5 still verify using their original `USER_CANCEL`
bytes. Older cancel commands omit the optional `cancel_reason` and default to
`USER_CANCEL`; existing account journals and saved reports still load. Resuming
an interrupted run requires the current driver, as before.

Authored-halt decisions use New York time, or say "for the rest
of the day" when the halt reaches the regular close (including early closes).
For example: "Trading is halted market-wide by the scenario; it resumes at 10:30 ET".
Runs keep driver 6; authored halts were not available in earlier shipped builds,
so there is no older message to preserve. There is no journal-byte change beyond
the new message on authored halts. These driver changes do not alter the selected scenario generator revision.

Live paper Auto also persists account/version forward-test windows in the optional
`forward_tests` key of `playbooks.json`. Modes resume after restart, with entry
limits and overlap read from the recovered journal and overdue time stops retried
on the first fresh market update. Replay copies omit these records. This does not
change replay driver 6, existing replay/backtest journal bytes, or reducer schema;
new live automatic commands use the existing journal encoding.

Removing an underlying override takes effect immediately when the common cap is
tighter; a looser fallback remains queued. This needs no new driver: older runs
cannot contain underlying overrides, so dropping redundant entries changes none
of their journal bytes. New runs still record driver 6, and interrupted driver-6
runs remain resumable. Existing journals recover both active and pending overrides.

The guardrail changes of this build retain the replay driver and recover existing
journals, including stored 100-percent floors. Recorded inputs can produce different
bytes when an account resets with opening-trade progress or a discipline latch, a
standalone closing stop starts cooldown, an opening order fills in parts (its first
opening counts once), or a buy-only put exercise would sell shares short. Partial
opening orders record the journal-only `opening_counted` flag, retained across
rollover so later fills do not count the same order twice. A partial order reaching the trade limit records the journal-only
`guardrails.trade_limit_order` completion exemption. Both fields are omitted at their
defaults; older journals without them still recover. HTTP projects the daily count
and latched reasons rather than these reducer bookkeeping fields.
Exact re-verification of affected inputs requires the original build.

Standalone price-triggered closes start cooldowns only when their trigger direction
is adverse to the position being closed, including the held option's call/put direction
for underlying triggers. Combo-price triggers use the signed closing debit (a rise
is adverse). Trailing stops and bracket stop-losses still count. Recorded
profit-taking triggers may therefore produce different journal bytes from builds that
treated every price-triggered close as a stop. Journal recovery and the replay driver
are unchanged; exact re-verification of these inputs requires the original build.

Verification currently compares exact hashes on the same build/platform. Math-library
changes can alter analytic floating-point fields; see the platform qualification in
[architecture](architecture.md#reproducible-runs). Journal compaction changes hashes,
so keep the original when exact run verification is needed.

Share opening commands append new command kinds without renumbering earlier kinds.
Their optional `stock_price` input records the underlying symbol, exact micro-dollar
price and market timestamp before execution, including a first purchase on an idle
account. It is absent on all older command kinds. The close-only route keeps its
existing command and transaction bytes. Recovery uses the existing share ledger,
marks, basis, fills and annotations; no new state fields are needed. Previews are
never journaled.

## Demo feed

`openportd --provider demo` rotates the thirteen built-in regular scenarios in their
Replay listing order, one a trading date: a date plays the scenario its count of
trading dates from 2 January 2026 selects, so it plays the same one whenever the
server starts. Stress drills are opt-in: `--option days=stress-rehearsal` exercises
books, stalls and halts without changing the default rotation. `--option days=trend,chop` selects and orders built-in ids;
unknown ids, overnight sessions and empty entries are startup errors. Custom files
in `--scenario-dir` remain available on Replay. `--option speed=N` accepts 1, 2, 5,
10, 30, 60, 120 or 300; default 1. Maximum-throughput playback is not supported.
Without `--symbols`, the feed subscribes to SPX, SPY, QQQ, XSP, NDX, RUT and VIX. Other symbols and
nonzero `--expiries` or `--window` are rejected.

Revision 8 is the default: ETF underlying prices hold at the 16:00 stock close
(13:00 early) while options trade to 16:15 (13:15 early). Revision 7 adds two-sided
liquid wings; `--option revision=7` keeps those quotes with the historical post-close
ETF price movement. Revision 6 adds deterministic opening activity to session volume
so volume-capped orders can trade liquid contracts from the open. `--option revision=5`
retains the earlier zero-volume opening. At revision 5, regular event times and window endpoints scale into early
closes, rounded down to minutes. Revision 4 adds American ETF option prices and a
simulated quarterly SPY/QQQ dividend calendar, with ex-date price drops and known
payments included in option forwards. The [scenario rules](scenarios.md) define
dates, amounts and precedence. `--option revision=4` keeps that calendar without
revision 5's regular-time scaling. `--option revision=3` retains the earlier
European ETF prices and explicit session dividends only. Revision 3 adds those four
indices to every selected demo day, without changing that built-in's Replay source
or symbol list. `--option revision=2` retains the old three-symbol feed, generator
output and twelve-day rotation; `revision=1` selects the original chain. Use the
same revision and `days` options when resuming an older demo journal: live account
journals do not record their feed configuration. Scenario replay journals record
the revision and select it automatically for verification and recovery.

The default first date is the last trading date before startup's New York date.
Each next session uses the next trading date, skipping weekends and calendar holidays.
Timed scenario events in both replays and the demo feed scale proportionally into early-close sessions, rounded down
to a minute. Seeds are FNV-1a of `id|YYYY-MM-DD`, independent of speed and wall time.
The same scenario and date reproduce prices on the same build and platform.
Sessions have 15-second snapshots and jump directly from the last snapshot to the
next open. Normal expiry, settlement, auto-close and the 17:00 ET trading-date
rollover apply. A series stays listed until its last trade, so positions carry from
day to day. Scenario opening levels can gap from the preceding close; the feed keeps
the close each day actually printed, unless the scenario explicitly authors
`previous_close`, which takes precedence. The
first day after a start also gets the closes of the trading date before it, generated
as the feed would have played that date, so the market-wide circuit breakers have a
previous close to measure a fall from from the first snapshot on.

The existing generator writes each day in a private temporary directory,
`openport-feed-PID-*` (scenario replays use `openport-demo-PID-*`). Replay pacing
consumes complete snapshots; the next day generates concurrently. Played files are
deleted, and shutdown removes the directory. A process killed outright cannot, so
openportd removes such directories at startup once the process that made them is no
longer running; a running server's and other users' are left alone. Feed status names the day
and labels prices simulated, without an end-of-recording stop between days. The
engine retains live accounts and write protection; Replay still runs beside it.

Cboe chart and proxy history, holiday downloads and volatility-series writes are
off. `--dividends massive` is rejected; a supplied dividend file remains usable.
At revision 4, without a `--dividends` source, the generated calendar also supplies
live account payments, American analytics, EX_DIVIDEND and early-assignment warnings.
An explicit file (even an empty one) replaces that account calendar; it does not
alter simulated prices. Earlier revisions do not add generated account dividends.
Candles stay in memory unless `--candle-dir` is explicit. The default journal is
`~/.openport/demo/paper-journal.jsonl`; accounts and replay history derive their
directories beside it. Journals do not record their feed, so keep any explicit demo
journal and Docker volume separate from live accounts. On restart, the feed resumes
after the latest recovered market time across the main and named accounts. When that
time's trading date has more snapshots to play, it plays the rest of that date: the
snapshots up to that time are applied without pacing and without reaching the
accounts, and the first one after it carries every contract's definition and latest
quote, so the chain is whole again. A date that had played to its last snapshot (a
quarter hour after its close) resumes on the next trading date, at its open. Without
recovered market time, the first day remains the last trading date before startup's
New York date.

## Public sandboxes

Only `--provider demo` may offer visitor accounts. `--sandboxes N` enables up to N
accounts at once; absent or 0 leaves the existing server behaviour unchanged.
Other providers and `--no-paper` are startup errors when N is positive.

For a public demo behind Railway's HTTPS proxy, for example:

```bash
openportd --provider demo --address 0.0.0.0 --sandboxes 100 \
  --write-token-file /var/lib/openport/write-token \
  --paper-journal /var/lib/openport/demo/paper-journal.jsonl \
  --allowed-origin https://terminal.example.com \
  --allowed-host terminal.example.com --client-ip-header X-Real-IP
```

Keep the operator token private. With sandboxes on, writes without credentials
are refused even on a loopback bind, since a local reverse proxy would make every
visitor a loopback writer: the operator trades the main account with the write
token, and status reports `write: "token"`. Railway supplies `X-Real-IP` and
`X-Forwarded-For`; select `X-Real-IP`, a single IP address. The configured header
must be overwritten by the trusted proxy, and the backend must not accept direct
public connections. No header is trusted by default. Missing, invalid, duplicate
or comma-separated values fall back to the connection address. IP literals are
normalized, including IPv6. Host and Origin checks still apply to creation.

`POST /api/sandboxes` accepts an empty JSON object without authentication and
returns HTTP 201 with `{account, token, idle_seconds, simulated: true}`. The account
uses the practice plan and its usual default cash, and pays the operator's
`--paper-fee` like every other new account. The random 128-bit bearer
secret is returned once, never logged or written to a URL. Only a digest stays in
memory. The terminal saves the token in session storage, falls back to memory
when storage is unavailable, and switches to that account. A refused sandbox
token is cleared so the visitor can create another. Operator token entry remains.

Creation permits 3 accounts per client and 30 globally in a rolling hour.
Pending creations count toward capacity. Creation returns HTTP 429 with
`SANDBOX_CAPACITY` when all slots are occupied, `SANDBOX_CLIENT_RATE` when the
client's creation limit is reached, or `SANDBOX_GLOBAL_RATE` at the global limit.
Each account permits 60 order requests per rolling minute, including previews,
what-if requests, modifications, share trades and their previews; excess requests
return HTTP 429 `SANDBOX_ORDER_RATE`. Cancels, flatten and share closes remain
available at the order cap.
The three rate-limit codes carry `Retry-After`: seconds until the oldest counted
request leaves that bucket's rolling window, rounded up to at least 1 second.
`SANDBOX_CAPACITY` has no `Retry-After` because the next free time is unknown.
Limits use an independent monotonic clock; they never advance the trading
reducer's market time.

Creation returns HTTP 503 `SANDBOX_UNAVAILABLE` if the command inbox or trading
storage is unavailable. An inbox rejection carries `Retry-After: 1`; a storage
failure has no known retry time and omits it. The notification test and channel
settings routes return HTTP 503 `NOTIFICATIONS_UNAVAILABLE` without `Retry-After`
when their source has no notification service. Sandbox tokens cannot use those
admin routes.

Sandbox tokens use `read` and `trade:ACCOUNT` scopes, with reads restricted to the
account and the simulated market. They permit orders, previews, cancels, flatten,
exercise, stock closure, alerts, notes and tags. They cannot change limits, guardrails,
kill switches, plans, playbook definitions or notification settings, create normal
accounts, reset accounts, run replays or backtests, or access another account.
Public API reads, account listings and WebSocket ticks omit sandboxes. Only their
own token or an authenticated admin sees them. Sandbox trading does not send
external notifications.

Accounts expire after 24 hours without authenticated requests. Set
`--sandbox-idle-seconds N` to change that interval. Keeping the terminal open and
polling counts as use; feed updates and outgoing ticks do not. The engine removes
the account, its credential and its entire directory under `sandboxes/` beside the
demo journal. The normal 64 MiB free-space journal refusal also applies to these
accounts. Capacity is returned after successful file removal; failed removal is
retried. Startup removes leftover sandbox files while holding the main journal's
writer lock. Sandboxes and their tokens do not survive restart. Main accounts,
named accounts and replay files are kept.

`GET /api/status` advertises `sandboxes: {enabled: true, idle_seconds}` while the
feature is on. With it off, that field is absent and creation returns HTTP 404.
Use a separate journal and volume for the simulated demo, as described above.

## Price history

The chart on Trade reads `GET /api/underlyings/{symbol}/candles` from two sources:

- **The engine's own spot.** Every index or ETF print the feed delivers is added to
  a one-minute bar at its print time, however many arrive between analytics passes,
  so a fast replay or a streaming feed keeps each minute's high and low. An
  underlying without prints is charted from the spot each analysis infers from
  parity, at the option data's market time. Delayed feeds therefore land on the
  minute the price was traded, not the minute it arrived, and an index frozen at its
  close adds nothing overnight while its parity spot moves with the global session.
- **Cboe's free chart files.** `openportd` fetches the latest regular session's
  one-minute bars (every minute while a session is open or just closed, every 15
  minutes otherwise) and the daily history (hourly) for each symbol. An official
  minute replaces a sampled one and later samples never change it. Cboe labels a
  minute bar by the minute it closes; OpenPort keys bars by the minute they open.
  Symbols Cboe does not chart (HTTP 403 or 404) are retried hourly. `--no-history`
  turns the fetch off.

Coarser intervals group minutes: 5, 15 and 30 minutes from the top of the hour,
and hours from half past, so the first hourly bar starts at the 09:30 ET open.
Daily bars are Cboe's; a session it has not published yet is built from its
regular-hours minutes (09:30 to the close, 13:00 on early-close days).

Finished minutes persist in `--candle-dir` (default `~/.openport/candles`), one
`SYMBOL.csv` per underlying with `start seconds,open,high,low,close,source` lines,
where the source is `o` for official and `s` for sampled and a later line for the
same minute wins. The directory keeps the last ten days of minutes; files are
compacted on startup and whenever superseded lines outnumber current ones. Daily
bars are refetched rather than stored. Replay and demo feeds keep bars in memory
by default and never fetch history, so a recorded day's prices do not mix with live
ones. Storage
failures are printed and never stop the engine.

## Probe readiness

Polling providers need one completed snapshot for **each** requested underlying.
Repeated success for one symbol does not stand in for another. Streaming feeds
need definitions and at least `--quotes N` received option quotes per underlying
(default 100). `--seconds S` sets the overall timeout (default 60); an incomplete
underlying produces a reason and exit code 1. Connection status alone does not
establish streaming readiness. Quote counts reflect retained events after queue
coalescing. Printed chains group by expiry instant and AM/PM settlement, so SPX
and SPXW on the same calendar date occupy separate groups.

## Product sessions and Cboe clocks

`md::trading_session(root, t)` returns regular, curb, global or closed, and the
market time: `t` while open, otherwise the most recent session end at or before
`t`. Regular trading is 09:30–16:15 ET for index roots and the ETFs Nasdaq lists as
trading until 16:15 (SPY, QQQ, IWM, DIA and about 60 more,
`md::is_late_close_underlying`; see [Nasdaq's options hours](https://www.nasdaqtrader.com/Trader.aspx?id=optionshours)),
and 09:30–16:00 for other equities. Early closes are 13:15 and 13:00 respectively.
SPX/SPXW, XSP, VIX/VIXW and RUT/RUTW also have curb trading 16:15–17:00 and GTH
20:15–09:25 Sunday evening through Friday morning. The current Cboe pages list
RUT in GTH as well. See [Cboe hours](https://www.cboe.com/about/hours/us-options/)
and [the GTH description](https://www.cboe.com/insights/posts/cboe-global-trading-hours).

A GTH session is assigned to the following morning's trading date:
`md::trading_date(t)` is a business day's New York date until 17:00 ET, when curb
ends, and the next business day after that and over weekends and holidays. Paper
accounts roll their day on it, and open sessions report their `end`, which ends DAY
orders. AM-settled series stop trading at the regular close before expiry
(`OptionContract::last_trade_time`). On expiry day, expiring PM index series stop at
16:00 and expiring ETF options at 16:15, which is their `expiry_time`; both settle on
the 16:00 closing print.

Summary and chain expiry objects expose `last_trade` and `auto_close` as UTC ISO
timestamps. `auto_close` is five minutes before `last_trade`, the evaluation plans'
cutoff; custom accounts can use another cutoff or none. The terminal defaults to
the nearest expiry whose auto-close has not arrived: SPXW until 15:55 ET and SPY,
QQQ, IWM and DIA until 16:10 on regular days. Early closes follow the same calendar.
On that day's chain, a countdown uses the underlying's effective market time from
status or connected ticks, including delayed feeds and paused replays. It does not
advance with the browser clock. Older servers without the times select the nearest
expiry with positive time remaining and show no countdown.

The calendar computes holidays and early closes from NYSE's rules, which Cboe's
options markets observe, for every year from 2022 on: a holiday on a Saturday closes
the Friday before (except New Year's Day, whose Friday would end the year), one on a
Sunday the Monday after, and the day before Independence Day and Christmas Eve close
at 13:00 when they fall Monday to Thursday, as does the day after Thanksgiving. The
tests check the rules against NYSE's published 2025–2028 calendars and Cboe's 2026
schedule. Before 2022 only weekdays are known.

Special closures are announced one at a time, so `openportd` also reads Cboe's
published holiday schedule ([CSV](https://www.cboe.com/us/options/holidays/csv/),
`providers::CboeHolidaySchedule`) when it starts and daily after, hourly while that
fails. Each date Cboe lists overrides the rules for that date through
`md::set_scheduled_days`: a closure, an early close, and whether the overnight session
runs into a holiday. A national day of mourning therefore takes effect once Cboe posts
it, without a new build, and dates seen earlier stay when the schedule rolls to the next
year. `--no-cboe-holidays` turns the fetch off; replay and demo feeds never fetch it.
The National Day of Mourning on 2025-01-09 is built in.

Around holidays GTH follows Cboe's schedule. It does not run into a weekend, New
Year's Day, Good Friday or Christmas; into the other seven holidays it runs from
20:15 the evening before until 11:30 ET on the holiday, for the next business day's
trading date, and the holiday evening opens the next business day's session as usual
([Cboe's holiday schedule](https://www.cboe.com/about/hours/us-options/)). On an
early-close day index options trade until 13:15 with no curb afterwards, as Cboe's
notices set out. The market-wide circuit breakers are modelled (see
[paper trading](paper-trading.md#orders-and-quote-matching)); halts of one stock or ETF are not, since no
provider here reports them. Trading sessions do not change option settlement or expiry
times.

The Cboe adapter polls the CDN files (`cdn-api.cboe.com/api/global/delayed_quotes/options/`)
every 15 seconds. Cboe moved them there from `cdn.cboe.com` in September 2026: the old
host served them stale from 2026-09-23, then redirected. The HTTP client follows
redirects (up to five, never from https to http, and without the Authorization header
once one leaves the original origin), so another move needs no new build. The symbols
are polled in turn, so each request step times out after 15 seconds and a request that
timed out is not retried: one slow response delays the others by that at most.
When a file's own timestamp is more than five minutes behind the clock, the adapter reads
the same chain document embedded in Cboe's quote page
(`www.cboe.com/delayed_quotes/spx/quote_table`, the `CTX.contextOptionsData` object)
and keeps whichever is fresher. The page is about 1.5 times the file's size and
Cboe refreshes it about once a minute, so the adapter fetches it no more often
than every 45 seconds, holds to it for five minutes, then tries the file again;
a page no fresher than the file (overnight, when neither moves) is left alone for
five minutes. The status message names the source, `(file)` or `(page)`. The page
stamps only a UTC time of day, dated by the clock. Because pages can trail the
stated delay by a minute or two, the paper feed check lets a delayed feed fall up
to three minutes behind its delay (or `max_quote_age`, if larger) before calling
it stalled.

Cboe option timestamps use `t = publication - 15 minutes`, capped to the most
recent session end only when that option root is closed. Live overnight and
curb quotes therefore advance while the stock/index print remains frozen.
`UnderlyingQuote.ts` is the parsed New York `data.last_trade_time`; missing or
invalid times remain unknown (zero), never fabricated from publication time. Outside
the regular session, measured by `t`, `current_price` goes on with after- and pre-market
trades still stamped with the last regular trade, so the underlying quote keeps the last
regular close instead, with bid and ask zero and the same `last_trade_time`: at or after
a business day's close (16:00, or 13:00 early), `data.close`; before the open or on a day
without a session, `data.prev_day_close`, which Cboe rolls over to the day's close at
about 21:00 ET. Shares and candles therefore use the regular close, not after-hours
prices stamped at it. In the regular session, or without a positive finite close, the
adapter keeps `current_price`, bid and ask. The adapter also publishes official closes as
`md::UnderlyingClose`, once per change. After the close, `data.close` holds the day's
closing price while `current_price` goes on with after-hours trades and
`last_trade_time` can stay at or just before 16:00. Cboe may revise the close within
minutes; both underlying quotes and official closes reflect revisions.
During the regular session, `data.prev_day_close` is the previous
business day's; in the evening the adapter does not read it, as Cboe may not have
rolled it over yet. Recordings carry these events too.
Analytics ignores an underlying print more than 30 minutes behind option data
(`AnalyticsOptions::max_spot_age_minutes`) and infers spot from parity, so frozen
index closes do not contaminate GTH Greeks. The limit sits above the 15-minute gap
between a stock or ETF's 16:00 closing print and its options' 16:15 close.

Status and WebSocket ticks include `{name, open, note}` under each underlying's
`session`. The top-level `market` remains the legacy regular-session calendar.
The as-of badge prefers the selected underlying's session: neutral overnight or
curb labels while open, market closed when closed, and stale only in an open
session when data age exceeds provider delay plus ten minutes.

## Reverse proxies

When a proxy rewrites `Host`, pass each public browser origin explicitly:

```sh
openportd --allowed-origin https://terminal.example.com \
          --allowed-origin http://localhost:3000
```

Origins match exact scheme and authority, with case-insensitive hostnames and
normalised default ports (`https` 443, `http` 80). Paths, credentials, wildcards,
and opaque origins are rejected. Same-origin requests remain accepted, and
non-browser clients may omit Origin. This setting permits WebSocket upgrades; it
does not add CORS response headers.

## Volatility event labels

`--events FILE` loads `YYYY-MM-DD,Label` lines for the volatility API's session-move
table. It accepts blank lines, comments beginning with `#` and a `date,label`
header. Dates must be valid and representable; labels are 1–160 bytes without
control characters. Duplicate dates are rejected with a line number. Labels apply
to every underlying, are read at startup, and do not affect pricing or trading.
No event dates are bundled. See [volatility metrics](volatility.md).

Daily Cboe history with a positive close and invalid OHLC is kept for realized
volatility's close-to-close fallback. Candle charts continue to omit invalid OHLC.


## Volatility series storage

`--series-dir DIR` stores local analytics history. The default is a `series`
directory beside the candle directory (`~/.openport/series` with default candle
settings). `--no-series` disables it. Replay and demo engines cannot write to it,
even when they inherit the live engine's options.

Each underlying has one append-only file per New York calendar month, for example
`series/SPX-2026-09.csv`. The first line is `#openport-series,1`; the second names
`minute` and the 22 metric columns. Minute is Unix seconds. Finite doubles use
17 significant digits and the classic locale; missing numbers are empty cells.
A forced backfill correction appends the same minute again; the last record for
that minute wins on reload. Normal sampling and backfill keep existing rows.
Daily close rows are cached separately for all sessions, rebuilt on reload.

The minute cache retains 45 elapsed days per underlying, measured from that
underlying's newest market minute, not wall time. `kSeriesMinuteRetention`
defines the window: minute starts after `newest - 45 days` through `newest`.
Each arrival updates the daily close before older minutes are evicted. Startup
streams every monthly file to rebuild all daily rows and the same minute window;
it does not retain the full minute history while loading. Daily queries use the
daily cache. Queries for older minutes read only the relevant monthly CSVs,
merge with cached minutes, and discard the temporary rows after the query.
They use the loader's parser and damage handling. The API's seven-day minute
range and 10,000-row limit still apply.

At roughly 230 bytes per cached row, 1,000 minutes a day use about 10 MB per
underlying for the window, or 52 MB for five underlyings. Continuous coverage
can retain at most 64,800 minutes (about 15 MB) per underlying. Daily rows add
roughly 60 KB per year per underlying at 252 sessions; file metadata and other
engine caches are additional. These are estimates, not measured process limits.

A store without a directory keeps the same minute window and all daily rows in
memory. Its evicted minutes are gone. An announced calendar change rebuilds the
daily cache from disk once, without repopulating old minutes. A memory-only
store can reselect closes only from retained minutes; if an old cached close
becomes invalid, that session is dropped rather than showing an incorrect close.

CSV files have no automatic retention or compaction. A typical populated row is
about 400 bytes (up to roughly 550 with long double representations): about 40 MB
per underlying per year for 391 regular-session minutes × 252 sessions, or about
145 MB for 1,440 minutes × 252 sessions with extended-hours coverage. Missing
fields reduce that size. Back up the directory and plan disk retention for
long-running extended-hours subscriptions.

Writes flush on each row. Storage errors do not stop the engine and appear in
`/api/status.series` with `enabled`, `directory`, `rows_today`, `last_write` and
`last_error`; Status displays them. `rows_today` counts unique minute rows on the
current New York date. `last_write` is the newest successfully stored market
minute (also recovered from disk), not wall-clock file modification time. Failed
minutes are not fabricated. The worker attempts a minute once; an offline
backfill can recover it later. Unsupported headers and torn or malformed files
are reported and block further appends to that file; valid preceding rows remain
readable. Repair such a file offline from a backup or recording.

To rebuild recordings without starting any network services:

```sh
openportd --series-dir ./series --backfill-series monday.oprec tuesday.oprec
openportd --series-dir ./series --backfill-series monday.oprec --force
```

The first complete snapshot of a minute is sampled. Repeating an unchanged
recording writes nothing. `--force` appends only changed values. Demo recordings
are refused. A recording without `SnapshotComplete` events cannot be backfilled.
Cboe proxy closes share the existing in-memory daily-bar cache; they are fetched
again after restart, and `--no-history` disables that fetch. They are never
written into the local metric CSVs. [Definitions and API](volatility.md#local-history).

## External notifications

`--notify-config FILE` enables server notifications. The JSON file must be a regular
file owned by the daemon's user with no group or other permissions (`chmod 600`).
Symlinks are refused. Keep tokens and private webhook URLs in this file or the
environment, never in command flags. Configuration errors, delivery failures and
`GET /api/status` omit destinations, tokens, chat IDs and provider response bodies.

Example configuration (replace the placeholders locally):

```json
{
  "queue_capacity": 256,
  "include_simulated": false,
  "channels": [
    {"id": "hook", "type": "webhook", "url": "https://example.net/alerts",
     "events": ["fill", "order_rejected", "rule_trip"]},
    {"id": "discord", "type": "discord", "url": "https://discord.com/api/webhooks/ID/TOKEN"},
    {"id": "telegram", "type": "telegram", "token": "BOT_TOKEN", "chat_id": "CHAT_ID"},
    {"id": "phone", "type": "ntfy", "url": "https://ntfy.sh/YOUR_PRIVATE_TOPIC",
     "events": ["floor", "assignment", "exercise", "playbook_ready", "feed_stalled", "alert"],
     "floor_distance": "500.00", "enabled": true}
  ]
}
```

`id` is a unique name of 1–64 letters, digits, underscores or hyphens. At most 32
channels are allowed. Webhooks and ntfy accept HTTP or HTTPS; use HTTPS to encrypt
off-machine delivery. TLS certificates are verified. Discord URLs must use
`https://discord.com/api/webhooks/`; Telegram always uses HTTPS. Redirects are not followed.
Generic webhooks receive JSON with `event`, `account`, `market_time`, `message`,
`simulated: true` and event-specific `details`. Discord receives plain content with
mentions disabled; Telegram receives plain text through `sendMessage`; ntfy receives
a UTF-8 text POST to the topic URL. Messages identify paper/simulated trading.

A file takes precedence over environment configuration. With no file,
`OPENPORT_NOTIFY_JSON` accepts the same JSON. If it is absent, these variables create
channels with the default filters and distance:

| Variables | Channel ID |
| --- | --- |
| `OPENPORT_NOTIFY_WEBHOOK_URL` | `webhook` |
| `OPENPORT_NOTIFY_DISCORD_URL` | `discord` |
| `OPENPORT_NOTIFY_TELEGRAM_TOKEN`, `OPENPORT_NOTIFY_TELEGRAM_CHAT_ID` | `telegram` |
| `OPENPORT_NOTIFY_NTFY_URL` | `ntfy` |

All nine events are enabled by default: `fill`, `order_rejected`, `floor`,
`rule_trip`, `assignment`, `exercise`, `playbook_ready`, `feed_stalled` and `alert`
(each firing of an [account alert](paper-trading.md#account-alerts)). `rule_trip`
reports a kill switch trip, the personal daily loss limit, and a plan daily loss
limit locking the day or failing the attempt. Plan notifications fire once on the
transition, with `code: "DAILY_LOSS_LIMIT"`, `action: "lock"` or `"fail"`, and the
loss `level` when available in the webhook's `details`. An empty
`events` array filters all automatic messages. Tests bypass event filters but require
an enabled channel. `floor_distance` defaults to $500 and compares fixed-point
`breach.room`, inclusive; missing floor room or incomplete valuation does not alert.
It fires on entry to the distance and rearms after leaving it. A staged order fires
when its stage ID becomes ready; price refreshes of the same stage stay quiet.
Feed stalls use the status timeout, the greater of 60 seconds and three poll
intervals, or an explicit stale/stopped state. Recovery rearms the feed warning.

By default only accounts on the live engine send. Set `"include_simulated": true`
in the notification file or `OPENPORT_NOTIFY_JSON` to also attach delivery to demo
and replay engines, including the main replay provider and interactive drills.
The opt-in applies to every configured channel type and its selected events.
Demo accounts appear as `demo/ACCOUNT`; replay accounts as `replay/RUN_ID/ACCOUNT`
(`replay/main/ACCOUNT` for a main replay provider without a run ID). Live account
names are unchanged. These names also keep concurrent live and replay observations
separate while sharing the bounded queue and destination rate limits.

Historical fills and deliveries loaded at startup, and events re-executed during
replay start/restart/resume catch-up, are ignored. Once ready, stepping or playing
a replay forwards new events. Verification, backtests and sandbox servers never
attach delivery. Browser price alerts and dividends remain in the terminal;
to receive a price alert off-screen, create an account alert on the underlying.
Firings are still recorded in the journal for run verification, independently of
delivery. Tests cover all channel formats and retries with a fake transport, plus
real loopback HTTP webhook and ntfy delivery from replay alerts, with no external
service or credentials. A channel's test button checks its configured delivery path.

The default queue holds 256 deliveries, including a request in flight; configure
1–10,000 with `queue_capacity`. A full queue drops new deliveries and counts them
per channel and globally. The delivery worker logs a counted warning without event
content. Each request has a five-second timeout per network step. Network errors,
HTTP 429 and HTTP 5xx retry at 1, 2 and 4 seconds, up to four attempts total. Retry
hints can extend those waits. Other errors, including redirects, are terminal.

Discord requests are spaced by at least two seconds per destination and honour
`Retry-After`, `retry_after`, `X-RateLimit-Remaining`, `X-RateLimit-Reset-After` and
global 429 responses. Its [rate limits](https://discord.com/developers/docs/topics/rate-limits)
are dynamic. Telegram uses at least 3.1 seconds per chat, including groups, and
35 ms per bot across chats; a 429's `parameters.retry_after` pauses that bot.
These budgets stay below Telegram's [published messaging limits](https://core.telegram.org/bots/faq#my-bot-is-hitting-limits-how-do-i-avoid-this).
Generic webhooks and ntfy use one second per destination and honour numeric
`Retry-After`. Identical destinations share budgets even across channel IDs.

`GET /api/status` includes `notifications`: enabled, `include_simulated`, queue depth/capacity, total
dropped deliveries and each channel's public settings, successful deliveries,
failed attempts, drops, last attempt, last successful delivery and last error code.
Configured channels remain visible when demo/replay forwarding is off; `enabled`
is false on that source, while `channels[].enabled` retains each channel's setting.
Queue and delivery counters describe the shared service, including other engines.
The browser refreshes status periodically. Times in delivery status are wall time;
event payload times are market time. Counters reset on restart.

`POST /api/notifications/test` with `{"channel":"phone"}` requires admin and
returns 202 when queued, not when delivered. Unknown, disabled and full channels
return 404, 409 and 429; a missing notification service returns 503
`NOTIFICATIONS_UNAVAILABLE` without `Retry-After`, as does the channel settings
route below.
`PUT /api/notifications/channels/phone` accepts only `enabled`, `events` and
`floor_distance`, requires admin and returns the public notification status.
Changes last until restart. Update the file or environment for lasting settings.
Removing an event or disabling a channel clears matching queued messages; a request
already in flight can finish. The Notifications section in Alerts settings exposes
these controls and never receives credentials.
It always lists configured channels, including on demo and replay, and separately
states whether demo/replay alerts are forwarded. The channel Enabled switches do
not override the operator's `include_simulated` setting. Demo/replay alerts are
not forwarded until that setting is true, and then follow enabled channels and
their event filters. Channel controls are read-only when forwarding is unavailable
or while viewing a replay; switch to live to manage them when forwarding is enabled.

Delivery is best effort. The queue is memory-only; a crash or shutdown can lose
pending alerts, and a request whose reply was lost can be delivered twice. One slow
channel can delay others on the single worker, but cannot block trading. Shutdown
cancels active I/O; system DNS resolution can still wait for the OS. No live provider
send was exercised in the offline test environment. Native SMTP is not included;
use a generic webhook connected to a webhook-to-email service for email alerts.

## Paper execution startup flags

| Flag | New main-account value |
| --- | --- |
| `--paper-fill-model as_displayed\|conservative\|midpoint` | `as_displayed` by default; the same presets as account reset |
| `--paper-slippage-ticks N` | Override the model's slippage, integer 0–10 |
| `--paper-fill-latency-ms N` | Override the model's latency, integer 0–60000 ms |
| `--paper-impact-ticks N` | Override the model's impact, integer 0–10 |

`conservative` selects 1 slippage tick, 1000 ms latency and 1 impact tick;
`midpoint` selects zero for those fields and 50% inside fills. `as_displayed`
selects zero throughout. Numeric flags override the model regardless of argument
order. Like `--plan`, these seed only a new main journal: recovery keeps all
recorded settings even if startup flags differ. Named accounts and resets use
request rules or `fill_model`; sandboxes use the practice plan's displayed fills.
Unlike `--paper-fee`, execution startup flags do not seed those other accounts.

## Paper fee schedules

`--paper-fee` remains the flat per-contract default for new accounts. Create/reset
can override it per attempt with `fee_model: "itemized"` or custom `rules.fees`;
`fee_model: "flat"` selects the server’s flat fee again. Recovery uses the recorded
schedule and preserves commission caps across partial fills and restarts. New
journal fields are optional; accounts without a schedule keep their existing
journal bytes. See [fees](paper-trading.md#fees) for amounts and API examples.

F33 keeps the replay driver version unchanged. Older journals recover their recorded
outcomes and attempt summaries unchanged. New resets include optional attempt rules,
decision time/equity, peak and floor in reducer state and the journal hash chain.
Replaying the same inputs under this build can produce different journal bytes after
a decision: closest-floor tracking freezes at the deciding observation, reducing
commands are accepted, rejection messages explain closing, and system liquidation
respects working user closes. Equity history also retains every decision mark; its
CSV format is unchanged. Verification remains per build, not a promise of identical
bytes across builds.

F52 playbook rules keep driver 6 and existing definitions' journal bytes. Only
entries using `trailing_stop` add the rule to their order note and an optional
`trailing: {peak, triggered}` strategy-review field to reducer state. Peak is Money
in the internal journal encoding; triggered is market time (zero before firing).
Older reviews omit the field and still recover. New configured exits add their
labelled cancellations and reducing IOC commands; old definitions and saved
backtest reports retain their original behavior and shape.

Journal repair accepts `openportd --repair-journals --dry-run` to report each
file, bytes that would be cut, and verified records kept without changing files or
creating backups. Add `--file PATH` to inspect or repair only that journal. Without
`--file`, the main journal and its `accounts/` and `replays/` journals are selected.
Stop the server first: dry-run also requires the journal lock. Exit 0 means all
selected journals were inspected/repaired; exit 1 means at least one failed
(including damage before the last line, which is never cut). `--dry-run` and
`--file` require `--repair-journals`; invalid options exit 2. Repair preserves the
original in a timestamped sibling backup.

Replay verification reports an estimated cost before work starts. Both the CLI
and HTTP use the maximum of the journal estimate (10 MiB/s or 2,000 transactions/s,
whichever is slower, minimum one second) and the replay input estimate: 4 seconds
plus 5 seconds per played hour, multiplied by max(1, scenario underlyings / 3).
The conservative hourly rate allows for wide option chains. Played time runs from
the journal start through the last journaled market time or the sidecar's later
`settled_through`. Scenarios count only overlap with their recorded-calendar
sessions, excluding closed nights and weekends; recordings use the continuous
played span without underlying scaling. Missing or unusable input metadata falls
back to the journal estimate. Estimates round up to whole seconds. At 30 seconds,
a warning is shown. Hardware, analytics and input generation can take longer.
`ReplayHost::stop()` closes its
control queue and refuses queued or subsequent starts with 503 `ENGINE_STOPPING`.

Newly closed recordings have an optional `.oprec.end` index, bound to device,
inode, size and modification time. It records each underlying's final market
clock and whether the file contains snapshot boundaries; end lookup and streaming
mode detection need no event scan. A missing/stale index (including moved or older
recordings) falls back to discovery at run startup and caches the end before
controls run. Generated scenarios use their known final window. A target after
the cached end still returns 400 naming that end before any playback. The index
never changes recording or journal bytes and is not part of run provenance.

## Reloading named tokens

`--token-file FILE` requires a valid, nonempty named-token file at startup. The
running server checks mtime and size on each authenticated request and in its
one-second loop. `SIGHUP` schedules a forced reload in that loop; admin
`POST /api/tokens/reload` with `{}` forces one immediately and returns loaded
`names`, `count` and `loaded_at`, never secrets. Python exposes `reload_tokens()`.
An empty replacement revokes all named tokens; a malformed or unreadable file
keeps the last successful immutable snapshot. Parse diagnostics name the line,
never its contents. Explicit failures return 400 `TOKEN_FILE_INVALID`; without
`--token-file`, the route returns 409 `TOKENS_UNCONFIGURED`. Prefer atomic file
replacement; force a reload if you deliberately preserve both mtime and size.

Workers share snapshots under a mutex, so a reload cannot publish a partial set.
Removed or changed credentials fail on the next authenticated request; without
`--require-token`, public reads remain public. WebSockets using changed or revoked
file credentials close before their next queued tick, while an in-flight message
or accepted command may finish. Legacy and sandbox tokens remain valid. No
restart is needed and sandboxes remain alive. `GET /api/status` reports
`tokens: {loaded_at, count}` for the last successful load. Reload never changes
`--require-token` or the server's open/token/disabled write policy.

### Backtest storage and retention

API backtests save reports, journals and progress under `backtests/ID/` beside the
main paper journal. `--backtest-keep N` (default 20; 0 unlimited) prunes the oldest
finished unpinned runs beyond N before a new start. Pinned runs are exempt and do
not count toward N. The newly started run can bring the total to N+1 until the
next start. Cancelled, failed and interrupted runs count as finished; active runs
are never purged. `PUT /api/backtests/ID {"keep":true}` persists a pin across
restart. Listings expose logical directory `bytes`, including journals and reports.
`DELETE /api/backtests/ID?purge=true` removes the entire saved directory, including
pinned runs, and refuses an active run (cancel and wait first). Plain DELETE still
only cancels. Run roots and children may not be symlinks. A small `sequence.json`
keeps new server IDs increasing after purge/retention. CLI reports are not pruned.
See [saved runs](playbooks.md#api-and-saved-runs) for comparison and partial results.

F52 report schema 2 adds per-attempt day rows and supplied input identities.
Schema 1 reports still load. Snapshot reads and comparison do not add reducer
commands or change any journal bytes; driver 6 and scenario revisions are unchanged.

F15 plan contract caps are opt-in: `max_contracts_held` is journaled only when nonzero. Existing plans keep their journal bytes; driver 6 and scenario revisions are unchanged.

F60 stop and trade-risk rules likewise omit `require_stop_loss`, `max_trade_risk` and `max_trade_risk_percent` at their off defaults. Only opted-in plans record the new fields and decisions; existing journal bytes and driver/scenario versions are unchanged. Preview risk fields are derived, never journaled.

F62/F58/F31 add optional plan time limits, inactivity, underlying whitelists and
opening hours without changing the replay driver or scenario revision. Rules at
their defaults and zero `last_activity` are omitted from reducer journals; existing
plans keep identical journal bytes. Configured plans record their rules, own-execution
activity timestamps and non-default `TIME_LIMIT`/`INACTIVITY` decision codes.
Hours cancellations record `OUTSIDE_PLAN_HOURS` with numeric New York minutes in
the journal. HTTP formats clock evidence for `OUTSIDE_PLAN_HOURS`, `FLAT_TIME` and
`HOLD_RESTRICTED` consistently as `HH:MM` New York time strings (or null).
Recovery and `--verify-run` use the recorded market clock and rules.

F6 adds optional `flat_time` and `no_overnight` custom rules, omitted from journals
at null/false defaults. The reducer records `flat_time_day` once per plan date and
`flat_pending` while mandatory closes remain; recovery resumes remainders without
retriggering the same date. Empty batches still advance opted-in flat plans.
The position-only `OVERNIGHT_HOLD` check precedes executions and mark checks on
rollover, excluding settlement-pending options. Option closes use
`system:flat_time:N` / `system:overnight:N` market IOCs, the combo-aware flatten
planner, and `max_order_contracts` splitting; stock fills use source `rule`.
Account evaluation exposes `flat_time` and `flat_now`. Disabled plans keep their
prior journal bytes; no replay driver or scenario revision changes.

Evaluation and verification use the same trade-objective derivation and restriction gates. Verification journals now retain enabled best-trade, closed-trade and short-profit evidence, including the share-fill attempt boundary; rule-disabled and funded journals keep their existing encoding.

F29 journals `trade_consistency_percent` only when nonzero and evaluation `best_trade` only when a positive closed whole trade exists under that rule. Summaries derive from reducer fills, closures and groups, including system closures; recovery repeats the same calculation. Off plans keep their bytes and hashes; driver/scenario revisions are unchanged.

F30 adds `min_trades` only when nonzero and evaluation `closed_trades` only when nonzero under that rule. Counts derive from current-attempt reducer history; plans without it add no state or journal fields.

F61 omits `min_hold_seconds`, `microscalp_seconds` and `microscalp_percent` at zero. Active microscalping records nonzero evaluation `short_profit` and a nondefault `first_stock_fill` attempt boundary; these derive from fills/closures and share fills, and reset with the attempt. Hold age is reconstructed from first opening fills, adding no timing state. Only opted-in OCO acceptance links its sibling before the hold check. Off journals, transaction hashes and replay/scenario revisions remain unchanged.

### Direction gates

The desk gathers `no_counter_positions` inputs on its single owner thread from each live account’s held book and each working opening order, compared separately with current valuations. Main and named accounts share this gate; archived, replay and sandbox accounts do not participate. Submit, increased-size modify, share trade and playbook entry paths share the acceptance check, with dry-run evidence for previews. An acceptance refusal never enters the reducer journal. Each proposed fill rechecks explicit cross-account direction inputs in the reducer and journals the inputs it used; violations cancel with `COUNTER_POSITION`. Recovery never reads another account. The `no_hedging` check instead lives in the deterministic reducer; both rules use the exposure check’s strict freshness policy for unknown delta. See [direction rules](paper-trading.md#direction-rules-f65).

### Volume-share gate

`max_volume_percent` reads ChainBook volume at pre-trade acceptance on the desk owner thread, for each option leg of opening submissions and increased-size modifications, including playbooks and previews. It checks `(abs(held) + opening) * 100 <= percent * volume` using wide integer arithmetic. Volume must be a current-market-trading-date, nonnegative whole count with no future timestamp; unknown or stale data returns `MAX_VOLUME_SHARE` with null volume evidence. Shares and pure reductions bypass the cap. Acceptance refusals are not journaled. Each proposed opening fill rechecks current volume against its quantity and holdings, cancelling with `MAX_VOLUME_SHARE` on failure. The reducer journals its explicit volume inputs and cancellation evidence; recovery never reads the feed. Backtests scan input recordings for usable current-date volume and reject the enabled rule with HTTP 400 if absent. Generated scenario sets and explicit scenario days support the rule without a volume scan: backtests always generate the current scenario revision, which carries deterministic intraday volume including opening activity at revision 6. See [volume-share semantics](paper-trading.md#volume-share-rule-f66).

### Plan event catalogue (F17/F59)

`openportd --event-calendar FILE` loads optional CSV `kind,time,symbol,session,label`.
Blank lines, `#` comments and the exact header are skipped; five comma-separated
columns are required (no quoted commas). Errors name the line. News timestamps
must be exact UTC ISO-8601 ending in Z; other times are YYYY-MM-DD. Earnings
sessions are before_open (empty defaults to it) or after_close; other kinds leave
session empty. News may leave symbol empty; other kinds require an uppercase
underlying. Labels have at most 64 characters; at most 256 entries. The read-scope route
`GET /api/calendar/events` serves this sorted/deduplicated catalogue for import.
This option is separate from `--events`, the volatility API’s session-move labels.

Only the account's saved rules.events decide F17/F59 outcomes. Create/reset journal
news_before_minutes, news_after_minutes, news_action, hold_restrictions and
hold_cutoff when nondefault. Holding plans also journal their business-day calendar.
Once-only calendar actions and crossed holding boundaries are journaled; recovery
never rereads the server CSV. Off-default omission keeps old plans' journal bytes
unchanged. Replay driver and scenario revisions are unchanged.

Combined F6/F17/F59 plans record crossed holding boundaries before executions.
At rollover, `OVERNIGHT_HOLD` precedes `HOLD_RESTRICTED` when both apply. Flat time
acts before calendar closes in the same transaction; journaled pending system
orders retain their labels and ownership of their legs after recovery. See
[combined rule semantics](paper-trading.md#news-blackouts-and-holding-restrictions-f17-f59).

### Program steps and costs (F27/F64)

Program progression and cost bookkeeping (F27/F64) use the existing journal schema
and replay driver. `verification` is appended to the internal phase enum, preserving
older evaluation/funded ordinals. New cost rule fields and actual attempt charges
are omitted at zero; archived payout totals are omitted at zero. Recovery never
charges a fee again. Account reset deltas add, replace or remove the current charge;
the running totals and reset streak are derived from recovered attempts. Older
summaries without rules end the known reset streak. Cumulative trader payouts are
derived from payout events (as settlement history is derived), including before old
resets; this derived value is never serialized as state or diagnostic snapshot. API create/reset accepts cost overrides; no startup
flag or user home-directory migration is required. Replay starts still require an
unlocked preset; next steps use a journaled account reset within the replay.

New HTTP resets record the optional command flag `program_costs: true`. Its absent
(default false) form re-encodes unchanged and preserves the older funded-restart
lock, refusal message and omission of archived payout totals when re-executing old
runs. This command-level compatibility choice avoids changing replay driver 6.
The optional `program_cost_overrides` bit mask records which reset cost settings
were explicit; an absent/zero mask is omitted, and a new same-plan reset retains
current settings for omitted fields.

Program-aware resets also archive enabled size-scaling progress. Reset identity
uses its original balance and loss limits, restoring a shifted lock balance before
comparing plan rules; contract and size-scaling settings remain part of the plan.
The current size and live rules stay in the attempt summary for historical reporting.
Legacy resets without `program_costs` omit this new archive field, preserving their
recorded state when replayed. Old summaries without original scaling provenance
cannot extend a reset streak across a size change.

New resets record `restore_scaled_rules: true`: resubmitting unchanged live rules
at the original starting balance restores original loss and lock amounts from
size-scaling progress. Commands without this flag retain their previous reset
behavior during replay; old account journals still recover their recorded rules.

New run starts record optional `opening_rule_checks: true`. Verification and crash
resume use its recorded value; absence preserves older runs’ acceptance-only
volume checks. New starts also record `compact_opening_rule_inputs: true`: at fill,
`opening_rule_inputs` events retain volume entries only for the attempted order's
legs and counter exposures only for the underlyings in its direction calculation,
in their original order. Missing volume entries remain missing. The check still
uses the complete supplied inputs, including whether any counter exposures exist
when direction cannot be established. Older runs without this flag reproduce
their full input maps byte for byte during verification and crash resume; both
event forms recover as recorded outcomes. These are transaction inputs, not callbacks or
persistent account settings. Restarted live desks supply fresh inputs before
matching; archived, replay and sandbox accounts remain outside cross-account checks.
