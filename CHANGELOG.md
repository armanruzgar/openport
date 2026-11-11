Notable changes in each release. Older releases: https://github.com/armanruzgar/openport/releases.

## Unreleased

### Upgrading

- **`chmod 600` a named-token file.** `--token-file`, and a `--write-token-file` that
  holds a token, must now be owner-only regular files; openportd refuses to start
  with one that others can read. Files openportd created itself already are.
- **Tighten a data directory made by an earlier version.** openportd now creates its
  directories owner-only and writes state files owner-only, but it leaves the
  permissions of directories that already exist alone, and files it does not rewrite
  keep theirs. Run `chmod -R go-rwx ~/.openport` once (for Homebrew,
  `chmod -R go-rwx "$(brew --prefix)/var/openport"`).
- **The Python client is not on PyPI.** 0.5.0 said it would publish there as
  `openport-client`; it never did, and the PyPI release workflow is gone. A PyPI
  package of that name is not from this project. Install a `python-v*` tag from
  GitHub into a virtual environment, as the
  [client's README](python/README.md#python-releases) shows.
- **Replay driver 7.** New reproducible runs record driver 7, which applies the
  evaluation, payout and order-rule fixes below. Runs recorded earlier still pass
  `--verify-run` with their original decisions, but an interrupted one can no longer
  be resumed and none can be restarted from a time: start a new run instead.
- **Replay driver 8.** New reproducible runs record driver 8, which also applies the
  fill and order fixes below: queue fills, stop stress, trailing stop-limits on the
  underlying, a walking order's `then` and limit flatten. Runs recorded with drivers
  1–7 still pass `--verify-run` with their original fills; like driver 7's, they can
  no longer be resumed or restarted.
- **Replay driver 9.** New reproducible runs record driver 9, which also applies the
  split, dividend, financing and margin fixes below. Runs recorded with drivers 1–8
  still pass `--verify-run` with their original rollovers and margin; like driver 8's,
  they can no longer be resumed or restarted.
- **Replay driver 10.** New reproducible runs record driver 10, which also applies the
  working share order fixes below: client IDs per attempt, an OCO pair checked and
  reserved as one, and stops reached only in their session. Runs recorded with
  drivers 1–9 still pass `--verify-run` with their original share orders; like driver
  9's, they can no longer be resumed or restarted.
- **Replay driver 11.** New reproducible runs record driver 11, which also applies the
  order and account upkeep fixes below: share orders cancelled with the option orders
  a rule stops, re-armed exits waiting for new size, exact pin deliveries, settlement
  marks, a reset after the day's end and bounded order change histories. Runs recorded
  with drivers 1–10 still pass `--verify-run` as they were recorded; like driver 10's,
  they can no longer be resumed or restarted.
- **Kill-switch and reset reasons are short text.** `POST /api/risk/kill` and
  `POST /api/account/reset` now refuse a `reason` over 200 bytes or with control
  characters (400 `INVALID_REQUEST`); a script that sent long or multi-line reasons
  must shorten them.

### Providers

- **Interactive Brokers option volume.** Snapshots also ask for field 7762, the day's
  volume, and keep it from real-time rows at their `_updated` time. Delayed and frozen
  rows, and a positive count beside a previous close or halted price or with no last
  price, leave volume unknown. The extra field takes no extra market-data line. Not
  yet run live with an account; reports welcome.

### Features

- **Subscribe to an underlying while openportd runs.** The underlyings were fixed at
  startup: to look at or trade NVDA you restarted openportd with a new `--symbols`,
  which interrupted the feed, the working orders and any recording, and a Homebrew
  service could not do it at all. On a polled feed (Cboe, Tradier, Massive, ThetaData,
  Schwab, Alpaca, Interactive Brokers) `POST /api/underlyings` now adds one, polled from
  the feed's next cycle, and `DELETE /api/underlyings/{symbol}` removes one added that
  way once no account holds, works or watches anything on it; both need admin access,
  and `GET /api/underlyings` lists what the server carries. The terminal's symbol
  search (Ctrl/Cmd+K) offers to subscribe to a symbol it does not find and lists the
  additions with a remove button; Python has `Client.subscribe()`, `unsubscribe()` and
  `underlyings()`. Additions last until openportd restarts and are not recorded: a
  recording keeps the underlyings its header names. Databento, tastytrade, a feed with
  a fallback, the demo market and replays keep a fixed set and say so (409
  `SUBSCRIPTION_FIXED`).
- **Notification history.** openportd kept no record of the notifications it
  published: a floor warning, a feed stall, a staged playbook order or an earlier
  firing of a repeating alert reached only a terminal open at the time, or an
  external channel, and a terminal opened later (or on another device) showed none of
  them. `GET /api/notifications/history` now returns an account's recent events as the
  event stream sent them, with the time the server published each and the feed's
  stalls beside them: the latest 200 per account, in memory until openportd restarts.
  The bell drawer reads it and adds those a browser cannot see for itself, unread and
  without a toast; `Client.notification_history()` reads it too. Feed stalls are
  published whether or not an outbound channel is configured, and an alert event's
  details now carry its `fired` count.

### Fixes

- **Live-only routes under the replay mirror.** `/api/replay/X` forwarded every route
  to the replay's engine, which shares the live server's account-group store, so
  `PUT` or `DELETE /api/replay/account-groups/...` edited the live groups. Account
  groups, copied orders and fill calibration now answer 404 under `/api/replay`;
  the mirrors meant to work (share orders, Heston, rules comparison, rule-pack
  validation) are now in `docs/openapi.yaml`.
- **WebSocket refusals.** An upgrade to `/ws` or `/ws/events` that a token, scope or
  account check refused always answered 403 with the JSON error as plain text, so a
  malformed `account` was 403 where REST says 400. It now carries the REST status and
  `application/json`; the other refusals stay plain text, as the contract now says.
- **Analysis imports that net to zero.** A holding whose lots net to zero was dropped
  without a word, though the docs said it was listed as skipped; it now is
  (`line 2 SPY: its lots net to zero`).
- **Hints and help that pointed the wrong way.** The Python client's missing-extra
  errors said to install `openport[pandas]`, `[arrow]` or `[ws]`, which pip resolves
  to an unrelated PyPI project; they now name `openport-client` and `./python[...]`.
  `openportd --help` placed earnings and analysis portfolios in `~/.openport` (they
  are beside `--candle-dir`), called Tradier's contract sizes and tastytrade's
  millisecond times opt-in (they are the defaults) and gave the demo revisions as
  1–8 (default 8); it now follows the generator's revision, 9. A sandbox past its
  order budget is told that alerts, notes and grouping count too.
- **OpenAPI gaps.** `Status.series`, the volatility history fields
  (`history_values`, `history_basis`, `history_sessions`, `ex_post_vrp`, `proxy`),
  the breach scan bounds, `AccountListItem.sandbox_idle_seconds` and a history run's
  `imported` and `simulated` were sent but not in the contract, and three archived-run
  mirrors never declared their `run_id` path parameter. A Python test now checks
  every path's parameters.
- **Warming-up and unknown underlyings.** Right after startup, before an underlying's
  first snapshot, its summary, chain and analytics answered 404 "no data for SPX yet",
  exactly as for an underlying the server does not carry (`ZZZ`, or `spx` for `SPX`),
  so a script could not tell "retry" from "typo". A carried underlying without a
  snapshot now answers 503 `NOT_READY` with `Retry-After: 5`, which the Python client
  retries, and one the server does not carry 404 `UNKNOWN_UNDERLYING`; the terminal
  still shows the first as waiting for its snapshot. `GET /api/status`'s underlying
  `state` is documented as the readiness check.
- **Misspelt market-data query parameters.** `/api/status` and the underlying's
  summary, chain, exposure, surface, Heston, volatility and candle views ignored a key
  they do not take, so `chain?windw=0.01` answered the full unwindowed chain and
  `exposure?covention=short_all` the default convention, with no sign of the typo.
  They now answer 400 `INVALID_REQUEST` naming the key, as probability, flow and the
  volatility scanner already did.
- **Cancel-all and share orders.** `POST /api/orders/cancel` cancels working and armed
  share orders as well as option orders, but its answer listed only the option orders,
  so a script or agent was told nothing was cancelled when share orders were. The
  answer now carries `cancelled_stock_orders` too, which the Python client's
  `CancelAllResult` types. The terminal's **Cancel all** counted option orders only;
  it now shows with share orders alone and counts them in its confirmation and result.
- **Methods a route does not take.** A known route asked with the wrong method
  answered 404, 400 or 405 depending on its handler, and no 405 carried the `Allow`
  header; `PUT /api/orders/what-if` even ran the what-if, and `PUT
  /api/orders/preview` was read as a change to order "preview". Every route now answers
  a method it does not take with 405 `METHOD_NOT_ALLOWED` and an `Allow` header from one
  table of routes, and `PUT`/`DELETE` reach only `/api/orders/{id}` itself.
- **Paper read errors and time bounds.** The paper read routes (`/api/account/equity`,
  `/api/fills`, `/api/orders`, `/api/trades`, the CSV exports, fill quality, the risk
  profile and hedges) answered any unknown key or bad value with the same 400 "Unknown
  or invalid query parameter"; the message now names the parameter and what it takes.
  `from` and `to` were read differently per route: the equity history took only a UTC
  instant and the CSV exports only a New York date. Both now take either (a date is the
  whole New York day, inclusive), and `GET /api/fills` takes them too.
- **Blank and duplicate account names.** `POST /api/accounts` and
  `PATCH /api/accounts/{id}` accepted a name of only spaces or invisible characters (a
  zero-width space), and one equal to another account's, `Main` included, so the
  account switcher, which shows names and not IDs, listed blank or indistinguishable
  accounts. Names are now trimmed of surrounding spaces and need a visible character
  (400 `INVALID_REQUEST` otherwise), and another live account's name, ignoring case,
  is 409 `ACCOUNT_NAME_TAKEN`.
- **Risk profile betas with hypothetical legs.** `GET /api/risk/profile` with `legs`
  took each beta row's dollar delta from the account's own risk, so `betas[].dollar_delta`,
  `weighted_dollar_delta` and `weighted_delta` reported the held book even with
  `held=false`, where the curves are the legs alone, and left the legs out with
  `held=true`. They now describe the same book as the curves: the held book in scope
  plus the legs, or the legs alone.
- **A playbook named `staged`.** Every POST under `/api/playbooks/staged/` was read
  as a staged order's action, so `POST /api/playbooks/staged/evaluate` failed for a
  playbook of that name with "Unknown staged-order action", and a trade-scoped token
  could change its mode. Only `/api/playbooks/staged/{stage}/send` and `/dismiss` are
  a stage's routes now; the rest are that playbook's, with its scopes.
- **CSV exports of small and large negative numbers.** JSON prints a double below
  1e-4 or from 1e15 with an exponent, and the formula guard of `/api/fills.csv` and
  `/api/trades.csv` took `-5e-05` for text, so a far out-of-the-money put's delta, or
  a small negative return, heat or attribution, reached a spreadsheet as text behind
  an apostrophe. Signed numbers with an exponent now stay numbers.
- **Commands on a quiet feed.** A trading command did not wake the engine thread, so
  on a quiet feed (the demo between its batches, a paused replay, a polling feed
  between polls) each order, cancel or preview waited out the engine's 50 ms wait for
  market data, holding a sequential client such as a bot to about 16 commands a
  second. Posting a command now wakes the engine, which applies it at once.
- **Trade notes with a non-string mistake.** `PUT /api/trades/{id}/note` with a
  `mistakes` pick that is not a string answered 400 with the JSON library's internal
  message; it now says `mistakes must be strings`, as tags do.
- **`--no-paper` writes and account routes beside a locked main journal.** A trading
  or playbook write to a server started with `--no-paper` answered 503 "Command inbox
  full or trading unavailable" with `Retry-After: 1`, so clients, the Python client
  among them, retried a request no retry could clear. It now says paper trading is
  turned off on this server and sends no `Retry-After`. Renaming, archiving,
  unarchiving or deleting a named account was refused with main's `JOURNAL_LOCKED`
  reason when another process held only main's journal; these routes now check the
  account they name.
- **Query values that are not UTF-8, and rules-compare's query.** A percent-decoded
  query key or value that was not UTF-8 reached the routes, and one a route echoed in
  its JSON answer (`chain?expiry=%ff`, `/api/earnings?symbol=%ff`) made printing the
  answer throw: the client's mistake came back as 500 `INTERNAL_ERROR`. Every route
  now refuses it with 400 `INVALID_REQUEST`. `POST /api/account/rules-compare` read
  only `account` from its query and ignored the rest, so a misspelt `?acount=second`
  compared the main account; like the other account routes it now answers 400 for
  anything but `account=ID`.
- **Copied orders and account-group bodies.** A ratio group sized each member as the
  order's quantity times its ratio and narrowed the result to 64 bits, so an absurd
  quantity (one whose share at a ratio up to 100 passes 2^63 - 1) wrapped to a small
  order the member's account could fill. `POST /api/orders/copy` and its preview now
  refuse such a quantity whole with 400 `INVALID_REQUEST` and send nothing. An empty or
  malformed JSON body on `PUT /api/account-groups/{name}` or the copy routes answered
  503 `GROUPS_UNAVAILABLE`, a temporary outage a client may retry; it is 400
  `INVALID_REQUEST`, as on `POST /api/orders`.
- **Implied session moves around an expiry without MFIV.** Each expiry's total variance
  used MFIV, or ATM when MFIV was missing, and consecutive totals were subtracted
  whatever their basis. MFIV sits above ATM on a skewed smile, so an MFIV expiry
  followed by an ATM one read as negative forward variance (a false calendar-arbitrage
  flag and a null move), and the next interval took the whole gap between the two
  levels, overstating that session's move. Each interval is now differenced on one
  basis at both ends: MFIV where both expiries have it, otherwise ATM, marked proxy.
- **The volatility page's skew reason.** `skew.reason` in
  `GET /api/underlyings/{symbol}/volatility` took the last reason of any 30-day skew
  input, 10-delta points included, so a chain whose 10-delta points did not
  interpolate (the demo's SPX) reported `unusable_bracketing_expiry` beside a valid
  25-delta RR and BF, and the terminal's 25Δ tile and the Python example showed it as
  theirs. It now comes only from the 25-delta RR and BF inputs (the ATM, call and put
  points); each 10-delta point keeps its own reason.
- **Surface-history comparisons with a range.** `GET /api/underlyings/{symbol}/surface-history`
  looked up the prior close, week and month comparisons only among the snapshots in
  the request's `from`/`to`, so a range that left out their sessions returned them as
  null, which the API documents as a close that was never captured. The Python client
  and the MCP tool send `compare=true` with the range they are given, so an agent that
  narrowed it got every comparison as null. Each target session is now read on its own,
  up to the market's time.
- **`--american-model bs2002` with strongly negative carry.** Where b·T + 2σ√T ≤ 0 in
  the formula's call form (a 20% dividend yield over two to five years, or a put at a
  high rate and low vol), Bjerksund-Stensland's exercise trigger fell below the strike,
  at-the-money contracts priced at intrinsic, and their early exercise premium was
  reported as 0 by `bjerksund_stensland_2002` instead of the tree's 0.45 to 3.61. The
  closed form is undefined there and those expiries now fall back to the Leisen-Reimer
  tree, as `eep_model` reports.
- **Cone implied vol at an expiry's close.** `realized.cones[].implied_vol` in
  `GET /api/underlyings/{symbol}/volatility` was null (`target_not_bracketed`) about one
  time in ten when the cone's horizon closed exactly at the first expiry's settlement (a
  weekly-only underlying after Friday's close, five sessions out), depending only on
  the nanoseconds of the snapshot time: the horizon was rounded twice on its way to
  minutes and could land a hair below that expiry. It is now computed as the expiries'
  own minutes are, so it is that exact tenor.
- **`--events` files from spreadsheets.** A file that began with a UTF-8 byte-order
  mark (Excel's "CSV UTF-8"), or whose header was `DATE,LABEL` or `date, label`,
  stopped openportd at startup with "events line 1: date must be YYYY-MM-DD", pointing
  at an invisible byte. The mark is skipped, and the header is recognised in any case
  and spacing.
- **Retried copies to an equity-split group.** `POST /api/orders/copy` recomputed an
  equity split on every request, and after the first copy filled, the paying member's
  slightly lower equity moved the remainder unit to another member. A retry with the
  same `client_order_id` then skipped the member that traded and sent the other one a
  new order, so a retried 1-lot left 2 lots across the group. A member that holds an
  order under the ID is now sent that order's size again, which it answers as first
  submitted, and only what those orders leave of the quantity is split among the rest.
- **A future timestamp no longer moves every paper account's clock.** One quote,
  trade, heartbeat or halt from a live feed stamped in the future (a unit or epoch
  error such as 2100-01-01 from milliseconds, or an upstream glitch) moved the market
  clock, which only moves forward, and with it every account's clock and journal:
  later orders on real contracts were refused as expired, and a restart restored the
  bogus time. A live event stamped more than five minutes after the system clock is
  now refused before it is recorded, queued or applied, and the underlying's
  `last_error` names it. An account journal already moved forward still needs its
  backup.
- **Recording to exFAT, FAT and network drives.** `--record-dir` published each
  finished day with a hard link, and on a filesystem without them it deleted the
  complete, cleanly closed day instead, at every rollover and shutdown, and stopped
  recording. A day now publishes by an exclusive rename where hard links fail, and a
  complete day that still cannot be published stays in its `.record-*` staging folder,
  named in the recording error.
- **`--record-dir` with the demo.** The demo hands its batches straight to the engine,
  so a directory recording got none of them and published an empty file into the
  Replay list at every clean shutdown, one more with each restart. The demo is now
  recorded like any other feed, and a trading date on which nothing arrived publishes
  no file.
- **Recorded days replay alone.** A `--record-dir` file for a new trading date
  repeated only the contract definitions. A snapshot feed such as Cboe sends a quote,
  open interest or close only when it changes, so a day replayed on its own showed
  unchanged contracts unquoted or without open interest, and its chain, IV surface,
  exposure and paper fills differed from the live session. A new day's file now opens
  with each contract's latest quote, open interest, volume and Greeks and each
  underlying's latest quote and close.
- **Analysis imports with lots on opposite sides.** A symbol's long and short lots were
  netted at their signed average cost, which could be negative: long 2 at $1 and short
  1 at $3 read as long 1 at −$1. The ledger refuses a negative cost, so the import
  answered 500 yet kept the portfolio, and every later read of it and all four risk
  views answered 500 too. Lots on opposite sides now close against each other, as an
  average-cost ledger reduces a position: the holding keeps the cost of the lots on the
  side it nets to. A portfolio stored with a negative cost reads again, without that
  cost in its book, and an import that cannot be valued is no longer kept.
- **SPX, NDX and RUT weeklies in analysis imports.** A file that names an option by its
  underlying, expiry, strike and right (the generic split columns, and tastytrade's and
  IBKR's columns) built every SPX option on the morning-settled SPX root, so a weekly or
  daily was a contract no chain lists: `not_in_chain`, unpriced and left out of every
  risk view. Off the monthly expiry they now take SPXW, NDXP or RUTW; a root written in
  the underlying column is still kept.
- **Futures options in analysis imports.** tastytrade's `Future Option` rows and IBKR's
  `Futures Options` and `Options On Futures` rows were read as equity options on the
  futures root (/ES became an ES option with a multiplier of 100), or refused the whole
  file when the option could not be read. They are now skipped and listed, as futures
  were, and so is a symbol starting with `/` in every layout.
- **MCP tools say what failed.** A timeout, a dropped connection or an `openportd`
  that is down, and arguments the client refuses before sending (`version_playbook`
  with a definition missing its `id`), reached the agent from every tool but
  `place_order` only as `Error executing tool NAME`, so after a trade, cancel or
  flatten it could not tell a lost answer from a bad argument. Every tool now returns
  an error: `UNREACHABLE` when nothing was sent, `NO_RESPONSE` when an answer was lost
  and a command may have been applied, `INVALID_REQUEST` for refused arguments.
- **`--plan` with `--paper-cash`.** `openportd --plan funded-intraday-50k --paper-cash
  60000` started an account under the preset's name with its dollar rules unscaled on
  the other balance, a floor locked at 50,000 under a 60,000 start, that was not that
  preset: a pass did not unlock the next step, and a funded account could not restart
  into its own preset (`PLAN_LOCKED`). `--paper-cash` now sets only a practice
  journal's balance; beside another preset it must be that preset's own, or
  `openportd` exits with a usage error. It never applied to an existing journal, so a
  service that passes it can drop it.
- **MCP equity pages.** The `equity` tool answered with an account's oldest 500
  samples and had no way to ask for the next page, so an account's recent equity was
  out of reach unless the agent guessed a start time. It now takes `cursor`, the
  `next` of the page before, and says samples come oldest first and that `start`
  selects recent ones.
- **Strategy runner bars after a quiet start.** `openport.bot.Runner` started on a
  symbol with no bar or one open bar (a fresh paused replay, or live before a bar
  closed) skipped every bar that closed before a poll first saw two, and a hook that
  stepped the replay before the first poll lost every bar the step closed. The runner
  now reads each symbol's bars before `on_start` and delivers every bar that closes
  after that.
- **Feed failover and `Stale` feeds.** A paragraph of the
  [runtime guide](docs/runtime.md#feed-failover) still said a feed that reports `Stale`
  while healthy counts as failing. Failover judges freshness apart from that label:
  such a feed keeps its underlyings while its snapshots advance, as the rest of the
  section and the providers guide say. The paragraph is gone.
- **Stop-required plans and share entries.** The plan rules in
  [paper trading](docs/paper-trading.md) said a stop-required plan refuses every share
  entry. It refuses an immediate share trade, which cannot carry a bracket; a working
  share order with a `bracket.stop_loss` is accepted, and its trade risk is measured to
  that stop.
- **The Docker `--verify-run` example.** The [install guide](docs/install.md) verified
  the main paper journal, which always fails: `--verify-run` reproduces replay and
  scenario runs and backtest journals, and an account journal is not a run. The
  example now verifies a run in `replays/`.
- **The playbook guide's example trades in the demo market.** The example definition in
  [playbooks](docs/playbooks.md), a 5-point SPX put spread with a 2× credit stop, takes
  in about $0.20 there, no more than its legs' bid-ask, so on every demo day it traded
  its stop was already reached when the entry filled and it closed at once for a loss.
  The example is now 15 points wide with a 3× stop, and a test runs it from the guide
  on the guide's own backtest days.
- **CI checks real API responses against the contract.** The Python tests that check
  the C++ fixtures' handler responses against `docs/openapi.yaml`, and drive a built
  `openportd` through the client, always skipped in CI, because the Python job had no
  build; so did the Arrow and Parquet export tests, without pyarrow. The Ubuntu C++
  job now runs them on its build, the Python job builds from its cache when only
  Python changed, and in CI a missing build fails them instead of skipping them.
- **A failing web test shows its assertion.** `waitForRender` retried a check for 15
  minutes, as long as CI's whole web job, so a page that never showed what a test
  expected ended as a cancelled job with no message. It now fails after 20 seconds
  with the check's own message (`OPENPORT_RENDER_WAIT_MS` allows longer on a loaded
  machine), and a test that hangs fails after 5 minutes.
- **An installed openportd finds its terminal from any directory.** Started by name
  from `PATH`, as `openportd --open` after a Homebrew, package or `cmake --install`
  install, it looked for the web terminal beside the working directory instead of
  beside its binary, so `/` answered 404 "web terminal not built", or served a
  `web/dist` the working directory happened to hold. It now finds the binary it runs
  from, following Homebrew's link into its Cellar.
- **Closing the terminal stops openportd.** `SIGHUP`, which a closing terminal window
  or SSH session sends, only asked for a `--token-file` reload, even without one, so
  the server kept running unseen, holding its port and journal lock, and a
  `--backtest` run went on. Without `--token-file` it now stops the server, or cancels
  the backtest and writes its report, as Ctrl-C does; under `nohup` it stays ignored.
- **The Homebrew service takes settings.** `brew services` ran openportd with fixed
  flags and no environment, and launchd does not read the shell's, so the service
  always watched Cboe's delayed SPX, SPY, QQQ, IWM and DIA, without recordings,
  notifications or another provider's key. It now adds the flags in
  `$(brew --prefix)/etc/openport/openportd.args`, one per line with its value, and
  the `NAME=VALUE` lines of `openportd.env` to its environment; `brew services
  restart openport` applies them. The service command carries this itself, so it
  works with the installed 0.5.1 as soon as the tap is updated.
- **The macOS archive names and keeps its oldest macOS.** It was compiled for whatever
  macOS built it: 0.5.1 is stamped for macOS 27, with OpenSSL built for 27, and needs
  C++ library functions that macOS 14 and older lack, so it cannot start there, and
  nothing said so. A Mac build that links OpenSSL and zstd statically now targets
  macOS 15 (`OPENPORT_MACOS_FLOOR` in `CMakeLists.txt`); `tools/release.sh` makes the
  archive only when those libraries are built for macOS 15 or older and checks the
  programs, and otherwise leaves it to the release workflow, which builds it on
  macOS 15. CI checks the same on every macOS build. The Homebrew formula requires
  macOS 15 on Apple Silicon, which the published 0.5.1 archive needs too, and the
  README, the install guide and the release notes say so.
- **Third-party licences ship with every release.** The archives, the `.deb` and
  `.rpm` packages and the container image carried only OpenPort's own MIT licence
  (the image not even that), though the programs contain databento-cpp, libdbn_c,
  simdjson, JSON for Modern C++, cpp-httplib and date, the macOS
  archive also OpenSSL and zstd, and the web terminal React, TanStack Query, Tailwind
  CSS and the Geist fonts, whose licences ask for their notices to go with every copy.
  `THIRD_PARTY_NOTICES.txt` now lists them with their licence texts, beside the
  README in each archive, in `share/doc/openport` of the packages and the image, and
  in Homebrew's documentation once a release includes it; the web terminal has its
  own beside its `index.html`. A CI check fails when a fetched library or a web
  runtime package is missing from them.
- **The order fuzzer's restart check.** `tools/order_fuzz.py --restart-check` required
  a restarted run's journal to be exactly the source's prefix, but a restart has
  journaled a restart input after that prefix since restarts could start mid-run, so
  every campaign failed with `restart_prefix` although the prefix matched byte for
  byte and both verifiers passed. It now requires the prefix, then exactly that one
  `run_input` naming the source run and the restart time.
- **A paper journal that cannot open is named at startup.** Only a journal held by
  another openportd was reported on the console. One that could not be opened
  (`JOURNAL_IO`, such as a directory or an unreadable file) or was damaged
  (`JOURNAL_CORRUPT`, `ACCOUNT_DAMAGED`) left paper trading off with nothing printed.
  Startup now prints the journal's path and the reason on stderr.
- **Minute-volume cap after a gap in the feed.** The cap took all volume since the
  last report a minute or more old as the last minute's, so for a minute after a feed
  outage, a reconnect or a slow poll it accepted orders sized on the whole gap's
  volume. It now refuses them with `MINUTE_VOLUME` until a minute of reports spans the
  gap; on a feed seen every minute, an unchanged older report still counts.
- **Quote width alone no longer decides a plan.** A single two-sided quote far wider
  than the price band, such as 4.00 × 16.00 on a call bought at 4.20, marked the
  position at its midpoint: it could pass a losing attempt, raise a trailing floor the
  next ordinary quote then failed, or fail it outright, while the bid had not moved.
  Such a quote now keeps the mark a narrower quote set within `max_quote_age`; a wide
  market that lasts longer marks at its midpoints again.
- **Queued fills after the close.** With the queue fill model, a GTC or GTD limit or a
  bracket target kept earning its place and fills after the regular close, when it
  cannot trade, and filled them at its limit at the next open wherever the market had
  gone: a buy below the new bid, or a target far above the ask that cancelled the stop
  that should have fired. An order now leaves the queue while it cannot trade and joins
  again behind what is displayed when its session opens, and what a batch lets it fill
  but it cannot fill then is not kept for a later batch.
- **Stop stress no longer cancels the stop.** With `stop_stress_ticks`, the price band
  measured a plain stop's stressed price, so on the wide quote that made the market
  fast, enough stress put it outside the band: the stop was cancelled (`RISK_CHANGED`,
  `PRICE_BAND`) and the position left open. The band now measures the market's price,
  and the stop fills with its stress.
- **A re-armed stop drops its earlier stress.** A bracket stop that triggered in a
  fast market, sold what a thin bid showed and re-armed kept that stress, so an
  ordinary quote reaching it later sold the rest `stop_stress_ticks` ticks under the
  bid. Each trigger now measures its own quote.
- **Trailing stop-limits on the underlying.** A stop-limit whose trigger trails spot
  moved its option limit by spot's dollar move: a sell limit of 3.50 became 38.50
  after a 30-point rally, so the stop never sold, and a buy stop's limit would have gone
  below zero, so its level never trailed. The limit now stays where it was set while
  the level trails spot. The simulator also refuses a reference or a trail on a
  trigger on a study, the clock or another underlying, as the API already did.
- **A walking order's `then`.** A walking limit whose step came due at another
  command's market time (a definition, or a day's rollover ahead of its quotes) and
  filled there never placed its `then` order. It now places it in that transaction.
- **Limit flatten of a long nobody bids for.** A limit flatten left a single long whose
  quote showed only an ask working at 0.00 for a bid, while a market flatten and a
  spread's close give such a long away at 0.00. A limit flatten now gives it away too.
- **A split on a live feed.** A split applies at the rollover into its ex-date, 17:00 ET
  the evening before, but the feed goes on giving the old shares' prices until the
  ex-date: the next batch's pre-split close marked the split shares, doubling a 2:1
  holding's value (or cutting a 1:10 one by 90%) all night, and plan rules passed or
  failed the account on it. An adjusted series was marked the same way from its
  standard equivalent's pre-split quote. A price printed before the ex-date now marks
  the shares divided by the split's ratio, and a quote given before the ex-date's
  open does not mark or fill the split stock's series.
- **Option stops on a split stock.** An option order triggered by the stock's price
  kept its pre-split level, so after a 2:1 split a stop under the old price fired on
  the ex-date's first quote with no move, and a level above it could no longer be
  reached. Such levels (the order's own, its bracket's and its chained orders') and a
  trail in dollars behind them now take the split, to the cent.
- **A dividend under half a cent.** A dividend that rounded to $0.00 on the shares held
  (one share at $0.004, say) failed the rollover, and the account with it: every later
  batch skipped it and every write, closing the shares and a reset included, answered
  `TRADING_UNAVAILABLE`, on every restart too. It now pays nothing, and the
  `EX_DIVIDEND` warning shows the amount in whole cents.
- **A catch-up rollover across a dividend and a split.** A rollover covering several
  trading dates (openportd down, say) applied every split before every dividend, so a
  dividend going ex before a later split was paid on the split shares: twice the cash
  for a 2:1. Each ex-date's splits and dividends now apply in date order.
- **Dividend assignment of adjusted series.** Overnight early assignment weighed a
  call's time value against a share's dividend, whatever the series delivered: a
  2:1-adjusted short call that a dividend made worth exercising was never assigned, and
  a 1:10 one that it did not was. It now weighs what the dividend pays on one unit's
  deliverable, after a split going ex the same night, and so does the `EARLY_ASSIGNMENT`
  warning.
- **Interest on an account made before the first market data.** An account created or
  reset with a credit rate before the first quotes arrived was credited interest from
  1970 at its first rollover (about $207,000 on $100,000 at 3.65%), which could pass a
  custom evaluation outright. Financing now runs from the account's first trading date.
- **Portfolio margin with adjusted series.** Portfolio margin took the value of
  adjusted series it does not scan out of equity as well as their strategy margin: a
  long adjusted option counted back as buying power, so the account could buy them on
  unlimited credit, and a short one's buy-back was charged twice. Only what the scan
  covers is now taken from equity.
- **Risk profiles of adjusted series under smile dynamics.** Under sticky moneyness or
  sticky delta, the risk profile, and the multi-day, beta-weighted and term-scaled
  scenario grid built from it, read an adjusted series' moved smile at its listed
  strike instead of its effective one, so they disagreed with the one-day grid. Both now
  read it at the effective strike.
- **A byte-order mark in a dividends or corporate-actions file.** A file saved as
  Excel's "CSV UTF-8", or by a Windows editor, starts with a UTF-8 byte-order mark,
  which `--dividends` and `--corporate-actions` read as part of the first line: the
  header became a data line, or the mark joined the first symbol, and openportd would
  not start, blaming the symbol or the action kind. The mark is now skipped, as the
  broker-fill import already skipped it.
- **Broker-fill imports.** With `format` `auto`, the default, a generic file whose side
  column was named `buy/sell` (which the generic reader takes) was taken for an IBKR
  Flex query and refused for lacking `TradePrice`; IBKR is now known by its price
  column. Past the 5,000-fill cap, the "Only the first 5,000 fills are read" notice was
  added again for every later fill that followed a row skipped for another reason, and
  those rows were listed one by one; the notice now comes once, at the first fill past
  the cap, and reading stops there.
- **A second journal repair within one second.** `--repair-journals`, and the repair a
  continued replay makes, keep the original as `FILE.torn-` and the UTC second; a
  journal torn and repaired again within that second failed with "Cannot create
  journal repair backup: File exists" and stayed refused. The later backup now takes
  the next free name, `-1` and up after the stamp.
- **Reading each journal twice at startup.** openportd read, parsed and checked the
  hash chain of every account journal once to resume it and again for the records it
  recovers from (about 1.4 s more for a 46 MB journal); `--compact-journals` did the
  same. Each journal is now read once.
- **Share-order client IDs after a reset.** A share order's client ID kept naming its
  order across account resets: sending the same order after a reset placed nothing and
  answered 200 with the earlier attempt's cancelled order, and other terms under the ID
  were refused with `DUPLICATE_CLIENT_ID`. Client IDs are now scoped to the attempt,
  as option orders' are, so the ID names a new order.
- **Retrying a rejected share order.** A retry of a share order rejected as it worked
  answered 200 as if it had been accepted, and a retry of an order whose `oco` partner
  was refused got 409 `DUPLICATE_CLIENT_ID`. Both now get the original rejection.
- **A share OCO pair on held shares.** A target and a stop for the same shares held
  were checked as two sales: a cash account, an IRA or a buy-only plan refused the
  pair as a short sale, and a margin account reserved buying power for a short that
  could not happen. The pair now counts once: the larger of its two sales, and the
  reservation of whichever order would take more.
- **Correcting a rejected share order in the terminal.** After a working order from
  Trade shares was rejected, the dialog sent every corrected order under the same
  client ID, which the server refused with `DUPLICATE_CLIENT_ID` until the dialog was
  closed. Changed terms now go out under a new ID; the same terms keep it, so a retry
  is still answered once.
- **Working share orders outside the Trade shares dialog.** They were listed only
  there: after Close all sold the shares, a sell order left working opened a short
  sale and reserved buying power with nothing on Positions or Orders to show it. They
  are now listed, with Cancel, under Positions → Shares and on the Orders page, Close
  all names the ones it leaves and the short sales they would open, and the reserved
  buying power says it is held by working orders.
- **Share orders under the kill switch and plan rules.** The kill switch, a passed or
  failed attempt, a locked day, flat time and the end of the plan's trading window
  cancelled working option orders that could open but left share orders working and
  reserving buying power until a price reached them and an execution check rejected
  them. Share orders that could open are now cancelled with the same code; those that
  only reduce keep working.
- **A re-armed stop exit on a used-up quote.** A bracket stop that sold part of its
  position and re-armed for the rest triggered and re-armed again on every later
  transaction while the same quote stood with its size used up, adding two
  whole-order records to the journal each time. It now waits for a quote with size.
- **Pin-risk deliveries of adjusted series.** A pin assignment delivered shares at the
  series' effective strike rounded to the micro-dollar, so the cash missed the strike
  amount by a fraction of a cent and kept sub-cent digits. The cash now moves by the
  exact amount.
- **A late settlement and a newer share price.** A settlement applied after the fact
  (after a restart, or by hand on Monday) marked the delivered shares at Friday's
  close as if it were current, over the price the feed had just given, and a share
  close then filled at it. The settlement now marks the shares only over an older
  price, stamped with the time it printed.
- **An account reset after the day ended.** A reset made after the plan day's end but
  before its rollover kept that day's `TRADE_LIMIT` and `PROFIT_LOCK` latches and
  opening-trade count through the whole next day. They now end with the finished day,
  as the rollover would have ended them.
- **Order change histories.** Every change asked of an order stayed on it, and each
  change journaled the whole order, so re-pricing one order many times grew the
  journal with the square of its changes. An order now keeps its latest 50 changes
  and reports how many earlier ones it dropped (`changes_dropped`).
- **Strategy margin and leg order.** When the exact allocator gave up on a large book,
  the fallback's requirement depended on the order the legs were listed in (by up to
  4% on one book), so `margin_requirement` and `margin_breakdown` could answer
  differently for the same positions. Legs are now taken in symbol order, as the
  account's own book already gave them.
- **Margin rows during an incomplete scan.** Under portfolio margin with an adjusted
  series held, the snapshot's `margin` rows left out the option minimum that the
  requirement includes while the scan is incomplete, so they summed to less than
  `short_requirement`. They now include it.
- **A queued buy-to-close on an ask-only quote.** With queue fills on, an order's wait
  reason read the bid of a quote that had none, which is undefined behaviour: a
  random `QUEUE` reason, or an abort in hardened builds. Such an order now waits on its
  limit.
- **Long client IDs with an `oco` partner.** A share order whose `client_order_id` was
  61-64 bytes was refused once it had an `oco` partner, with a message saying IDs may be
  1-64 bytes. The partner's terms are now checked without the `:oco` suffix.
- **Share stops and after-hours prints.** A regular-hours share stop was reached by an
  after-hours or premarket print, and then sold at the next open at whatever price it
  printed. A stop is now reached only by a print from the regular session, its close
  included.
- **Forged state deltas in a journal.** Recovery trusted an array delta's length and
  the shape of an object delta's removals: in a hand-edited or foreign journal whose
  hash chain was recomputed, a length of -1 padded memory until openportd ran out, a
  large or fractional one recovered quietly with nulls or an emptied array, and a
  removal written as a string deleted that key. Recovery now refuses each as
  `JOURNAL_CORRUPT`, before padding anything; what openportd writes is unchanged.
- **Program fees and payout caps that lock an account.** A fee near the money range
  ($9.2 trillion) was accepted, after which every reset charging a fee, a same-plan
  reset that inherits it included, failed with `ARITHMETIC_OVERFLOW`; and a payout cap,
  or a payout review's cap, below the payout minimum blocked every payout it applied
  to for good, as "below the minimum". Rules set from now on take fees of at most
  $1,000,000 each and caps of at least the minimum (`INVALID_RULES`); journals and
  recorded runs from before keep theirs.
- **Event calendars on busy accounts.** With holding restrictions and a large event
  calendar (up to its 256 events), each market batch, opening-order check and flat-time
  pass worked out every event's window, entering the plan's holiday calendar and
  clearing the thread's business-day cache each time: about 1 ms a check at 256
  events. Only events whose window can fall in the time asked about are now worked
  out, and the calendar is entered only for them; the windows are the same.
- **Share round trips held through a split.** A trip's opened and closed share counts
  stayed on the pre-split basis while later fills counted split shares, so `GET
  /api/trades` and the Journal showed a trip bought as 30 at $300, split 3:1 and sold as
  90 at $110 as opened 30 at $300.00 and closed 90 at $110.00, a loss to the eye though
  it gained $900. A trip held through a split now counts its earlier shares in split
  shares: opened 90 at $100.00.
- **Tag and playbook exposure limits on long-lived accounts.** With any tag or
  playbook exposure override, every market batch, order check and preview rebuilt
  every round trip the account had ever traded to find the open ones' tags: at 400
  round trips a quote batch took about 12 ms and an order 60 ms. The open round trips
  now come from the ones the account keeps up to date as it trades; the whole history
  is read only for held shares an option delivered, or just after a restart.
- **Accounts with many share orders.** Share orders are kept for good, and every
  market batch, rollover and buying-power check scanned all of them, while each new
  order compared its client ID with every earlier one: with 20,000 ended orders a price
  batch took about 1.7 ms, and each order placed cost more than the last. Open share
  orders are now indexed as option orders are, and a client ID finds its order at once.
- **Saving personal guardrails after gains.** An unchanged absolute soft floor can
  be saved when the trailing plan floor has overtaken it, including with unrelated
  guardrail tightening. A newly requested soft floor still must exceed the plan floor.
- **Trade protection rules.** Already-reached triggers and marketable OCO legs no
  longer bypass minimum hold, and a take-profit or time trigger is held to it when it
  triggers; protective stops still close a young position. Under required-stop or
  trade-risk rules, an open position's bracket stop can tighten but cannot be moved
  farther away.
- **Open-loss caps.** Percentage drawdown room uses the day's opening floor, so
  intraday gains cannot shrink it to zero and immediately flatten new entries.
  A flatten stays pending through partial fills and price recovery until flat,
  refusing adds and retrying on later quotes, including after journal recovery.
- **Funded payout floors and scaling.** A starting-balance payout keeps its raised
  floor through later highs and day closes, independently of the plan's lock level.
  Static-floor reset payouts restore the full drawdown room below the reset balance.
  Size reviews add back unpaid reset removals and review forfeits as withdrawals.
- **Evaluation profit caps.** Equity targets now require a flat account when trade
  consistency or microscalping is enabled, so the winning trade cannot escape those
  checks by remaining open until automatic target liquidation. Replay drivers 1–6
  retain their original decisions; new runs use driver 7.
- **Rule comparisons count microscalping by round trip.** `POST /api/account/rules-compare`
  judged a whole trade as short only when its longest leg was, counted its net P&L, and
  left share round trips out. A 10-second leg that made $500, rolled into a one-hour leg
  that lost $100, counted nothing, so the account's own rules could replay to a pass its
  `MICROSCALPING` objective held back. Each option and share round trip now counts on
  its own, as it does on the account.
- **Rule comparisons and the inactivity rule.** `POST /api/account/rules-compare` never
  counted the attempt's executions toward `inactivity_days`, so every rule set with one,
  the account's own included, failed `INACTIVITY` once the attempt was older than the
  window, however often the account traded. The window now runs from the trader's own
  fills and share trades, and, as on the account, a trade on the first day past the
  window comes too late to keep it.
- **Covered calls in the strategy optimizer.** A covered call's share cost was charged
  twice, so every one was valued 100 × spot too low: doubled `max_loss`, a negative
  `max_profit`, no breakeven, `pop` 0, an `expected` P&L near −100 × spot, half the
  `quantity`, and over budget for any budget between the true and the doubled loss.
  The shares are now charged once, through `net`; a covered call to expiry loses at
  most (spot − bid) × 100 and breaks even at spot − bid.
- **Builds from source on a Mac with Homebrew's simdjson 5.** Homebrew installs its own
  simdjson, nlohmann-json and googletest headers in the directory Boost comes from,
  and the build searched it before the pinned copies, so the code compiled against
  Homebrew's headers but linked the pinned library. With simdjson 5 installed (it
  comes with Homebrew's Node) the result crashed parsing Cboe's chain. The pinned
  headers now come first.
- **Back-to-back fill calibrations.** A `POST /api/fills/calibration` sent as soon as
  the previous one was answered could get 503 `CALIBRATION_BUSY`, because the worker
  answered before marking itself free. It now frees itself first.
- **A request body cannot end the server.** A JSON body nested a few thousand levels
  deep overflowed an HTTP thread's stack on four routes (what-if, closing positions,
  rules comparison and rule-pack validation) and ended `openportd`. Every POST, PUT
  and PATCH body is now checked once before any route reads it: one that is not JSON,
  not UTF-8 or nested more than 64 deep gets 400 `INVALID_REQUEST`. A byte that is
  not UTF-8 sent to `/api/replay` or `/api/replay/packs` used to end the process;
  routes that answered 500 for a body they could not parse now answer 400.
- **Every request is answered.** A share-order preview whose cost passed the money
  range (10,000,000 shares at a $1,000,000 limit) threw while its answer was being
  built, so the request never answered and its connection was held for good; 512 of
  them shut the server to new connections. It now answers 422
  `ARITHMETIC_OVERFLOW`, and a request whose handler drops it unanswered gets 500.
- **One account's overflow stays on that account.** A balance near the largest amount
  an account holds, with an in-the-money option expiring today, overflowed the
  expiry-delivery projection behind the risk warnings. That stopped the feed for every
  account, answered a committed fill as an error and made `openportd` exit at each
  start. The account now shows a `WARNINGS_UNAVAILABLE` warning in their place.
- **Unreadable state files are named.** A `market-halts.json` or `playbooks.json`
  with bytes that are not UTF-8 (a UTF-16 or Latin-1 save) ended `openportd` seconds
  after it started, with a message that named no file. The server now keeps
  running and reports the file's path and the byte where it stopped being JSON.
- **Earnings labels.** An imported label longer than 64 bytes was cut inside a
  multi-byte character, after which `GET /api/earnings` answered 500 and
  `earnings.json` stopped being saved. Labels are now cut on a character boundary,
  and a label that is not UTF-8 is refused with its line number.
- **Event files must be UTF-8.** An `--events` or `--event-calendar` file saved as
  Latin-1 (Excel on Windows) loaded without a word, then volatility and
  `GET /api/calendar/events` answered 500 for the whole run. `openportd` now refuses
  such a file at startup, naming the file and line.
- **Replay refuses impossible contract IDs.** A recording that defined a contract
  with ID 4294967295 crashed `openportd` when replayed, and a very large ID made it
  allocate gigabytes. A definition's ID must now be below 500,000, the most
  contracts a recording holds; a file with a larger one is reported as malformed.
- **Replay refuses impossible contract definitions.** A recording whose definition
  carried a NaN strike replayed without a word, and that one contract made the whole
  chain's GEX, VEX and gamma flip NaN; a zero or negative multiplier or an expiry such
  as month 13 was taken too. A definition's strike and multiplier must now be positive
  and its expiry a real date, or the file is reported as malformed at that record.
- **Extended-hours share orders overnight and on weekends.** Closed, the session with
  premarket and after hours dated its market time to the last regular close (16:00, or
  13:00 on an early close) from midnight to 04:00 and all through weekends and
  holidays, though after hours had run on to 20:00 (17:00). A share price that was
  already stale at 21:00 therefore counted as fresh again at 03:00 or on Saturday, and
  an armed extended-hours stop could trigger on it. The market time is now the end of
  the last after-hours session.
- **A chart of nearly equal values.** A line chart whose values differed only in the
  last digit of a double (0.1 + 0.2 beside 0.3) never finished choosing its axis
  ticks and held the tab until it ran out of memory. The ticks are now counted
  rather than stepped to, and values that print alike share one tick.
- **Requests after an idle spell.** The first broker request or webhook sent after
  the server had closed an idle kept-alive connection failed with "Connection reset
  by peer" or "end of stream", though the server was up. tastytrade's OAuth refresh,
  about 14 minutes into a stream, dropped the feed and reconnected. A connection the
  server has closed is now replaced before a request goes out, a POST reuses one only
  within a second of the last request, and a GET that fails on a reused connection is
  sent once more on a new one.
- **HTTP to an IPv6 address, to a non-default port and through compressing proxies.**
  A URL with an IPv6 literal kept its brackets as the host, so `[::1]` never resolved
  (an IBKR gateway at `https://[::1]:5000/v1/api` passed the loopback check and never
  connected) and `http://[::1]/` split inside the address. The host is now the bare
  address, sent back in brackets in `Host`. Ports must be 1-65535, and a URL with
  `user:password@` is refused instead of reading `user` as the host. `Host` now carries
  the port when it is not the scheme's default (Theta Terminal, the IBKR gateway,
  webhooks on their own port), as RFC 9110 requires. A redirect, 204 or 304 without a
  body that still said `Content-Encoding: gzip` failed as a corrupt gzip stream, so
  such a redirect was never followed.
- **No other site can frame the terminal.** Any web page could load the terminal in
  an invisible frame and lay its own buttons over it. On the default loopback bind,
  where writes need no token, the framed copy's requests carry the server's own
  Origin, so the visitor's clicks could place orders, reset the account or trip the
  kill switch. Every response now carries `X-Frame-Options: DENY` and
  `Content-Security-Policy: frame-ancestors 'none'`, plus
  `X-Content-Type-Options: nosniff`.
- **The Brief counts the plan's daily loss limit.** Today's loss allowance used the
  risk limit's daily loss, the floor and the soft floor, but not the plan's own daily
  loss limit, the one that fails the attempt or locks the day. On `static-100k` it
  showed $8,000 of floor room while a $4,000 loss ends the attempt. The plan's daily
  loss room now counts, and the risk limit's part is labelled as such.
- **The ticket's Mid price is a valid limit.** On a contract whose tick is more than a
  cent (SPX and SPXW above $3, for one) with a spread an odd number of ticks wide,
  the Mid button filled in a limit between ticks, which the preview and the order
  refused with `INVALID_TICK`. Bid, Mid and Ask now snap to the contract's tick, down
  for a buy and up for a sell.
- **Trading over plain HTTP on a LAN address.** Browsers offer `crypto.randomUUID`
  only on HTTPS or a loopback address, and the terminal called it for every client
  order ID, transfer, alert and drawing. Opened as `http://<server-ip>:8080`, every
  order ticket failed with `crypto.randomUUID is not a function` and offered a retry
  that failed the same way. IDs now fall back to `crypto.getRandomValues`, and a
  ticket offers a retry only for an order it actually sent.
- **ThetaData imports keep trade venues.** `openportd --import-day thetadata` stopped
  at the first trade row with an exchange code, saying an OptionTrade venue needs a
  v5 header, and wrote no recording for that day or any later one in the range. The
  CLI's source did not pass on that ThetaData trades carry venues; it now does, and
  the import writes a v5 recording that keeps each print's exchange.
- **A visitor's sandbox events stay private.** On a public demo, anyone who knew a
  sandbox's ID (it shows in `?account=` links, logs and screenshots) could open
  `/ws/events` for it without a token and watch that visitor's fills, orders and
  rejections, though REST and ticks hide sandboxes. Its stream now opens only for
  the sandbox's own token or an authenticated admin; anyone else gets 404
  `UNKNOWN_ACCOUNT`, as from its REST routes.
- **Closing a tastytrade stream.** Closing the DXLink connection waited out a
  one-minute idle timer, so stopping the feed or `openportd` took a minute (as long as
  systemd's stop timeout), and every hourly chain refresh, quote-token refresh and
  reconnect after an error went a minute without data. It now closes at once.
- **Massive imports with a strike window.** With `--window` above 0, every Massive
  quote rebuilt the put-call parity estimate behind the window from every quoted
  contract, so an import ran about 70 times slower than with `--window 0`. Once an
  underlying has an estimate, quotes now refresh it once per market second.
- **The 4 MiB import limit needs credentials first.** `POST /api/analysis/portfolios`
  may send 4 MiB where every other route takes 64 KiB, and the larger limit was
  granted before the request was authenticated. A client without a token could open
  hundreds of connections that each held about 4 MiB until their reads timed out,
  about 2 GiB in all: enough to restart a demo server, which deletes every visitor's
  sandbox. The import's Host, Origin, credentials and Content-Type are now checked
  from its header, and a request the server would refuse is answered before its
  body is read.
- **A resting order no longer journals every drain of a streaming feed.** While any
  account held a position or an order, the live engine re-ran the chain analytics and
  wrote, and fully synced, a market transaction on every drain of its queue, even when
  nothing the account trades had changed: about 90 records a second on a busy feed,
  reaching the 100,000-record large-journal warning within half an hour. A quote of a
  contract an account holds, works or has an alert on, a price of its shares, a halt,
  an official close or a completed snapshot still reaches it at once; the rest of the
  market reaches it with the next analytics pass, once a second. Demo and replay runs
  are unchanged.
- **Editing an account group keeps its archived members.** The editor listed and
  saved only accounts that could join a new group, so saving a group with an
  archived member dropped it without a word, and copied orders no longer reached it
  once unarchived. Such members now stay as checked rows marked archived; unchecking
  one is the way to remove it.
- **Rolled strategies stay strategies.** After rolling a straddle, strangle, risk
  reversal or ratio spread, or a whole condor as one eight-leg order, Positions no
  longer showed the new legs as a strategy: they lost their label, net P&L, risk
  profile and the Close, Roll and Exits buttons. Any roll the Roll dialog builds now
  keeps the legs it opened together.
- **Rolling a vertical by its width.** A same-expiry roll that moved a vertical by
  exactly its width named one contract in two legs, which the server refused with a
  generic message. The Roll dialog now says that a new strike is one the strategy
  already holds before anything is sent.
- **Sandbox exits have a budget.** A visitor's cancels, closes, exercises and
  abandonments were exempt from the sandbox's 60-requests-a-minute limit, but the
  engine journals one it refuses as it does any command. Cancelling an order that
  does not exist, over and over, grew the sandbox's journal without end, about 450
  bytes a request, on a volume every account shares. Exits now have a budget of
  their own, 60 per rolling minute, so a visitor who has spent the order budget can
  still close out; past it they get 429 `SANDBOX_ORDER_RATE` with `Retry-After`.
- **The backtest list is cheap to poll.** Every `GET /api/backtests` parsed each
  saved run's whole `report.json` (about a second with twenty long runs) while holding
  the lock a running backtest reports its progress through, and the Backtest page asked
  for it every second even with nothing running. The server now keeps what the list
  shows of each report until the file changes, and the page polls every second only
  while a run is active, otherwise every 15 seconds.
- **Faster ThetaData imports.** `openportd --import-day thetadata` sent one request for
  each underlying and two for each expiry in every fifteen-second window, each after a
  100 ms pause: about 4,900 requests and eight minutes of pauses for one SPY expiry,
  hours for SPX. Each is now requested in blocks of up to an hour (25 requests for
  that SPY day), a block too large for Theta's limits is split, and a request waits
  only when Theta Terminal answers 429.
- **Analyze breakevens past a custom date.** With a custom date later than the
  candidate's first expiry, the slice table's BE rows came from the custom date's
  curve, not the expiry's, so they moved or vanished while the chart still drew the
  expiry curve crossing zero. They now always come from the expiry line.
- **Replay from here on a live trade.** Back on the live account with a replay still
  running, a live trade's Journal review offered **Replay from here**, which ended
  the running replay and started a new one before the replay account's trade with
  the same number, on another contract. The live journal also drew the replay's
  scenario events on its trades. Both now use a replay only on that replay's own
  journal.
- **Stopping a replay from the Replay page.** The page kept a “Now replaying” panel
  for the stopped run with its Pause, Stop and step buttons enabled (Pause then
  failed with `NO_REPLAY`), and **Trade this replay** switched the terminal back to
  the dead run. When the run was being traded, the terminal left it saying it had
  stopped in another window. The run is now forgotten once the stop is answered, the
  notice says the replay stopped, and a run stopped elsewhere leaves the page once the
  server no longer lists it.
- **Playbook reports with many trades.** `GET /api/playbooks`, which the terminal
  reads every three seconds, rebuilt every round trip of the account and searched all
  its fills again for each entry it scored, so its cost grew faster than the square of
  a playbook's trades: three hundred round trips took over a second. A read now builds
  the round trips, each order's fills and each playbook's counted entries once, and
  scores the current attempt and the forward tests in one pass; the engine's playbook
  checks and per-playbook pass odds no longer repeat that work either. Reports are
  unchanged.
- **Replay history listings.** `GET /api/replay` read every saved run's whole journal
  on each call to estimate its verification time, and after a restart recovered every
  saved run while holding the history lock: with a hundred saved runs, the first
  listing stalled every other request for tens of seconds. What a listing reads of a
  journal is now kept until the journal's size or modified time changes, in memory and
  in `ID.jsonl.summary` beside it, so polling and restarts read only journals that
  changed. The first listing after an upgrade still reads each journal once.
- **A working order's result stops taking the keyboard.** After an order rested,
  every change to the account (each quote move on its contract) moved focus to the
  ticket's result and scrolled the page to it, twice, and briefly showed the status
  from when it was sent. The result now takes focus once, keeps its last refreshed
  copy while the next loads, and fetches just that order instead of every order.
- **The order preview follows the market.** A ticket's preview (what fills at once,
  buying power after, maximum loss, suggested size) was checked again only when the
  account changed, and an account with nothing on the contract does not change as
  its quotes move. A ticket left open kept the preview from when its terms were last
  edited. It is now checked again as quotes move, at most every three seconds, and
  keeps the last answer up while the next loads.
- **Streaming feeds outlast long outages.** A tastytrade feed stopped for good after
  eight failed reconnects, about three minutes of outage (a router reboot, a laptop
  asleep, broker maintenance), and a Databento stream after eight in about two; only
  a restart brought either back. Both now retry for as long as they run and report
  each failure as `error`, waiting at most a minute (tastytrade) or 30 seconds
  (Databento) between attempts.
- **tastytrade sessions whose spot drifts.** With `--window`, every contract the window
  had once admitted stayed subscribed, so a session whose spot drifted far enough
  passed DXLink's 25,000 subscriptions and the feed stopped. Contracts that drifted
  out of the window now give up their subscriptions, farthest from spot first, when
  the cap needs the room.
- **tastytrade spot.** An underlying quote without a two-sided market (one-sided
  before the open, or empty) or a trade without a price set the underlying's spot to
  zero until the next priced event. Such events are now ignored.
- **tastytrade under a busy stream.** The adapter rebuilt and compared its whole
  subscription set, up to 25,000 entries, after every DXLink message, and walked the
  full chain on every underlying tick: 15 to 40 ms each on a large chain, enough to
  fall behind a few dozen messages a second. The set is now rebuilt only when the
  subscribed contracts change, and strikes are selected again only when spot moves
  the window past one.
- **tastytrade's hourly chain refresh keeps the stream.** Every hour the adapter closed
  DXLink, reported every underlying `connecting`, fetched the chains, handshook again
  and subscribed everything anew, at most 10,000 subscriptions a minute, with no
  quotes meanwhile. Chains are now fetched again on the open stream, one underlying at
  a time, and DXLink is sent only what changed; the stream reconnects only to renew its
  quote token or after a failure.
- **tastytrade health with timed and untimed quotes mixed.** An underlying's state was
  each option quote's own: `live` when both sides carried a time, `stale` when a
  priced side had none, so contracts of both kinds flipped it on every quote, one
  status event each through the queue and into recordings. It is now `live` once a
  timed quote arrives and turns `stale` only after a minute without one.
- **The terminal says when it loses the server.** With `openportd` stopped or out of
  reach, the header kept a green pulsing **live** badge, a demo or replay feed stayed
  “<1 min old”, and the ticket said an order would fill now at the ask with Buy
  enabled, ending in the browser's “Failed to fetch”. The badge now reads
  **disconnected**, the age counts on the wall clock, a banner says when the
  connection dropped, tickets refuse to send while the server cannot be reached, and
  a request that got no answer says the server could not be reached.
- **Cboe when a quote page fails.** While the CDN file was stale and the adapter read
  Cboe's quote page instead, one failed page read (a 503, a 429, a page without the
  chain) reported `cboe SPX: HTTP 0` and then published the stale file, hours old, for
  five minutes, taking quotes and spot back to the file's time. A failed page now
  reports its own status and is read again 45 seconds later, the hold on the page goes
  on, and a chain older than the one already published is never published.
- **Cboe chart history after a server error.** A 500, 502, 503 or 429 for a daily
  chart file left the next attempt an hour away, so one error at startup cost charts
  and the realized-vol fallback their daily bars for an hour, while a network or parse
  error retried after a minute. Every failure but a missing file (403, 404) now
  retries after a minute.
- **Open terminals stop re-downloading the fill history with every quote batch.**
  While an account held a position, each new account version made every open tab
  fetch the account's whole fill list and current-attempt trades again for its fill
  and delivery alerts: about 4.5 MB a batch for an account with 1,500 round trips. The
  trading status in `/api/status`, ticks and the account list now carries
  `history_version`, the account version at which its orders, fills, share fills,
  dividends, cash flows or settlements last changed, and the terminal refetches the
  fills and the delivery alerts' trades only when it moves.
- **The terminal keeps fewer old copies of market data.** Chain, summary, surface and
  the other market queries key on their underlying's data version, which changes with
  each one-second tick, and every superseded version stayed in the browser for the
  query library's default five minutes: about 300 copies of each chain on the page.
  They now go five seconds after the page moves on to a newer version.
- **One page's error no longer blanks the terminal.** Nothing caught a render error,
  so one (an unexpected payload, or a what-if candidate saved by another build without
  a name) emptied the whole page, navigation and kill switch included, and a bad saved
  candidate did it again on every reload. Each page now shows its error in its place
  with Try again, Reload and Reset saved layout, which clears only this browser's
  saved drafts and layout. Saved what-if and Analyze candidates are checked field by
  field, and malformed ones are dropped.
- **Feeds that report `stale` while healthy.** Tradier's index chains, Schwab
  responses without `isDelayed` and Interactive Brokers' frozen or unreported values
  report `stale` after every successful poll, and the engine never counted those polls
  as updates. A minute in, the status read `no successful update within 60 seconds`
  with `last_success` empty, and a `feed_stalled` notification went out at the first
  poll and never rearmed, so a later real outage sent none. Such polls now count as
  updates, and the `stale` label alone is no longer a stall.
- **Volume and closes across a feed failover.** The fallback kept only the muted feed's
  quotes, open interest and Greeks; its volume counts and official closes were
  dropped, and a switch did not send them. After a switch, or the return to the main
  feed, a contract kept the other feed's volume until its own count changed (all day,
  for one that stopped trading), and a Cboe close sent while Cboe was muted for that
  underlying was lost for the day. Both are now kept for each feed and sent with a
  switch.
- **Interactive Brokers outside the session.** An underlying whose last price came
  marked `C` (its previous close) or `H` (halted) with no bid or ask, as SPX always
  has none and a stock overnight, stopped the whole chain at every poll with `no
  underlying price yet; the gateway is warming up its market-data line`: nothing was
  defined or quoted. That price now centres the strike window, so the chain is mapped
  and quoted, without being published as the underlying's price. A row with no price
  at all now says so instead of blaming the warm-up.
- **A partial early exercise shows in the Journal.** When part of a long was exercised
  early and the rest sold, the trade read “Contracts in / out 5 / 5” beside fills that
  closed only 4, and nothing in `/api/trades` or the CSV exports named the exercised
  contract. Each round trip now lists every closure, partial ones included, as
  `closures` (kind, quantity, price, fee and time) in `/api/trades` and as a last
  `closures` column in `trades.csv`, and the trade detail shows each as a row.
- **State files are replaced whole and synced.** Thirteen stores wrote a temporary
  file and renamed it over the old one, each in its own way. The equity history's
  compaction (the only copy of an account's minute curve), imported analysis
  portfolios, replay metadata, scenario packs, the earnings history and rewritten
  surface-history months synced nothing before the rename, so a power loss or kernel
  panic soon after could leave the file empty or cut short, and a new replay's
  metadata was written in place. The others used `fsync`, which on macOS leaves the
  data in the drive's cache. One writer now serves them all: it creates the temporary
  file afresh, owner-only and never through a symlink, syncs it to the drive
  (`F_FULLFSYNC` on macOS) before the rename and syncs the directory after it.
  Rebuildable caches (chart candles, replay summaries, recording indexes) skip the
  syncs.
- **State files are owner-only.** Under a shell's umask 022 (Homebrew services and
  runs from a terminal; the systemd unit sets 0077), the equity history beside each
  journal, imported analysis portfolios (a real brokerage account's holdings, which
  the API shows only to admins), `playbooks.json`, `earnings.json` and replay metadata
  were written readable by every local user, in 0755 directories, beside an
  owner-only journal. State files are now written 0600, appends included, and the
  directories openportd creates 0700. Imported portfolios, `playbooks.json`,
  `earnings.json` and equity histories an earlier version left readable are tightened
  when they are next opened.
- **One unreadable earnings row no longer costs the rest.** `earnings.json` was read in
  one pass that stopped at the first row this build could not read (a date such as
  `2026-1-28` from a hand edit, or another version's file): the rows after it were
  dropped, and the next capture, settlement or import rewrote the file without them,
  though a recorded implied move cannot be measured again. The load error also stayed
  in `/api/earnings` after the rewrite succeeded. Each row is now read on its own and
  only unreadable ones are skipped. Before its first rewrite, a file that was not read
  in full is kept as it was beside it, as `earnings.json.unread-TIME`, and the error
  then names that copy; a successful save otherwise clears the error.
- **Demo replays no longer use a guessable temporary directory.** The Replay page's
  demo days went to `$TMPDIR/openport-demo-PID-N`, N counting up from 1. Another local
  user who created those names first made `GET /api/replay` and demo starts fail with
  `REPLAY_FAILED` once per name in a shared, sticky `/tmp`, or had openportd reuse a
  directory it did not make. The directory is now made by `mkdtemp`, with a random
  name, exclusively and owner-only, as the demo feed's already was.
- **The demo restarted mid-day keeps a day's authored previous close.** A day that
  authors its previous close (`stress-rehearsal`) sends it with its first batch. After
  a restart part way through the date, the batches before the saved time are replayed
  unsent to rebuild the book, and that close was dropped with them, so a playbook
  measuring from the prior close or a gap found none. It now goes out with the first
  batch after the restart.
- **The demo starts without generating a second day.** To find the first day's
  previous closes, `openportd --provider demo` generated, compressed and read back the
  whole day before it (about 95 MB) beside the first one, roughly doubling startup
  work, and threw it away. Those closes now come from that day's prices alone, with no
  option priced and nothing written, and a first day that authors its previous close
  needs none.
- **One visitor can no longer hold every sandbox.** The per-client limit counted
  creations in the last hour, and using a sandbox keeps it alive, so one address that
  created 3 an hour and used each within the idle period held all 20 slots of the
  public demo after seven hours, and every other visitor got `SANDBOX_CAPACITY`. A
  client may now also hold at most 3 unexpired sandboxes at once, pending ones
  included; past that, creation returns `SANDBOX_CLIENT_RATE` without `Retry-After`.
- **A torn surface-history or series row no longer stops the month's recording.** An
  append cut short by a crash, a kill or a full disk left a final line without its
  newline, and from the next start every later surface snapshot (or minute row) of
  that symbol and month was refused, on disk and in memory, until the month ended or
  someone cut the line by hand; only `last_error` said so. At the next start such a
  row is now cut, after the file as it was is kept beside it as `FILE.torn-TIME`
  (as journal repair does), and recording goes on. A bad row anywhere else still
  refuses appends.
- **Massive pages stay on Massive.** The chain and dividends polls fetched whatever
  `next_url` a page named, with the API key in the request's `Authorization` header:
  a page naming another host, or plain `http://`, would have been sent the key, since
  each page is a request of its own and not a redirect the HTTP client strips it from.
  A `next_url` naming the same page was fetched until the 2,000-page limit, every
  poll. A next page must now use the same scheme, host, port and path as the one
  before it and differ from it, as the historical importer already required; the
  poll otherwise fails with an error and keeps the last chain.
- **A ThetaData response cut off mid-row no longer delists the rest of the chain.** The
  NDJSON reader skipped an incomplete last record without an error, so a 200 response
  without a length whose connection dropped inside a row (through a proxy, say) read
  as a whole, shorter snapshot, and every contract after the cut was published at bid
  and ask 0. Such a response is now an error, and the last complete chain stays.
- **ThetaData on a delayed plan.** The feed always reported `live` and real-time, so on a
  delayed entitlement the header said real-time, the freshness check called
  15-minute-old data stale, and the delayed-feed fill model was never suggested. The
  feed is now plan-dependent, like Massive: from a minute into the regular session an
  underlying whose newest data runs more than 10 minutes behind the clock is reported
  `delayed`, otherwise `live`, and outside the session the last finding stands.
- **ThetaData rows that cannot be.** An expiration such as `2036-13-45` or `2036--1-05`
  defined a contract with an expiry that does not exist, and a negative bid, ask or
  size was published as it came. Such an expiration is now refused, and a negative
  price or size reads as none.
- **ThetaData open interest after a failed poll.** Open interest is fetched every 30th
  poll, but the count moved on before any request, so a first poll that failed (Theta
  Terminal still starting) or one failed open-interest request left the chain without
  open interest, and exposure without it, for the next 29 polls. It is now asked for
  again on the next poll.
- **What-if candidates added in two tabs are both kept.** Each tab kept its own copy
  of an account's what-if list and saved the whole copy, so with Positions open in
  two tabs, whichever added a candidate last dropped the other's after a reload. A
  tab now drops its copy when another tab saves the list (or clears storage), and
  reads the other's before it shows or changes it.
- **The publishing workflows pin their actions.** The release, image and Pages
  workflows, which run with tokens that can upload release archives and checksums,
  push the image users pull as `latest` and deploy the site, named third-party actions
  by movable major tags (`docker/build-push-action@v7`). Each is now pinned to the
  commit of its current release, with the version in a comment, and Dependabot keeps
  the pins current.
- **The risk limits editor keeps each override's caps with its row.** An additional
  cap typed for an underlying override was stored by symbol, apart from the row, so a
  negative cap left on a row that was then removed kept **Save limits** disabled with
  no message, and caps typed into a new row vanished once its symbol was entered. Each
  row now carries its own caps, only the caps that will be saved are checked, and an
  invalid cap is marked and explained.
- **Terminal links naming an object property.** A hash such as `#/SPX/constructor`
  or `#/SPX/toString` resolved its page through a plain object of old page names and
  got the object's own method back, so the terminal showed the chain instead of the
  Dashboard, titled the tab "undefined · OpenPort", and the next expiry change wrote
  the method's source into the address. Only the old `portfolio` link is an alias now;
  any other unknown page opens the Dashboard.
- **Copy diagnostics names Schwab, Alpaca and IBKR.** The report and the prefilled
  issue said `Provider: other` for those three feeds, and for any failover, because
  only the other providers were on its list and a failover's `primary+backup` name
  matched none. Every provider openportd can run is named now, and a failover reports
  both of its providers.
- **A finished run with an unreadable completion time no longer breaks the Replay
  page.** Today's practice took a drill's last completion without checking that its
  `completed_at` parsed, so one edited or damaged replay sidecar made the page fail to
  render. Such a completion is skipped, as the practice streak already did.
- **The custom plan form takes whole numbers as digits only.** Its whole-number
  fields (open-loss, minute-volume and best-position caps, review hold and trade
  share, reset percent, maximum resets, news minutes, payout days and percents and
  the like) read text as JavaScript numbers, so `1e2` saved 100, `0x10` 16, `0b11` 3
  and `5.` 5 while the field kept showing what was typed. Anything but digits is now
  refused with the field's message.
- **A limit keeps its time in force after a look at Market.** Choosing Market on the
  order ticket stored IOC as the ticket's time in force, so switching back to Limit
  sent a Day or GTC limit as IOC, which is cancelled at once unless marketable. Market
  orders still go as IOC; the limit's own choice now survives the switch.
- **The order ticket dialog after an order is sent.** Below 1280 px the ticket is a
  dialog, and once an order was sent its only actions were Close and a **Cancel** link
  that read as cancelling the order but only closed the dialog, leaving the order
  working; Bid, Mid and Ask still changed the sent order's limit field. The dialog now
  offers **Another order** and **Done** as the docked ticket does, drops **Cancel**
  once the order is sent, disables the quote buttons, and says that a working order
  is cancelled or changed on the Orders page.
- **A required stop in the overnight and curb sessions.** Under a plan that requires
  a stop-loss, an opening ticket with the default Day time in force said to add a
  protective stop, but in those sessions the stop control appears only for GTC, GTD,
  EXTO or GTC + EXTO. The notice now names those choices.
- **Streaming feeds are no longer stale at every close.** A stream (Databento,
  tastytrade) proves itself only with quotes, so a minute after each session's last
  quote every underlying showed `stale` and sent a `feed_stalled` notification, after
  every close, weekend and holiday, while the paper gate rightly saw a closed market.
  A stream is now held to the timeout only while its market is open, and a session's
  first quote has the whole timeout from the open.
- **A feed's error keeps its message.** An underlying whose feed reported an error
  (rejected credentials, a missing entitlement) turned `stale` a minute later with
  `no successful update within 60 seconds`, in its state and in the feed headline;
  the provider's cause survived only in `last_error`. It now keeps `error` and the
  provider's message.
- **The demo and replays report their engine rates.** `events_per_second` and
  `analytics_ms` in `GET /api/status` stayed 0 for the demo market and every replay,
  as their batches bypassed the live loop's analytics pass; each batch now counts as
  one, and the rate falls to 0 while a replay is paused.
- **A status read no longer stalls the engine behind a recording write.** With
  `--record-dir`, reading the status held the engine's lock while it waited for the
  recorder, which a provider thread holds through each frame write and a day's
  rollover, so the engine paused its publication for the length of the disk write. The
  recorder is now read first.
- **A failed update check keeps the release it already knew.** With update checks on,
  each daily check first cleared the release the previous check had found, so a check
  made offline, rate-limited by GitHub or answered with something unreadable hid the
  "is available" notice for another day. The known release now stays until a check
  names the latest one.
- **A new attempt takes the chosen plan's margin.** In **Start a new attempt**, margin
  settings edited under one plan stayed when another plan, Custom or a different
  **Start from** was chosen, and went out as explicit overrides: a cash plan chosen
  after editing house margin started as a margin account. The dialog now starts margin
  again from each plan chosen, as **New account** already did.
- **Token files must be their owner's alone, and an unreadable one is not replaced.**
  `--token-file` was read whatever its permissions, so a file an editor left 0644
  handed its admin and trade secrets to every local user without a warning. It must
  now be a regular file owned by the user openportd runs as, readable by no one else
  (`chmod 600`), as `--notify-config` already had to be; otherwise startup fails and a
  reload keeps the last good set. The same holds for a `--write-token-file` that holds
  a token. One that existed but could not be read (a root-owned file left by
  `sudo openportd`, or mode 000) was taken for an empty one and replaced with a new
  random token, breaking every saved link, script and MCP configuration; openportd
  now refuses to start and says why.
- **One far-future minute no longer wipes an underlying's chart history.** The chart
  store pruned minutes against the newest it had, so one quote or vendor bar stamped
  days ahead (a millisecond time read as seconds) erased every real minute before it,
  in memory and on disk at the next start, and later minutes were no longer saved. On
  a live feed a minute starting more than five minutes past the system clock is now
  ignored, read back from disk included, so a file left with one heals at the next
  start. Demo and replay time can run ahead of the clock and is not bounded.
- **A container no longer keeps a killed run's demo days.** The startup sweep of
  orphaned demo feed directories kept any named for the starting process's own PID.
  In the image openportd is PID 1 at every start, so each hard kill (an OOM, a stop
  past the grace period) of a container restarted by its restart policy left another
  160-190 MB directory in its writable layer. At startup, before the feed makes its
  own, directories named for that PID are now removed too.
- **The strategy optimizer searches when asked.** Pressing **Search** again with the
  same terms made no request, so the candidates kept the prices of the first search
  for as long as the page stayed open; they now say which quotes they used, and each
  press searches again. Switching to another symbol whose summary was already loaded
  kept the form and searched the new symbol at once with the old symbol's target
  price (SPY valued for 7000); another symbol now starts a new form.
- **A multi-leg order's underlying trigger in its history.** Changing the trigger of a
  closing combo armed on the underlying showed the index level as a net price in the
  order's history: "trigger $5,800.00 db → $5,790.00 db". Only a combo trigger's level
  is a debit or credit now; others read "$5,800.00 → $5,790.00".
- **Close all lists only what it will close.** Between an option's expiry and its
  settlement, a long that covers an expired short stays until that short settles, and
  the server sends no order for it. The Close all and Flatten dialog still listed it as
  "Sell N at market" and counted it on the button, which did nothing when it was the
  only position. It now pairs shorts with their covers as the server does, lists only
  the contracts that will close, and says which longs wait for the short to settle.
- **The Homebrew service no longer logs the token link.** It ran without
  `--no-token-link`, so every start (each `keep_alive` restart included) wrote
  `http://127.0.0.1:8080/#token=…` to `$(brew --prefix)/var/log/openport.log`, which
  launchd creates readable by every local account, handing them write and admin
  access. The service now passes the flag, so the log shows the `--print-url` command
  the caveats already give, and a fresh install makes `var/openport` owner-only.
- **A walk's default step and cap follow the ticket.** The walk step and cap were set
  once, when the ticket opened: flipping a ticket from Buy to Sell kept the ask as the
  cap, which then pointed away from the market and blocked the order; a ticket opened
  before its quote kept an empty cap; and a strategy whose legs changed kept the old
  combo tick as the step, which the server refuses with `INVALID_TICK`. Until you type
  them, the step and cap now follow the ticket's side, quote and legs.
- **The Journal names every reason the simulator closed a trade.** An expanded trade
  closed by the plan's flat time, a news or holding window, the no-overnight rule, the
  open-loss cap, a daily-loss lock, a time limit or inactivity read "Closed by the
  simulator's  liquidation", with the reason left blank, and its row badge showed the
  server's raw label. Each reason now has its own words, a flatten is no longer called
  a liquidation, and a reason this build does not know is shown as sent.
- **`tools/demo_soak.py` no longer reports a soak that never ran as a probe fill.** Exit
  status 1 means a probe filled, but a server that refused the connection, timed out,
  dropped it or answered with something other than JSON, a summary with no expiries,
  a chain with no leg priced yet, or a refused `PUT /api/replay` speed ended the
  script in a traceback, which Python reports as status 1 too. Each now stops the soak
  with a one-line reason and exit status 2, as its documentation says.
- **The Python replay examples run against a default local server.** `put_spread.py`,
  `last_hour.py` and `bot_replay.py` read `OPENPORT_WRITE_TOKEN` unconditionally and
  stopped with `KeyError` when it was unset, as it is for a loopback server with open
  writes, where a placeholder token fails too (`WRITE_TOKEN_REQUIRED`). They now send a
  token only when one is set. `bot_replay.py` also stepped the replay while it was
  still fast-forwarding to its start, so it always failed with `REPLAY_FAST_FORWARD`
  and left the replay running, which made the next run refuse to start; it now waits,
  as `last_hour.py` does, and stops a replay whose run failed.
- **The Python bot runner keeps its event stream through a burst.** Events went into a
  256-slot wake-up queue drained only between ticks, and the 257th undrained event (a
  slow hook, or `on_start` stepping a replay through many fills) raised `QueueFull`,
  which ended the stream for the rest of the run and was reported as a streaming
  failure in `event_error`. A full queue now drops the wake-up, which carries nothing
  REST does not reconcile. History runners also called `on_tick` with the equity
  sample a fill produced before `on_fill` for that fill; each fill now comes just
  before the sample that names it, as a live runner sees them.
- **What-if results match the candidates on screen.** The comparison table stayed up
  when a ticket or the hedge panel's **Add to what-if** changed the candidate list,
  showing a changed candidate's name with numbers from before the change, and a
  **Compare** answer that arrived after a candidate was removed brought the removed
  one back. Results now show only while the list is the one they were computed for.
- **What-if candidates say what a conditional order does.** A stop or other triggered
  order was listed and named "at market", like an order that trades at once, and its
  bracket and walk were left out. The order text now adds the trigger ("when bid ≤
  $4.20"), the walk's cap and the bracket's exits, and a default name is cut to the
  64 bytes the server takes.
- **The Journal and Orders pages keep up with a long history.** Both recomputed what
  they show from the whole history (trade grouping, statistics, calendar, reports, the
  order and fill searches) on every live tick, and the Journal's "Coach flag" report
  sorted every trade's peers once per trade, which took seconds per tick with a few
  thousand closed trades. Each is now computed once per change of the data or filters,
  and the coach report finds every peer group's median size in one pass.
- **The chart no longer offers a VWAP it cannot draw.** Candles carry no volume from
  any provider, so ticking VWAP only ever showed a note. The study now appears once
  the loaded bars carry volume.
- **The chart's study period takes typed values.** The Period field applied only whole
  values from 2 to 200 and threw every other keystroke away, so it could not be
  cleared and 14 could not be typed digit by digit (the 1 was dropped and the 4 made
  204). The field now keeps what is typed, applies it once it is a whole 2–200, and
  returns to the period in use on Enter or when it loses focus.
- **Journal rows and Flow expiries open from the keyboard.** A trade, strategy, whole
  trade or share row in the Journal, and an expiry in Flow's net premium table, opened
  only on a mouse click: nothing in the row took focus, so a keyboard or screen reader
  could not reach a trade's fills, review, chart, notes and tags, or an expiry's
  strikes. The row's name is now a button that opens and closes it and says whether it
  is open; a click anywhere on the row still works.
- **The exposure heatmap keeps your scroll position.** Each time spot crossed to a new
  nearest strike, the **By strike and expiry** grid jumped back to centre the spot
  row, even after you had scrolled elsewhere. It now follows spot only until you
  scroll it.
- **The Positions table's realized P&L is exact and says it is net.** The column took
  fees from realized P&L in binary floating point, so a sub-cent fee (from an itemized
  fee schedule) could show a cent less than the same net everywhere else, $39.31
  instead of $39.32. It also read "Realized", like the Realized tile and the sidebar,
  which show P&L before fees, so one label had two meanings. The column is now "Net
  realized", subtracted exactly, and its tooltip gives the gross, the fees and the net.
- **MCP refusals keep the server's evidence, and tools say what they change.** A
  refused order came back from the MCP tools with its code and message only, though
  the server sends `actual`, `limit`, `scope` and, for some checks, `evidence`, so an
  agent told `PRICE_BAND` could not see the band or the price without another call.
  Those fields now go with the error. Every tool also carries MCP annotations (reads,
  previews and what-ifs read-only; cancel, flatten, stop, archive, delete, modify and
  note replacement destructive), the tools that take a `change`, `terms`,
  `definition`, `pack`, `settings` or `candidates` object list the keys they accept,
  and `calendar_events`, which reads openportd's own calendar, no longer takes an
  `account` or `replay` it ignored, nor labels that calendar with a running replay's
  feed and time. `candles`, `surface` and `strategy_template` drop an `account` they
  never sent.
- **No stale install metadata in the repository.** `python/src/openport.egg-info`, an
  old editable install's metadata for a distribution named `openport` 0.1.0, was
  tracked though ignored, so anything that put `python/src` on `sys.path`, as the
  Python tests do, saw a phantom `openport` 0.1.0 beside `openport-client`. It is no
  longer tracked, and a test keeps such metadata out.
- **Numbers and times read the same on every page.** The positions table showed
  dollar Greeks as bare numbers (`57422.55`, `-104.64`) beside money columns; compact
  dollars such as GEX used an ASCII hyphen where signed money uses a minus sign; the
  Orders and Fills time column was on a 12-hour clock with no zone; the expanded
  watchlist showed its market time as a raw UTC timestamp; and a resting limit's hint
  could state one price to the cent and the other to the tenth of a cent ("$1.50" and
  "$0.500"). Dollar Greeks are now money, compact dollars take the minus sign, order
  and fill times are on the 24-hour New York clock with "ET", the watchlist time is in
  New York time, and both prices of a hint share one precision.
- **Line and bar charts for screen readers and the keyboard.** The equity, risk
  profile, payoff, volatility history, backtest and exposure charts had no accessible
  name and showed their values only on pointer hover. Each chart is now named (by its
  series, or by its bars' range), takes focus, and the arrow keys, Home and End step
  through its points while a live region reads each one out. A draggable marker's
  slider also states its range.
- **Positions, orders and tickets on a phone.** On a 375-pixel screen the positions,
  orders and fills tables scroll sideways for a screen or more, and scrolling to a
  row's Close, Edit or Cancel moved its contract out of sight, so with several rows the
  button was easy to press on the wrong one; an order ticket's submit button sat more
  than two screens down its sheet. Each row's contract now stays at the left edge while
  the table scrolls, and the ticket's submit stays at the foot of the sheet.
- **A one-cancels-other order with an underlying level says when it arms.** The
  ticket's chained OCO order read "Works beside this order" whether or not it waited
  for the underlying to cross a level, and never said which way it was crossed. With
  no underlying price yet, a level below the market was sent as "at or above" and
  armed on the first update. The text now gives the level and its direction, and
  without a price the ticket asks which way the level is crossed.
- **Orders says when its filters hide every row.** The side, status, origin and search
  filters apply to all three tabs, and a filter that hid every row left "No working
  orders." or "No fills yet." beside a tab reading "Working · 1" or "Fills · 1" and a
  Cancel all button. The page now says that no orders or fills match the filters, and
  says there are none only when there are none.
- **No demo offer while the feed is still connecting.** For the first half minute of
  `openportd --provider demo`, before any quote was valid, the header banner and the
  welcome dialog said "Nothing can be traded right now" and offered **Try the demo**,
  a replay of a simulated day, though the server was itself the demo market and
  tradable moments later. The demo is now offered only while every market is closed
  or its feed has stalled, and never on a simulated feed.
- **Deleting an analysis portfolio reports a failure.** When the delete was refused (a
  wrong or expired write token, the server out of reach), the Analysis page showed
  nothing, kept the portfolio and left Delete clickable while the request was out. The
  failure is now shown beside the portfolio, Delete waits for the request, and a
  portfolio another tab already deleted leaves the list.
- **Linux release builds run on their own architecture before they ship.** CI builds
  and tests only amd64 Linux and arm64 macOS, and the release workflow packaged and
  attached the arm64 Linux archive, `.deb` and `.rpm` without ever starting the
  binary. Each architecture's image now goes through CI's Docker smoke test (status,
  the path check, a paper fill and the API contract) on its own runner first, and
  nothing built from it is attached if that fails.
- **A release's `SHA256SUMS` no longer names an archive the workflow replaced.**
  `tools/release.sh --publish` attached its own Linux archive and a `SHA256SUMS`
  covering it; publishing the draft rebuilt that archive under the same name, with
  different bytes, so for the tens of minutes until the workflow rewrote the sums (or
  for good, had it failed) the release listed a wrong checksum. The script now
  attaches only this machine's archive and keeps its Linux build in `dist/linux/` to
  try locally. Its Docker image, and that Linux build, also reported build revision
  `unknown`; they now carry the commit's, as the native build does. Release builds
  compile source paths relative to the checkout, so the binaries no longer name the
  build machine's directories, and `tools/record_site_demo.mjs` no longer names them
  in its instructions either.
- **The Backtest page says why Start is disabled.** With writes disabled, a write token
  still to enter, or the account disabled, Start, Keep and Delete were disabled with an
  empty line where the reason belonged. The reason is now shown beside Start.
- **The halt drill's Minutes field can be retyped.** Clearing the field wrote 1 back
  into it, so typing 30 after a Backspace gave 130, and **Halt underlying** sent that.
  The field now keeps what is typed, and the button waits for whole minutes from 1 to
  390.
- **The header says which underlying is shown.** The underlying buttons marked the
  current symbol only by colour, so a screen reader heard a row of identical buttons.
  The current one now carries `aria-current`.
- **A second new playbook saves.** New playbook always proposed the ID `put-spread`, and
  the server never reuses an ID, archived playbooks' included, so a second New playbook
  › Save version failed with `INVALID_PLAYBOOK` until the ID was changed by hand in the
  JSON. The template now takes the first unused ID (`put-spread-2`, ...), and the
  editor says that IDs are permanent.
- **The Replay page shows a failed request once, where it belongs.** A failed start,
  resume, verify, restart or delete was printed twice, above and in the recordings,
  and opening Restart or Delete afterwards showed that earlier failure above the
  confirm button, as if the restart or delete had failed already; a cancelled failed
  delete stayed on the page. It is now shown once, a dialog opens without an old
  failure, and closing the dialog clears its own.
- **A scenario pack that fits clears the size error.** After choosing a pack over
  64 KiB, choosing a valid one filled the text box but kept "Scenario packs are at most
  64 KiB" under it, and the same file could not be chosen again after editing the
  text. A file that fits now clears the error, and any file can be chosen again.
- **Risk and equity panels stay up while they refresh.** The Dashboard's intraday
  equity chart, the scenario explorer and the historical VaR and stress panel are
  fetched again whenever the account changes (every fill, order or cancel, and every
  few seconds while positions are held), and each blanked until the new answer came:
  the equity chart flashed "Loading equity history…" and lost its day choice, and the
  explorer's grid also vanished at each keystroke. They now keep the last result on
  screen while the next one loads, and say when they are loading for the first time.
- **An expiry auto-close under half a minute is described as one.** The Rules page
  rounded the cutoff to whole minutes, so an account that closes expiring positions 1
  to 29 seconds before their last trade read "Positions are held into expiry", the
  plan table and the reset dialog said "0 min before", and 90 seconds read "2 minutes".
  Cutoffs are now stated to the second ("20 seconds", "1 min 30 s").
- **Beta overrides that do not parse are no longer dropped silently.** In the
  Positions risk profile, beta-override text that did not parse (or a beta beyond
  ±10) was sent as no overrides at all, so the beta-weighted profile on screen
  ignored the text still in the field, marked only by a red border. Such text is now
  not sent: the profile keeps the last overrides that parsed, and the field says
  what it takes, as the scenario explorer does.
- **One word, one meaning, and one brand.** "Journal" names the trade journal page, but
  the Dashboard and the Replay page's finished runs used it for the size of the
  account's journal file, whose record count has nothing to do with the trades there;
  that figure is now the "account file". The welcome dialog and the alerts dialog
  wrote the name as "openport"; they say "OpenPort" like the rest of the terminal. The
  Dashboard no longer reads "Practice · Practice" for the built-in Practice plan.
- **A strategy search its client cancelled says so.** A client that closes the
  connection cancels its optimizer search, and one that closes only its sending side
  after the request (`shutdown(SHUT_WR)`, `nc -N`, some proxies) cannot be told from
  one that has gone. Such a client still read the answer: 200 with `evaluated` 0, an
  empty candidate list and `truncated` true, as if nothing fit the budget. A search its
  client cancelled now answers 503 `REQUEST_CANCELLED`.
- **Early assignment notifications count contracts.** The webhook, Discord, Telegram
  and ntfy text of an early assignment printed the closed position's sign, `closed -5
  contracts`. It now reads `assigned on 5 short contracts` (an exercise, `exercised 5
  long contracts`), and one contract is singular.
- **A bare `--paper-journal` file name.** With `--paper-journal journal.jsonl` the
  journal's directory was empty, so the market-halts file beside it failed its
  directory sync after every write: `circuit_breaker.error` stayed set in
  `GET /api/status`, and the file was rewritten and synced on every batch. The
  shared writer no longer reports that; openportd now also makes the journal path
  absolute at startup, so every file kept beside it (halts, playbooks, sandboxes,
  replays, backtests) names its real directory.
- **`HEAD` and other methods on the API.** `HEAD /api/...` went to the routes
  without any credential, origin or scope check, as did any method openportd does
  not use, and an API answer to `HEAD` carried its body, which a client on a
  kept-alive connection read as the start of the next response. `HEAD` is now
  checked and routed as `GET` and answered without a body; any other method is
  refused with 405 `METHOD_NOT_ALLOWED` before a route sees it.
- **Every WebSocket upgrade is answered.** An upgrade to `/ws?x=1`, `/ws/` or any
  path but `/ws` and `/ws/events` closed the connection without a word. It now gets
  404 for an unknown path and 400 for a query on `/ws`.
- **An event stream says whether its account exists.** `/ws/events` takes an
  account before it exists (or before its replay starts) and follows it from then on,
  so a mistyped account looked like a quiet one. The first message now carries
  `exists`, and the Python client warns when a live account does not exist.
- **Slow links get the whole terminal.** A response had to be delivered within 60
  seconds in full, so a client slower than about 150 kbit/s (a phone, an SSH
  tunnel) got the 1.1 MB script cut short and a blank terminal. Each piece the client
  takes now has its own 60 seconds. The terminal's text files also go gzipped to a
  browser that takes it, under a third of the bytes, and the public demo's proxy
  compresses API answers.
- **A busy port fails at once, and a bad `--address` says so.** Both were found only
  when the web server started, after the journals, the crash log, the provider and the
  engine, and were reported as a bare Boost error (`Invalid argument [system:22]` for
  `--address localhost`). openportd now checks the port before it starts anything,
  and reports one in use as `--port 8080 is in use on 127.0.0.1: Address already in
  use (is another openportd running? choose another --port)`; an `--address` that is
  not an IP address is refused naming the flag.
- **Notification filters keep their confirmation.** Each channel's settings were
  rebuilt whenever the server's copy changed, so "Filters saved for this server
  session." vanished as soon as the status refreshed, and an unsaved edit was thrown
  away when another tab or client changed the same channel. A channel now keeps its
  message and any edit in progress, and follows the server's changes while it has
  none.
- **A build without the providers links again.** With `-DOPENPORT_BUILD_PROVIDERS=OFF`
  the default build failed at the test link: two funded-account tests call the
  server's per-fill equity samples, and the server is not built without the
  providers. They now build only where the server does, a CI job builds that
  configuration on every C++ change, and the test executable names each static
  library once, so Apple's linker no longer warns about duplicates on every link.
- **CI compiles the benchmarks.** A default build includes them and CONTRIBUTING runs
  them, but every CI job, the image and the release builds turned them off, so a
  change to an interface they use (the trading session, the desk or the margin
  internals) passed CI and broke `cmake --build build` for contributors. The Ubuntu
  Release build now compiles them, without running them.
- **Databento's own downloads are checked.** databento-cpp fetches JSON for Modern
  C++, cpp-httplib (which carries the Databento key over TLS) and date by URL with no
  checksum, and its JSON copy is the one every OpenPort target compiles against, so a
  moved tag or a tampered archive would have been built in without any check failing.
  The same releases are now declared first, pinned by SHA-256 as OpenPort's own
  dependencies are, and a test fails for any fetched library without a checksum.
  unordered_dense, fetched and configured on every build but never used, is gone, as
  is its entry in the third-party notices.
- **The remote-access Caddy example compresses the API.** Its site block had no
  `encode`, so every API answer reached a phone or a remote browser uncompressed (an
  SPX chain, about 85 KB, is about 15 KB gzipped), on every refresh. It now has
  `encode zstd gzip`, as the demo deployment's Caddyfile does.
- **The Trade page waits for the feed's first snapshot.** For about half a minute after
  `openportd` starts (or across a restart), an underlying's summary answers 404 until
  its first snapshot, and the chain showed "ApiError: no data for SPX yet". It now
  says it is waiting for the first snapshot and gives the feed's state; and the
  terminal's other panels show an error's message without the "ApiError:" prefix.
- **The guardrails editor says what it takes.** **Save guardrails** turned off with no
  message when a field was out of range (size creep must exceed 100, for one), and
  most ranges were not in the labels. Each field now gives its range and, when a value
  will not do, says why under it; the dialog also offers the write token, as the risk
  limits editor does.
- **The strategy leg bar beside the sidebar.** Between 1024 and 1279 pixels wide, the
  bar that collects a strategy's legs on the chain spanned the whole window, and the
  sidebar covered its left part, which holds the strategy's name and leg count, so
  only Clear and Review showed. The bar now starts beside the sidebar.
- **The replay banner no longer talks over everything.** The banner at the top of
  every page while trading a replay was one polite live region, and its clock changes
  every second or two, so a screen reader was asked to read the whole banner, file
  name, seed and buttons included, again and again. The banner is now a named region,
  and only a change in what the replay is doing (playing, paused, stepping, finished,
  a skip queued) is announced.
- **Analyze draws an edited candidate only with its own P&L.** For a quarter of a
  second after a change of units, a strike or the IV shift, the chart and price slices
  showed the previous strategy's P&L under the edited candidate (the 1-unit P&L after
  changing to 2 units). The curves now wait for the edited candidate's own profile.
- **A saved Analyze candidate whose expiry has gone says so.** A candidate kept in the
  browser past its expiry (yesterday's 0DTE, or a replay of another date) asked for its
  dead chain on every data update, showed an `UNKNOWN_CONTRACT` error, and its expiry
  menu showed the front expiry as if the leg were on it. It is now marked expired, asks
  for nothing, shows its own expiry as not listed, and can be moved to the nearest
  listed expiry or removed.
- **Spread alerts start only from held legs that make one.** **Start from held legs**
  listed every underlying with a position, and picking one held as a single leg, more
  than four legs, or legs in a ratio above 10 silently did nothing. The list now holds
  only the underlyings whose legs make a spread alert, and says so when none do.
- **Write tokens beyond ASCII work in the terminal.** The terminal put the token into
  the Authorization header as it was typed. Browsers send header values as Latin-1, so
  a token such as `tökén-secret` reached openportd as different bytes and every write
  was refused, and a token with a character above U+00FF (Cyrillic, say) made every
  request fail before it was sent, so the terminal never loaded. The token now goes as
  its UTF-8 bytes, which is what openportd compares, and a `#token=` link holding a
  control character is ignored.
- **The navigation drawer on narrow screens.** Below 1024 px the closed sidebar was
  only moved off screen: its 15 links and controls stayed first in the tab order and
  were read out by screen readers, and opening it left focus on the menu button
  behind the backdrop. A closed drawer is now inert, opening it moves focus to the
  current page's link and closing returns it to the menu button, which says whether
  the drawer is open.
- **The sidebar's System pages stay reachable.** On a 768 px tall window the account
  block (larger once an evaluation is active) left the page list too little room, and
  Replay, Backtest and Status sat in a scroll area that nothing showed was there. The
  whole sidebar now scrolls as one, the page list first.
- **A sandbox that ends hands over to the main account cleanly.** When a public-demo
  sandbox expired, or the server stopped offering sandboxes, the terminal moved to the
  main account without starting a new account scope, so for a moment the main account
  could show the sandbox's positions, orders and fills, a late sandbox response could be
  shown as current, and the next alert asked to confirm a changed destination. Every
  change of account now starts a new scope, however it came about.
- **The 1-9 page keys work after clicking a button.** Chromium and Firefox keep focus
  on a button clicked with the mouse, and the page shortcuts ignored any key pressed on
  a button, so after clicking a sidebar page, a tab or any other button the advertised
  "1-9 Switch sidebar page" did nothing until focus moved. Digits now switch pages from
  a focused button; the left and right arrows still stay with it.
- **Calendar and diagonal templates say what their far leg lacks.** On the last listed
  expiry, or with **Choose a later expiry** picked, the strategy templates showed
  "Selecting listed contracts…" forever; a saved preset whose far expiry had passed
  asked to "Load the far chain", which nothing would load. The dialog now says that no
  later expiry is listed or that one must be chosen, and a recalled preset whose far
  expiry has passed starts from the first later one.
- **Loading the terminal sends its first requests once.** The WebSocket opening a few
  milliseconds after the page's first requests cancelled them and sent them again,
  status and the replay listing included, on every load and reload. The socket's first
  open now leaves them alone; a reconnect, or an open after a failed attempt, still
  refreshes everything, since the server may have restarted.
- **One replay driver number.** The current driver was a literal in four places (the
  start input's nested ternary, verification's accepted range, resume's check and
  archives' opening-settlement test), each edited by hand for every new driver; one
  missed would have made every new run refuse to resume or fail `--verify-run`. It is
  now `kReplayDriver`, and one table maps each driver to its option for recording,
  verification, resume and archives.
- **One damaged replay sidecar no longer hides the Replay page.** A run's `<id>.json`
  that was valid JSON but had a `drill` that was not an object with a score, or a
  scored drill under a `scenario` that was not text, made `GET /api/replay` answer 400
  and `GET /api/replay/history` 500, hiding every saved run, the demo catalogue and
  the running replay. Such a sidecar now lists its own run with an `error`.
- **Verifying a run without its sidecar no longer makes it resumable.** The
  verification result was saved into a new sidecar holding nothing else, and the
  listing then showed the stopped run as interrupted: a resume ran it again and
  appended to its archived journal, invalidating the verification just passed. A run
  resumes only from a sidecar holding its start state.
- **Replay history lists the newest run first.** It was sorted by run id in reverse,
  which starts with the scenario or recording name, so runs were grouped by reverse
  name and a run just finished could land anywhere. The run whose journal was
  written last now leads.
- **Scenario pack listings answer at once.** `GET /api/replay/packs` and
  `/packs/{id}` waited behind a replay start or an `until` step on the control thread;
  they are now read on the calling thread, as the other listings are, and
  `GET /api/replay` reads the scenarios under the lock an import changes them with.
- **Scenario and replay dates from 2022 to 2200.** A scenario file, a pack or a
  `POST /api/replay` date override took any trading date: up to 2262 the demo ran a
  session with no options, as its expiries do not fit the clock, and before 2022,
  where the exchange calendar ends, holidays such as 1970-01-01 played as trading
  days. Dates outside 2022-01-01 to 2200-12-31 are refused.
- **A filled roll or close ends with Done.** After a strategy roll or close was
  accepted, its ticket offered **Another order**, which left a dead dialog: quantity 0,
  every control disabled, still titled with the position that no longer existed. Those
  tickets now offer **Done** once the order is accepted, and **Another order** only
  after a refusal or failure, when the position is still there to trade.
- **`--compact-journals` syncs each rewrite once.** The temporary rewrite was synced
  to disk after every record it wrote, one full drive flush per transaction (minutes
  for tens of thousands of records), though it is read back and must recover the same
  account before it replaces the journal. It is now synced once, when whole.
- **A server that requires a token says so.** With `--require-token`, a terminal with
  no token, or the wrong one, sat on "Waiting for the first snapshot… Connecting to
  openportd…" and polled and reconnected every few seconds, so a refused token looked
  like a server that was down. The terminal now says that openportd answers only with
  its token, or that it refused the saved one, and asks again only once the token
  changes.
- **Watchlist cards no longer blank on each update.** Each new data version of a
  symbol blanked its card's change, ATM IV, IV rank and expected move until three
  fresh requests returned, and fetched the daily candles again although the prior
  day's close does not move during the day. Cards now keep their figures while the
  next ones load, and ask for the prior close once per trading day (each minute until
  that bar has arrived).
- **Jump to symbol follows the keyboard.** In **Jump to symbol**, the arrow keys
  moved the highlight past the visible rows without scrolling to it, so Enter opened a
  symbol out of view, and when the list shrank while open the highlight could fall
  off the end, leaving Enter doing nothing. The highlighted row is now scrolled into
  view and always stays on a listed symbol.
- **Imported earnings moves have no session dates, not 1970-01-01.** `before` and
  `after` in `GET /api/earnings` (and `earnings.json`) read `1970-01-01` for every
  imported move, where the API documents null. They are now null until known, and a
  file an earlier version wrote reads the same.
- **A print at a locked quote is no longer a buy.** With bid equal to ask, a print at
  that price was classified at the ask, so the flow tape counted it as bought premium
  and toward sweeps. It is at both sides at once and now counts as `mid`, toward
  neither.
- **`surface-history` with only `to`.** The default `from` was 45 days before the
  analytics time whatever `to` said, so a `to` more than 45 days back was refused as
  an unordered range. The default 45 days now end at the given `to`, as on the series
  route.
- **A failed sandbox creation spends no creation budget.** A creation refused with 503
  `SANDBOX_UNAVAILABLE` (a full command inbox, whose `Retry-After: 1` invites a retry,
  or a journal that cannot be written) still counted as one of the visitor's 3 and the
  demo's 30 creations an hour: three such failures locked a visitor out for an hour,
  and thirty every visitor, though no sandbox was made. It no longer counts.
- **openportd's errors stay in view and name what is wrong.** A usage error printed
  its one-line reason and then the whole 140-line help, so the reason scrolled away;
  it now prints the reason and `Run openportd --help for usage.`. A missing
  `DATABENTO_API_KEY` or `MASSIVE_API_KEY` for `--import-day` was reported as a usage
  error after progress lines; it is now said first, alone, with exit status 1.
  `--verify-run` with more than one argument says it takes exactly one journal. The
  `--backtest` command reported raw JSON library errors for a non-numeric
  `--scenarios` or `--workers` and a malformed or wrong playbooks file, and an unknown
  `--plan` as `Cannot read nope`; each now names the flag or file and what it must be.
- **openportd says what it is running.** An `--allowed-origin` the web server would
  refuse was found only after the crash log, the journal (seeded with the given plan)
  and the feed had started, so the corrected command ran the first one's plan; it is
  now refused while the flags are read. Plan, cash, fee and fill flags given for a
  journal that already holds another account were ignored silently; startup now names
  each one it ignored. The banner's link uses brackets for an IPv6 address and
  `localhost` for a wildcard bind, startup names the paper journal, says on stderr
  when writes are off because a non-loopback bind has no token, and warns when the
  web root has no built terminal instead of announcing it as served.
- **`openport-probe --help` is an answer.** It printed the usage to stderr with exit
  status 2; it now goes to stdout with status 0, a missing provider or symbol is
  named, and `--certify` ends by printing the outcome and the report directory's full
  path.
- **A fractional share no longer refuses a whole broker export.** One row such as
  `SPY,10.532` made a portfolio import fail with `quantity must be a whole number`,
  losing every other holding, though dividend reinvestment and fractional trading put
  such rows in most exports. The whole shares are now imported and the fraction is
  listed as skipped; an option quantity must still be whole.
- **Backfilling an older month of the minute series no longer slows down as it goes.**
  For a minute more than 45 days before the newest one kept, `--backfill-series` read
  and parsed that month's whole CSV twice per minute it wrote, so ten sessions into a
  series directory that had been recording live took minutes instead of
  milliseconds. Such a month is now parsed once and kept while its file is unchanged.
- **Backtest retention has a byte budget.** A run keeps every day's and attempt's
  journal, 2 to 7 MB per simulated day, and retention only counted runs, so the
  default 20 runs of 252 days could take tens of GB, more with sweeps. Before a start,
  the oldest finished unpinned runs now also go until the rest take at most
  `--backtest-keep-gb` (decimal GB, default 20, 0 unlimited); the newest always stays.
- **A long-lived account no longer copies its whole equity history on every
  publication.** Every market batch and command copied each account's full equity
  curve (one mark a minute plus every fill, never pruned for the current attempt:
  about 22 MB a year) into its new view, and each fill's sample scanned the whole
  history for a duplicate. The history is now shared copy-on-write between the store
  and its views, and a fill's sample is checked against the highest fill kept.
- **Oversized portfolio imports name their 4 MiB limit.** A portfolio import over
  4 MiB sent with `Content-Length`, as browsers and curl send it, was refused with
  "Body exceeds 64 KiB", the limit of every other route; only a chunked one was told
  4 MiB. Both now are, and the Analysis view says imports take up to 4 MiB, not 64 KiB.
- **A damaged saved rule pack.** A rule pack kept in this browser whose plan had
  `null` rules (a hand-edited or damaged entry) was still offered, and listing its
  plans to start from then failed. Such a pack is now dropped when read, like other
  malformed entries.
- **Archiving a playbook asks first.** Archive on the Playbooks page archived the
  playbook at once, though archiving cannot be undone: its ID cannot be used again,
  and every live account running it switches to Off, which ends its forward tests.
  It now asks for confirmation and says so, as enabling Auto does.
- **The API contract test checks every documented read.** `tools/contract_test.py`
  skipped a route whose ID it could not fill, so on a server without playbooks, as in
  CI, the playbook, signal-secret and replay playbook reads went unchecked. A missing
  ID now checks their documented answers, and a route the test cannot fill fails it.
- **CI checks the public demo's proxy.** `tools/demo_proxy_test.py`, the only check
  that the demo's Caddyfile refuses anonymous and spoofed requests and fails closed
  when its auth gateway stalls, ran only by hand. CI's Docker job now runs it, and a
  change under `deploy/` runs that job.
- **Massive pages that lead back.** A chain poll refused a `next_url` naming the page
  just fetched but not an earlier one, so two pages naming each other were fetched in
  turn until the 2,000-page limit: 2,001 requests a poll, every poll. A next page the
  poll already fetched now ends it with an error at once, and the last chain stays.

## 0.5.1 (2026-10-28)

The project's new home on GitHub, a Homebrew service that starts, and patch updates
to the web terminal's dependencies.

### Upgrading

- **New project home.** The GitHub account moved from `38st` to `armanruzgar`. The
  image is now `ghcr.io/armanruzgar/openport`, which holds every earlier release tag;
  `ghcr.io/38st/openport` no longer pulls. The repository's Compose files default to
  the new path, but an older checkout, a `.env` or an `OPENPORT_IMAGE` naming the old
  path needs the new one. A Homebrew tap added as `38st/openport` keeps working
  through GitHub's redirect, which holds only while nobody else owns that name;
  `brew tap --custom-remote 38st/openport https://github.com/armanruzgar/openport`
  points it at the new home. The update check, issue links and the server's
  User-Agent use the new address. The update check in 0.5.0 still asks the old
  address and refuses the redirect, so it never reports this release or a later one:
  upgrade 0.5.0 by hand.

### Fixes

- Homebrew: `brew services start openport` failed on current Homebrew, whose service
  block has no `opt_share`; the service now passes `opt_pkgshare/"web"` as its web
  root. The fix reached 0.5.0 installs through the tap.

## 0.5.0 (2026-10-26)

New market-data providers and feed failover, working share orders, splits, queue
fills and real-fill calibration, prop-firm rule types, an Analyze tab with a strategy
optimizer and hedge suggestions, historical stress and VaR, an option tape, outside
portfolio analysis, Linux packages and a backup and upgrade guide. Providers marked
"not yet run live" were tested against documentation-shaped samples or recorded
responses only; reports from live use are welcome.

### Upgrading

Back up the data root first ([upgrade and backup](docs/upgrade-and-backup.md)). An
older `openportd` cannot be assumed to read journals written by this release, and
these formats and defaults changed:

- **Recordings.** Providers that deliver a trade's venue (Databento, and ThetaData or
  Databento imports) now write recording format version 5, which 0.4.0 refuses at
  the header. Every other provider writes the same files as before. Daily
  `--record-dir` files and a feed behind `--fallback-provider` carry the venue
  header as well.
- **Fills CSV.** Every earlier column stays in place; the export ends with three new
  columns, `feed`, `stress_ticks` and `queued`. Fills in the API report the same
  fields, and share fills name their order as `stock_order`.
- **Named token files.** Each secret must be at least eight printable characters; a
  shorter secret makes the file invalid, as a malformed line does. A `#` starts a
  comment only at the start of a field, so a `#` inside a secret is part of it.
  Token files reload live on change, `SIGHUP` or `POST /api/tokens/reload`.
- **Journal maintenance.** `--compact-journals` and `--repair-journals` are separate
  runs: `--compact-journals` refuses `--dry-run` and `--file`, which belong to repair
  (a dry run used to compact for real). An empty write-token file no longer blocks
  startup; it is replaced.
- **Program rules.** Financing rates, pin risk, queue fills, stop stress and every new
  prop-firm rule type are program rules: setting one drops a preset's `plan_id`, and
  financing overrides on evaluation, verification and funded plans are refused with
  `PLAN_LOCKED`. All of them are off by default and in no built-in preset, so existing
  plans keep their journal bytes. Built-in presets are marked `illustrative` in
  `GET /api/plans`.
- **API responses.** The default exposure convention is named `long_calls_short_puts`
  (`open_interest` is still accepted as a deprecated input); without a finite positive
  spot, exposure totals and cells are null with `exposure.available=false` and a
  reason instead of 0. Series and surface-history errors use the standard error body.
  Account WebSocket events are versioned with bounded delivery. Outbound notification
  channels keep their previous default events; `order_state` is opt-in.
- **Docker and systemd.** The image starts through an entrypoint script that adds the
  server settings only to a server start; `--backtest`, `--import-day` and
  `--verify-run` run in the image without them. `docker-compose.yml` reads
  `OPENPORT_IMAGE` and `OPENPORT_VOLUME`. The new Linux packages' systemd unit starts
  with `--no-token-link`, so the token stays out of journald; open the terminal with
  `openportd --print-url --open`.
- **Python.** The client publishes to PyPI as `openport-client` (import name
  `openport`; the PyPI name `openport` is an unrelated project), from `python-v*` tags
  on `main` only.
- **Build.** The Interactive Brokers adapter is behind `OPENPORT_WITH_IBKR` (on by
  default). The build embeds the stress shocks and built-in scenarios and objectives
  from `data/`.

### Providers and historical import

- **Charles Schwab market data.** `--provider schwab` polls Trader API option
  chains with the underlying quote, open interest, volume, delta and gamma, using
  an OAuth refresh token from the environment. Health follows each response's
  `isDelayed`; `isMini`/`isNonStandard` and `mini`/`nonStandard` both mark a
  nonstandard contract. Tested against documentation-shaped samples; not yet run live.
- **Alpaca market data.** `--provider alpaca` polls option snapshots for equity
  and ETF underlyings from the free indicative feed (reported as delayed, derived
  quotes) or OPRA with a subscription. Alpaca has no index options, so SPX and
  other indexes are refused. Not yet run live.
- **Interactive Brokers market data.** `--provider ibkr` reads Client Portal Web
  API snapshots through your logged-in local gateway: it keeps the session alive,
  maps contract ids to OSI symbols, paces requests and counts market-data lines.
  Requires `--expiries` and `--window`. Not yet run live.
- **Feed failover.** `--fallback-provider NAME` switches an underlying to a second
  provider once the main one has been in error or silent for
  `--fallback-seconds` (default 30), and back once it has been fresh for as long.
  A primary labelled stale whose snapshots advance keeps its underlyings, and none
  move onto a backup that could not start or reports an error. Contracts map by OSI
  symbol; the backup reports delayed with a fallback label in status and the
  terminal. Paper trading keeps its freshness checks, and every fill names the feed
  that priced it (`feed` in the fills API and journal).
- **Published AM settlement values.** `--am-settlement cboe` fetches the special
  opening quotations Cboe publishes for SPX, RUT, DJX and VIX (indices SET, RLS, DJS
  and VRO, from Cboe's public daily history) and settles live AM positions only on
  the history row for their exact expiry date, with source `published_soq`. Waiting
  positions show `settle_by: "soq"` while the expiry date is within the fetcher's
  14-day lookback, instead of turning `manual` at 17:00 ET on the expiry date; manual
  import remains the fallback. The index's delayed quote is not used, because its
  trade time cannot tell reliably when it shows the day's value. Options trading and
  rollover wait while a position awaits settlement; Cboe's posting lag is unmeasured.
  Tested with responses recorded on 2026-10-21.
- **Import historical options from Massive (Polygon).** Historical contract discovery,
  paginated quotes and trades preserve vendor timestamps. Quote and trade pages grow
  up to Massive's 50,000-row maximum within the buffer cap. Tested with generated
  saved-response fixtures; paid historical access has not been validated live.
- **Import date ranges and resume.** All importers accept inclusive `--import-days`
  ranges and repeated dates, skip calendar closures, report progress and skip only
  matching complete recordings.
- **Provider certification.** `openport-probe --certify` writes redacted JSON/Markdown
  reports for timed observations or a snapshot cycle, including event/contract
  coverage, clock checks, Greek scale hints and errors, for the new provider report
  issue template. Demo/replay reports are labelled as such and do not claim live
  validation.
- **Greek and volume mappings.** Documented ORATS/DXLink Greeks and ThetaData vega/rho
  scales are mapped; ThetaData theta remains unknown. Tradier chain and DXLink session
  volume are mapped without inventing trades or timestamps, and Tradier volume from an
  earlier trading date is withheld. Sample/mock tested, not yet validated live.
- Option prints carry the printing venue where the provider gives one. Unknown
  ThetaData snapshot times are preserved; unused DXLink Profile subscriptions are
  gone, supplied Summary event times are kept for open interest, and tastytrade
  subscribes only to the events it reads, raising the contracts its cap allows.
  A ThetaData `Retry-After` outside 1-60 s, or a date, backs off instead of failing
  the import. `/api/status` reports `provider.extended_hours_prints`, false for Cboe
  and the demo market.
- `--provider replay` without `--symbols` plays the recording's symbols.

### Recording and replay

- **Record across trading dates.** `--record-dir` rolls at calendar boundaries and
  publishes only complete files, with optional oldest-first age/size retention that
  protects active files. Replay lists trading dates, symbols and sources across
  providers. Retention runs off the feed thread at rollover, reports files it cannot
  remove instead of stopping recording, and counts stale staging folders toward the
  size cap.
- Replay pacing survives several pauses and resumes before the pacing loop observes
  them. A replay stall no longer moves orders to another account, and a new replay
  run drops the last run's market data in the terminal.
- Backtests and replays: a damaged run or sidecar no longer blocks the rest, failed
  resumes and cancelled checks lose nothing, listings resume, and rule codes and
  archived modes stay straight. Replay CSV exports name the run as the account.

### Trading, financing and halts

- **Working share orders.** Stock and ETF shares take resting market, limit, stop and
  stop-limit orders (`POST /api/stocks/orders`, `/preview`, `GET`,
  `DELETE /api/stocks/orders/{id}`), DAY or GTC, with extended-hours limits
  (premarket and after hours), brackets and OCO. Opening orders reserve buying power
  and see working sells in buy-only and cash-account checks; each fill takes the
  share checks, and a bracket stop satisfies `require_stop_loss`. Market share orders
  reserve buying power at the last price from acceptance and need one; resting share
  fills take `no_counter_positions` as option fills do. The share ticket places them
  and lists working ones; the Python client adds `place_stock_order`,
  `preview_stock_order`, `stock_orders` and `cancel_stock_order`. Buy-writes at one
  net price are not offered.
- Extended-hours share limits fill only on prints fresh within premarket or after
  hours, never on the regular close, which outside the regular session keeps its own
  print time even from a current snapshot. The share ticket disables Extended hours
  with a caption while the feed reports no extended-hours prints.
- **Splits and adjusted deliverables.** `--corporate-actions FILE`
  (`SYMBOL,YYYY-MM-DD,split,N:D`) splits held shares on their ex-date at the
  rollover, paying cash in lieu of a fraction, and adjusts the deliverable of every
  share-settled series on the underlying; settlement, exercise, assignment, strategy
  margin (lots of the adjusted deliverable), cash-account coverage and risk repricing
  follow it, and the desk quotes adjusted series from the standard series that pays
  the same. Listed adjusted series trade when their deliverable is supplied instead
  of `NONSTANDARD_UNSUPPORTED`. Positions report `deliverable`, accounts their
  `corporate_actions`; working share orders on a split stock end with
  `CORPORATE_ACTION`. Recorded runs keep the splits as an input.
- Adjusted series no listed series pays the same as, or whose listed equivalent never
  quotes, are valued by model from the strikes around them, floored at their
  deliverable's exercise value in the session's own decimal arithmetic, reported as
  `modelled_mark`, never fill, and no longer stall the rollover. A contract left with
  less than one share delivers cash only and is worth that fixed payoff:
  undiscounted for an American series, discounted for a European one, the discounted
  value being what historical stress margins a short one at.
- **Financing, pin risk and halts.** Practice cash transfers, rollover credit/debit
  interest and stock-borrow fees are journaled and visible in Dashboard; financing
  counts in equity and daily loss without adjusting its opening base. Opt-in seeded
  expiry pin risk models contrary exercise near the strike and warns before expiry.
  Single-name scenario/replay halts pause one underlying, with recorded, verifiable
  halt-drill commands in the replay API and Dashboard. Shares halted through the
  close roll over on their last pre-halt print, reported as `halted`; prints inside
  a simulated halt are not taken as marks.
- What-if accepts OTO/OCO chains, evaluates their current-quote fills and contingent
  orders on a private account copy, and applies submission's opening gates (volume
  caps and other opening rules) to parents and every child, as submit does.
- Exercise of a defined-risk position never leaves short shares; starting cash must
  be positive; early-close days have their own hold cutoffs; ETF scenario prices
  settle at the 16:00 close; and expiry warns and alerts when share delivery would
  overdraw the account.
- Exercise opens shares through the plan's share-entry rules (entry windows,
  `require_stop_loss`, hedging, `max_trade_risk`, exposure) as a share trade does;
  shares that only reduce a holding are unaffected. A session refuses a size-scaling
  maximum below its starting balance at construction, as a reset does.
- **Copy and allocation trading.** Named account groups
  (`PUT/DELETE /api/account-groups/{name}`, `GET /api/account-groups`, admin to
  change) and `POST /api/orders/copy` (and `/preview`) mirror one order to every
  member as its own submission, sized by fixed ratio or split by marked equity, each
  with its own checks and journal; the reply lists each account's status and
  response. An equity-allocated copy skips an unmarked member with `STALE_QUOTE`
  instead of failing. The Python client adds `account_groups`, `put_account_group`,
  `delete_account_group`, `copy_order` and `preview_copy_order`; Manage accounts
  edits groups and the option ticket offers **Copy to group** with several live
  accounts.

### Evaluation and prop rules

All new rule types are off by default and in no built-in preset.

- Open-loss cap on one trade's marked loss, net across its legs (fixed, or a share of
  the start-of-day profit balance or drawdown room), that closes the whole trade (a
  spread as one order, shorts first, never leaving a naked leg, with adds refused
  while the close is pending), blocks adds or fails the attempt (`OPEN_LOSS_CAP`),
  journaled as `open_loss_cap` events.
- Balance drawdown basis: the floor follows and checks the closed balance, so it can
  be set per phase; a static floor ignores the hidden lock balance. Best-position cap
  on the profit one trade counts toward the target (`BEST_POSITION` objective);
  opening-order cap on the last minute's option volume, refused with `MINUTE_VOLUME`
  when that volume is unknown.
- Payout reviews of short-held, oversized or open-loss trades (share round trips
  included; a trade closed at a payout's own timestamp is reviewed by that payout
  only) escalate warning, forfeit and cap, in the payout preview and the journaled
  payout. The floor after a payout can stay, reset the account to a share of its
  starting balance, or move up to it; payout caps and margin follow the plan.
- `POST /api/account/rules-compare` replays the attempt's recorded days against
  presets or custom rules side by side, adding back everything a payout took out
  (forfeits and reset removals too); payout reviews and the floor after a payout are
  listed as not replayed. The Rules page shows where each passes, fails or locks.
- Versioned rule packs (`effective_from`, compared with today's New York date,
  `source`, `verify`) validate through `POST /api/rule-packs/validate`, which refuses
  `unlocked_by` cycles; the terminal imports, exports and starts custom attempts from
  them, apart from the presets.
- `pass-odds?economics=true` adds challenge economics: expected value and break-even
  pass rate from each program step's odds, fees and resets and the first funded
  payout.
- Opt-in discipline guardrails: a losing-streak cooldown and size-creep warnings or
  blocks against the median of recent opening orders, journaled as `tilt_flag`
  events.
- The flat-time countdown knows weekends and market holidays. Plan imports keep edits
  made meanwhile.
- Python client and MCP: `rules_compare`, `validate_rule_pack` and economics on
  `pass_odds`.

### Fill models and fill quality

- **Queue fills** (`fill_model: "queue"`, `queue_fills`, `queue_seed`): a resting
  single-leg limit at the touch waits behind the size displayed when it joins; trade
  prints at its price use that queue up before it fills at its limit, prints through
  its price fill it, and cancellations come out of the queue in proportion. Without
  prints, volume increases while the touch stays at its price (half each side) or a
  documented five-minute turnover of the displayed size stand in; the turnover fills
  orders even when nothing traded at their price. Time priority follows when an order
  reached its price (after latency, or the change that sent it there), and only a
  smaller size keeps its place: a larger size or any other change goes to the back.
  Fractions round at random from the seed, so the same feed always gives the same
  fills. Orders show the estimate and wait with `QUEUE`. Recordings' trade prints
  reach only accounts using it; others keep their bytes.
- **Stop stress** (`stop_stress_ticks`, `stop_stress_percent`): stops reached while a
  contract's spread or quote rate is a chosen multiple of its normal slip extra ticks
  on their first fill attempt, never past a stop-limit's limit, reported on the order
  and its fills. A stop-limit that rests after it fills later unstressed. Off by
  default.
- Daemon flags `--paper-fill-model queue`, `--paper-queue-seed`,
  `--paper-stop-stress-ticks` and `--paper-stop-stress-percent` seed a new main
  journal. Latency, slippage, impact and inside fills stay outside the program rules.
- **Fill quality** (`GET /api/fills/quality`, Journal): fills placed between the mid
  and the natural by time of day, spread width and DTE, with a heuristic
  Conservative calibration.
- **Real-fill calibration** (`POST /api/fills/calibration`, Journal): import generic,
  tastytrade, thinkorswim/Schwab or IBKR fills, compare each with the same-time
  recorded quote and what the paper models would have done, and get suggested
  settings. Analysis only: nothing is stored or traded. It needs a read token (not
  anonymous where reads are public; never a sandbox), runs one at a time on a worker
  (503 `CALIBRATION_BUSY`) and searches at most 31 days of fills, 32 recordings,
  4 GiB and 20 seconds, newest first, stopping once every fill has a quote; `search`
  reports how far it got. Quantities above a million contracts are skipped. The
  broker layouts are tested on sample files, not live exports.
- Python `Client.fill_quality()` and `Client.calibrate_fills()`.

### Risk

- **Historical stress and VaR.** Cited historical stress scenarios and 1-day
  historical VaR/expected shortfall, with an observed ATM-IV-change variant when
  available. 99% VaR/ES needs 100 observations (95% still needs 20); below that
  `var99`/`es99` are null and `tail99_reason` says why. Read-only historical risk and
  scenario endpoints, with Python and MCP methods.
- **Scenario explorer.** Beta-aware spot × volatility × time scenario grids, with
  optional term scaling; existing rule grids retain their defaults. The spot ×
  volatility grid and the risk profile can reprice with sticky-moneyness or
  sticky-delta smiles along each underlying's fitted SSVI surface (`dynamics=`),
  beside the default sticky strike, in every explorer mode with a Smile selector;
  underlyings without a surface stay sticky strike and are named. Rules, margin and
  breach estimates are unchanged.
- **Caps and alerts.** Gamma, theta, reachable grid-loss and gross short-vega caps,
  with tag and playbook scopes and evaluation rollover discipline. Account alerts
  for utilisation, grid loss, gamma and modelled floor-touch odds through existing
  notification channels. A tagged order that reduces an untagged book over its scope
  cap is no longer refused: the scope is valued over the same positions with and
  without the order. Scenario grids for grid-loss caps and alerts are remembered by
  their exact inputs, so order checks stop repricing every open order's grid.
- Portfolio Greeks persist beside equity, so the terminal charts risk intraday and
  across days without reconstructing old values.

### Analyze

- **Analyze tab.** Trade's new tab draws up to four candidate strategies' P&L by
  price on today, a custom date and their first expiry, with or without the held
  book, under an IV-shift slider; a price-slices table (% moves, prices, ±σ,
  breakevens); strikes dragged on the chart or stepped per leg; and Send to ticket.
  `GET /api/risk/profile` takes hypothetical `legs` and `held=false` so candidates
  use the held book's own model; leg quantities are bounded and doubled signs
  refused.
- **Strategy optimizer.** `GET /api/underlyings/{symbol}/optimizer` searches
  verticals, iron condors, butterflies, calendars, singles and covered calls for a
  target price, date and loss budget, ranked by expected P&L under the market's or
  the user's shifted lognormal, probability of profit or return on risk. Candidate
  counts are capped (an even sample past the cap) and searches run on a two-worker
  pool that answers 503 `SEARCH_BUSY` when full, never on HTTP I/O threads; a search
  stops early when its client disconnects or the server stops, and queued ones are
  dropped at shutdown. A chain without an at-the-money term structure uses the
  expiry's ATM IV over the time (`variance_fallback`) rather than a zero-width
  distribution. Derived estimates, not advice; fees and early exercise are not
  modelled.
- **Hedge suggestions.** `GET /api/risk/hedges` gives the smallest share, ATM option
  or SPX/SPY beta hedge bringing a bucket's delta, vega or worst grid cell back
  inside its limit or above the floor, ranked by spread cost and residual risk. For
  a delta target the SPY/SPX hedge is sized on the beta-weighted dollar delta to SPY
  (the risk profile's betas, overridable with `betas=`) and reports the residual
  beta-weighted delta; a missing beta is named and the hedge falls back to one for
  one. Positions sends an option hedge to the what-if and then to its ticket.
- Python `optimize_strategies`, `hedges` and `risk_profile(legs=, held=)` with
  matching MCP tools.

### Volatility

- **Surface history.** Per-expiry SVI parameters persist at scheduled intraday slots
  and the close, one close per session on disk, loading 13 months and keeping 400
  days in memory; compare the same expiry against the prior close, week and month.
  Range lists return at most 400 snapshots with a `truncated` flag, and Python
  defaults to comparisons.
- **Event volatility.** An event variance premium is derived from adjacent straddling
  expiries using supplied labels and earnings/news calendars, including BMO/AMC;
  missing brackets and confounded solves remain explicit. Estimates count one event
  per date and session, and an event at a settlement instant solves after that
  expiry.
- **Vol scanner.** Batch API and sortable terminal view for every subscribed symbol,
  with history coverage, feed timestamps, simulation labels and symbol navigation;
  rows are cached per publication.
- **Volatility history.** Ranks, percentiles and population z-scores for signed skew
  and term ratios, plus a same-session VIX-minus-MFIV tracking line when observed.
  Live comparison measurements remain pending.
- Python and MCP read stored volatility and surface history and the batch scanner,
  and select the account calendar for event estimates.
- Deep in the money, time value at the price's rounding noise is reported as below
  intrinsic instead of solved to a wrong volatility.

### Exposure

- OI-weighted delta, daily charm and vomma exposure, per-strike and expiry totals,
  signed walls and sticky-strike spot flips alongside GEX/VEX.
- Positioning conventions and snapshot-date 0DTE/ex-0DTE slices through the API,
  terminal, Python client/frames and MCP. The default long-call/short-put convention
  is preserved and named `long_calls_short_puts`; customer flow is reported as
  unavailable without aggressor-side data. Formulas, units, coverage and search
  limits are documented.
- Charm's carry and yield rates use the floored exposure horizon, so a small
  forward/spot gap near expiry no longer dominates 0DTE CEX. Flip searches bisect
  only the brackets nearest spot and reprice one metric; a convention or slice
  request copies only the selected expiries.

### Pricing models

- **Closed-form American models.** `--american-model bs2002|baw` removes the early
  exercise premium with Bjerksund-Stensland (2002) or Barone-Adesi-Whaley instead of
  the 31-step Leisen-Reimer tree. The tree stays the default: measured against a
  converged tree it was more accurate, and no slower. Expiries with cash dividends
  always use the tree; each expiry reports its `eep_model`.
- **SABR beside SVI.** The Volatility view can overlay per-expiry SABR fits (Hagan
  lognormal, β fixed at 1 or ½, or fitted) with their parameters, RMSE and the same
  butterfly and calendar checks. The surface API fits them only when asked
  (`sabr_beta`, 0 to 1 in steps of 0.1, or `fit`; at most 60 expiries), so nothing
  changes for analytics or other readers.
- **Heston calibration (research, API only).** `GET /api/underlyings/{symbol}/heston`
  fits one Heston parameter set to the leading expiries and reports it beside each
  expiry's SVI and SSVI RMSE. Its parameters are weakly identified on real smiles;
  nothing else uses it. Heston and SABR fits run on a background worker, one per
  snapshot at a time, so they never stall other API reads; a request for a snapshot
  already fitting gets 503 with `Retry-After`.

### Flow, earnings and outside portfolios

- **Option tape.** A Flow tab on Trade shows time and sales from feeds that deliver
  option trades (Databento, and recordings or imports that hold them), each print
  classified at the bid, at the ask, mid or outside. Sweeps, blocks and best-effort
  multi-leg groups are inferred, and net premium is summed by expiry and strike.
  Feeds without trades say so plainly; only the demo market adds prints, labelled
  simulated and never grouped into spreads. `GET /api/underlyings/{symbol}/flow`.
- **Earnings moves.** For each earnings event in the event calendar, the front
  straddle's implied move before the news and the realized move after it build a
  history on the Volatility page, with how often the straddle priced more than came.
  Past quarters can be imported from a CSV and stay labelled imported.
  `GET /api/earnings`.
- **Outside portfolios.** Import a positions CSV (generic columns, or tastytrade,
  Schwab or IBKR exports) into an analysis-only portfolio and see its Greeks, risk
  profile, scenario grid, historical stress tests and one-day historical
  VaR/expected shortfall on your feed, without the floor or paper margin an account
  would have. It is not a paper account and can never be traded. Every route, reads
  included, needs admin; imports may be up to 4 MiB. `/api/analysis/portfolios`.
- The Python client and MCP server read the tape, the earnings history and analysis
  portfolios.

### Journal and practice

- Journal: entry-context edge reports, persisted mistake/setup picks and
  with/without cost comparisons; transparent trade coach flags and an opt-in,
  user-configured HTTP AI review hook. A journal fill-quality panel.
- Replay: start a fresh paused account from a reviewed entry with the original
  source/seed/date/plan, and mark authored scenario events on review/equity charts.
  Drill eligibility derives from the hash chain.
- Practice: grade declared objectives from journal states, retain scores and
  personal bests, and offer spaced fresh-seed practice with completion streaks.
- Scenarios: import/export packs and fixed challenges, including challenge inputs
  and recomputed drill results in replay verification receipts. Morning crush,
  Afternoon waterfall and Range bleed have objectives in a separate built-in
  objectives file, so their sources and earlier runs' hashes are unchanged. Two user
  scenario files with one id are reported.

### Terminal

- Keyboard help, symbol jump, chain contract navigation, buy/sell prefills,
  review/cancel shortcuts and focused-position close through the existing ticket.
  Enter on buttons and quotes acts on their own side; Escape keeps strategy legs and
  any mode change clears tickets.
- Chain settings remembered per symbol, named route/preference workspaces, a
  subscribed-symbol watchlist, and persistent local notification history with unread
  counts, mark-all-read and clear beside the existing toasts.
- Large chains are windowed with unchanged rows reused and collapsible stacked
  expiries; the chain steps and scrolls from the expiry shown, and chain legs quote
  outside the strike window.
- SMA, EMA and Bollinger studies plus saved per-symbol horizontal lines and
  trendlines, including price-alert creation; plan levels draw as dashed lines and
  slow swipes pan. VWAP remains unavailable on the current volume-free candle API;
  its math is tested with sample volumes.
- Chain, chart, positions and orders pop-outs, an optional wide-screen
  positions/orders split, and phone calls/puts selection with bottom-sheet tickets.
- Orders: edits follow the live order, corrected orders and exits go out under a new
  client order ID with take-profits on the entry's side, picks drop closed legs,
  rolls name the chosen strike, and trade risk reads in dollars. Backtests drop
  deleted runs and balance targets measure the balance.
- Alerts: the opt-in update check runs on plain HTTP, price alerts wait for a price
  and stay in step across tabs, a new attempt alerts again, and saved alert claims
  and brief levels stay current. A socket blip keeps the account scope; a slow
  browser socket keeps the newest frame of each tick stream, so live status no
  longer freezes behind a replay.
- A `#token=` link opened in a tab already showing the terminal is adopted there.

### API, Python and automation

- Versioned account WebSocket events with bounded delivery and typed Python
  iteration; the stream dispatches only to the addressed account's subscribers and
  sends `floor` only on entering or leaving the warning band.
- `openport.bot` strategy hooks for live paper, replay and saved history, with REST
  fallback.
- Inbound playbook signals: webhook senders use a per-playbook, per-account secret
  (issued, rotated and revoked by an admin, stored only as SHA-256) as an HMAC
  signature with a ±300 s timestamp window, or in the body over HTTPS for senders
  such as TradingView that cannot set headers; bearer clients use
  `POST /api/playbooks/{id}/evaluate`. Nonces and rate limits are per secret, and
  saved manual/automatic modes remain authoritative. Removing an account or expiring
  a sandbox ends its forward tests and playbook modes.
- Bounded backtest parameter sweeps with exact variant reports and historical
  ranking; `DELETE /api/backtests/sweeps/{id}?purge=true` removes a saved sweep, as
  for single runs. Optional sweep fields and comparison IDs are validated.
- Optional Arrow/Parquet exports preserving money text, with an offline example
  notebook.
- Python client coverage for every documented HTTP operation, with a contract parity
  guard; MCP `place_order` returns the client order id it sent, and `Fill` carries
  the optional feed. MCP tools for playbooks, staged orders, backtests, alerts,
  journals, what-if, market views and order changes retain account scopes and
  preview guidance.
- Server: open HTTP connections are capped at 512, a failed accept waits instead of
  spinning, browser sockets accept legacy tokens with spaces or UTF-8, a failed HTTP
  retry closes its connection, and CSV exports neutralise cells a spreadsheet would
  run as formulas.

### Install, access and support

- **Open the terminal without hunting for a token.** `openportd --open` launches
  the browser after startup; `--print-url [--open]` reads existing token settings
  and exits, and `--address localhost` names the host as given. The startup link
  percent-encodes its token, so tokens with `&`, `#` or `%` work. `--no-token-link`
  prints the `--print-url` command instead of the token link at startup. `--open`
  documents, and on Linux warns, that the launcher's arguments are visible in `ps`.
- **Install on more platforms.** Release automation packages Linux amd64/arm64
  `.deb` and `.rpm` assets with a private systemd service. Intel Macs build the
  release tag through Homebrew; Windows Docker Desktop/WSL2 paths are documented.
- **Keep data through upgrades.** New backup, restore, rollback and journal-repair
  guidance covers native, Homebrew, Docker and systemd storage. Remote/phone
  recipes use Tailscale or Caddy with cookie authentication.
- **Start and find help more easily.** The site adds Homebrew installation tabs,
  first-ten-minutes links and Discussions. Bug forms accept sanitized diagnostics;
  Discussions templates cover questions and ideas.
- **Prepare a hosted demo.** `deploy/demo` runs simulated prices and bounded
  sandboxes behind authenticated HTTPS, with container resource limits. The site
  keeps its browser-demo button hidden until a real URL is configured. Nothing is
  hosted or published by this change.
- **Share diagnostics deliberately.** Status copies version/build/platform,
  provider/feed and recent error categories into a reviewable bug report. Local
  fatal signal logs (`crash.log`) contain no memory dump; there is no telemetry.
  `/api/status` adds optional build metadata for clients.
- The demo market's scenario revision 9 covers seven underlyings, gives every strike
  of an ETF monthly its monthly open interest, holds SPY option vols after the close
  and an overnight index at `previous_close`, and makes an authored dividend its
  quarter's only payment.

### Fixes

- A clean shutdown no longer sends a feed-stalled notification for every
  underlying.
- Playbooks saved under the 8 MiB limit are written within it, so startup accepts
  them; a journal failure behind an automatic playbook cancel or close fails only
  that account instead of the engine thread.
- A holding window ending at `24:00` is midnight of the next day, so hold
  restrictions are enforced. The Cboe holiday listing drops an upcoming closure it
  no longer lists.
- Imported fill quantities stay within a million contracts and paper outcomes sum
  whole blocks, so misread columns cannot overflow totals.
- Market-halt state is synced before rename and a failed write is retried; sandbox
  notes, alerts and trade grouping share the order rate limit.

## 0.4.0 (2026-10-06)

### Evaluation plans

- **Practise more kinds of evaluations.** Static floors, floors that stop trailing,
  daily loss rules, minimum days and best-day consistency join the custom plan
  editor. Dashboard shows what remains to pass.
- **Set trade-level objectives.** Require closed whole option trades, limit the
  biggest trade's share of profit, set minimum holding time or cap profit from
  very short trades. Protective and system exits still work during a minimum hold.
- **Control when and what you trade.** Set evaluation and inactivity deadlines,
  allowed symbols, entry hours, mandatory flat time and no-overnight rules. Saved
  event calendars add news blackouts and weekend, earnings and dividend restrictions.
- **Limit opening exposure.** Cap contracts held, require stops, limit risk per
  trade, forbid opposing positions or cap holdings against traded option volume.
  Volume caps refuse entries when volume is unknown or stale; stops can still slip.
- **Follow a two-step programme.** A passed challenge unlocks verification, then
  simulated funded trading. Add payout consistency, retained buffers, contract
  scaling and funded size reviews. Fees and reset limits track programme costs
  without deducting trading cash. Funded screens stay hidden unless enabled or
  already funded; OpenPort funds no one and pays no real withdrawals.

### Orders, exits and fills

- **Keep protection through partial fills.** Brackets grow with entries and shrink
  with exits. Attach stop/target pairs to held options or spreads, use stop-limits
  or trailing stops, or link orders so one cancels or triggers another.
- **Trade across sessions.** All-session orders and timestamp expiry join DAY/GTC.
  Conditions can watch another symbol, volatility or a New York time. Overnight
  and curb execution needs limits; new orders, including GTC, are refused between
  product sessions. Missing trigger data does not trigger an order.
- **Choose inside fills and walking limits.** Limits can fill at midpoint or a
  chosen share of the spread, or move in steps toward a cap. These are optional;
  As displayed remains the default. Neither model simulates queue position.
- **Preview costs, fills and order changes.** Choose flat or itemized fees and see
  why an order waits. Fills retain the quote, displayed size and quote age.
- **Close and adjust the whole position.** Roll strikes or an entire condor;
  Flatten keeps spread closes working across quotes and preserves remaining
  protection. Preview what it leaves behind, or use limit flatten outside regular
  hours. Worthless longs can be abandoned, or marked do-not-exercise for expiry.

### Shares, margin and risk

- **Trade stock and ETF shares** during stock regular hours, with buying-power and
  delta previews. Covered-call and collar templates buy shares first: shares remain
  if the option order fails or partly fills. Automatic playbooks still trade options.
- **Choose cash, IRA or margin accounts.** Add house requirements or portfolio
  volatility shocks. Strategy margin recognises share covers, straddles and
  combinations across expiries, showing the allocation and any conservative fallback.
- **Compare adjustments before trading.** What-if compares buying power, Greeks,
  scenario losses and floor room. Held-book warnings and per-symbol limits help
  track exposure; risk profiles add later dates, IV changes, beta weighting and
  probability cones. These are model estimates, not predictions.

### Journal and account review

- **Keep rolls and adjustments in one whole trade.** Group entries for review,
  retain planned risk, and compare return on buying power and P&L by Greek.
  Exports identify the attempt, replay, scenario and seed, and label forced exits.
- **Inspect settlement sources.** Journal shows references and provenance. New
  demo and replay runs use the expiry opening print for AM settlement, an
  approximation of the official quotation; live feeds still need manual imports.
- **Manage accounts.** Rename, archive or delete accounts, or copy risk settings.
  Deleted files remain; damaged journals are readable through verified history.

### Replays and practice days

- **Carry one account through several sessions** with overnight gaps, GTC orders,
  assignment and dividends. Scenarios add XSP, NDX, RUT and VIX, American ETF
  pricing and simulated quarterly SPY/QQQ dividends. Stress days can include bad
  quotes, feed stalls, halts and early closes; these are not historical reconstructions.
- **Step, watch or retry a drill.** Advance by time or the next snapshot, play at
  a chosen speed to a target, or interrupt a long step. Restart at a chosen time
  with the original commands through that point in a new run, keeping the source.
- **Resume unfinished runs after shutdown** and verify finished runs in the
  terminal, with a downloadable receipt. Verification detects changed inputs and
  damaged history; identical results are not promised across different platforms.

### Playbooks and backtests

- **Add technical entry and exit rules.** Use moving averages, RSI, Bollinger
  bands, VIX levels and opening gaps; manage trailing profit, days to expiry and
  trading-day deadlines. Missing history blocks entries rather than inventing data.
- **Forward-test automatically on live paper accounts.** Auto resumes after restart
  and compares each version with saved backtests. Normal trading checks still apply.
- **Test two to eight playbooks together** on one account with shared buying power
  and plan limits, plus results for each playbook and daily evaluation progress.
  Comparing and summing separate runs remains an independent-results comparison.
  Open positions at the end stay reported, without invented closing prices.

### Alerts and upgrade notes

- **Keep account alerts running with the terminal closed.** Watch options, spreads,
  IV, equity, floor room and exposure, with optional external delivery. Demo and
  replay forwarding needs explicit opt-in; backtests and sandboxes never forward.
- **Older journals still load.** Saved scenario runs retain their original price
  generation for verification. Resume/restart requires supported runs and unchanged
  inputs; for an older live demo journal, keep its original revision and day settings.
- **Resets preserve daily discipline.** Profit locks, trade limits, opening counts
  and active cooldowns survive an account reset. New 100% soft-floor settings are
  refused, though stored ones still load. Passed or failed attempts can still close.
- **Pin backtests you want to keep.** The server now defaults to retaining 20
  finished unpinned runs, pruning older excess runs before starting another.
  Set retention to zero for unlimited storage; CLI reports are outside this policy.
- **Practice is not a prediction.** Delayed quotes give hindsight, and generated
  days cannot establish future performance. Historical importers and providers
  other than Cboe, Demo and Replay still have not been run live with a key/account.

### Fixed

- **Closing and buying power** now handle partial protection, worthless wings,
  shared covers and margin released by fills more consistently. Reductions can
  proceed over exposure limits, but removing a hedge can still increase risk and fail.
- **Replay timing** keeps same-time quotes together, preserves gaps after stepping
  and refuses out-of-range steps before moving. Storage failures are clearer.
- **P&L and history** preserve prior-day marks, dividends and whole-trade labels.
  Resets restore original scaled plan limits. Profit-taking exits no longer start
  a stop cooldown unless the separate loss threshold is met.
