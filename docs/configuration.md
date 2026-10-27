# Configuration

Providers, command-line flags, terminal settings and who may write.

## Providers

| Provider | `--provider` | Data | Key |
| --- | --- | --- | --- |
| Cboe delayed | `cboe` (default) | 15-minute delayed chain snapshots for US index and equity options, with session volume, open interest and Cboe's Greeks, polled every 15 s from Cboe's data files, or about once a minute from its quote pages when the files fall behind | none |
| Databento | `databento` | Real-time OPRA consolidated quotes (`cbbo-1s` or `cmbp-1`), trades and open interest, streamed | `DATABENTO_API_KEY` |
| Massive | `massive` | Option chain snapshots, real-time or delayed depending on your plan, polled every 5 s | `MASSIVE_API_KEY` |
| ThetaData | `thetadata` | Snapshots from your local Theta Terminal (v3), polled every 2 s | Theta Terminal login |
| Tradier | `tradier` | Option chain snapshots and underlying prices; production options are real-time, index timing is unconfirmed. Sandbox is 15-minute delayed. Polling follows the request budget. Not yet run live with an account; reports welcome | `TRADIER_ACCESS_TOKEN` |
| tastytrade | `tastytrade` | Production DXLink option quotes, session volume, open interest, vendor Greeks and underlying prices. Not yet run live with an account; reports welcome | `TASTYTRADE_CLIENT_SECRET`, `TASTYTRADE_REFRESH_TOKEN`; optional `TASTYTRADE_CLIENT_ID` |
| Charles Schwab | `schwab` | Trader API option chain snapshots with the underlying quote, open interest, volume, delta and gamma; real-time or delayed as each response's `isDelayed` says. Polling follows the 120 requests a minute budget. Not yet run live with an account; reports welcome | `SCHWAB_CLIENT_ID`, `SCHWAB_CLIENT_SECRET`, `SCHWAB_REFRESH_TOKEN` |
| Alpaca | `alpaca` | Option chain snapshots for equity and ETF underlyings (Alpaca lists no index options, so no SPX, NDX, RUT or VIX), from the free indicative feed (`--option feed=indicative`, derived quotes, not the OPRA NBBO) or OPRA with a subscription (`feed=opra`); volume, delta and gamma, no open interest. Not yet run live with an account; reports welcome | `APCA_API_KEY_ID`, `APCA_API_SECRET_KEY` |
| Interactive Brokers | `ibkr` | Client Portal Web API snapshots through the local gateway you have logged in to, with open interest, delta and gamma; needs `--expiries` and `--window`, because every contract takes one of the account's market-data lines. Build option `OPENPORT_WITH_IBKR` (on by default). Not yet run live with an account; reports welcome | the gateway's own login |
| Demo | `demo` | Simulated SPX, SPY, QQQ, XSP, NDX, RUT and VIX regular sessions, rotating on successive trading dates; `--option days=trend,chop --option speed=60` | none |
| Replay | `replay` | A recording played back as the whole feed, at 1×, 10×, 60× or full speed (`--option file=PATH --option speed=10`) | none |

Databento, Massive and ThetaData follow their documented APIs and are tested against
sample responses, but have not yet been run live with a key. If you have one, an
[issue](https://github.com/armanruzgar/openport/issues) saying how it went is welcome.

The Tradier, tastytrade, Schwab, Alpaca and Interactive Brokers adapters read market
data only. They never send broker orders or read balances or positions; the
Interactive Brokers gateway requires one call listing account IDs before market
data, and its answer is discarded. Credentials come from environment variables, never flags.
tastytrade requires a funded production account; its sandbox has no market data.
Tradier option sizes are read as contracts and DXLink times as milliseconds, each with
an option to override. Documented ORATS and DXLink IV/Greek units are mapped, with
session volume when its source clock is known; these mappings are not yet validated
live. ThetaData maps IV, delta and vega/rho (divided by 100), leaving its ambiguous
theta time basis unknown. [Unit sources](providers.md#vendor-units-and-source-references).
See [broker operation and limitations](runtime.md#broker-market-data) before
using any of these feeds for paper trading.

Providers deliver very different things: Databento sends raw exchange quotes with no
Greeks and no underlying price, while others ship their own Greeks. OpenPort normalises
all of them into the same contracts, quotes, volume and open interest, then computes
everything itself, so the numbers mean the same thing whichever provider you use.

To test a feed, run `openport-probe --certify PROVIDER SYMBOL... --report-dir DIR`.
Polling defaults to one cycle; streaming defaults to 60 seconds. `--seconds N`
selects a duration, or a polling deadline with `--snapshot`. The directory must
be new. [Report contents and exit codes](providers.md#live-certification-reports).
To add a feed, see [writing a provider adapter](providers.md).

## Command-line flags

| Flags | Controls |
| --- | --- |
| `--provider NAME`, `--poll-seconds N`, `--option KEY=VALUE` | The market-data provider and its settings, such as `quotes=cmbp-1` for Databento or `sandbox=true` for Tradier; Tradier validates poll intervals against its request budget |
| `--provider demo --option days=ID,ID,... --option speed=N` | Built-in regular scenarios in the given order; default all regular scenarios at 1×, supported speeds 1, 2, 5, 10, 30, 60, 120, 300. Symbols default to their coverage. Network services are off; candles stay in memory unless `--candle-dir` is given |
| `--fallback-provider NAME`, `--fallback-seconds N`, `--fallback-option KEY=VALUE` | A second live provider that supplies an underlying once the main one has been in error or silent for N seconds (default 30, at most 3600), until it has been fresh again for N seconds; its options and key work as for `--provider`. Data from the backup reports `delayed` with a fallback label, and fills name their feed ([failover](runtime.md#feed-failover)) |
| `--symbols SPX,SPY,QQQ,IWM,DIA`, `--expiries N`, `--window F` | The underlyings (default SPX, SPY, QQQ, IWM and DIA), the nearest N expiries and strikes within ±F of spot |
| `--rate R` | The rate assumed when no index curve is available |
| `--american-model lr\|bs2002\|baw` | How American expiries' early exercise premium is removed before IV solves: the default 31-step Leisen-Reimer tree (`lr`), or the faster Bjerksund-Stensland (2002) or Barone-Adesi-Whaley closed forms. Expiries with cash dividends always use the tree. Accuracy and timing are in [American analytics](american-analytics.md#closed-form-alternatives) |
| `--address`, `--port`, `--web-root`, `--allowed-origin`, `--allowed-host`, `--write-token`, `--write-token-file` | The web server and who may write (see [Security](#security)) |
| `--token-file FILE` | Named tokens: one `NAME SCOPES SECRET` per line, with comma-separated scopes and `#` comments |
| `--open` | Open the terminal after startup; opt-in even on a TTY. Creates a token file beside the journal when no token is configured. The token link is the browser launcher's argument, which `ps` shows other local users while it runs; on a shared machine use `--print-url` and paste the link |
| `--print-url [--open]` | Read existing token settings, print the local terminal link and exit; optionally open it. Reuse the service's address, port and token-file flags. A host name address such as `localhost` is used as given |
| `--no-token-link` | At startup, print the `--print-url` command for a token file instead of its token link, for services whose stdout goes to a shared log such as journald. The systemd unit uses it |
| `--crash-log PATH` | Local fatal signal log; defaults to `crash.log` beside the paper journal. No network reporting |
| `--notify-config FILE` | Owner-only JSON file for notification channels, filters and the `include_simulated` demo/replay opt-in (default false); alternatively `OPENPORT_NOTIFY_JSON` or channel environment variables ([setup](runtime.md#external-notifications)) |
| `--sandboxes N` | Offer up to N visitor accounts; 0 (default) disables them. Requires `--provider demo` and paper trading |
| `--sandbox-idle-seconds N` | Delete sandboxes after N seconds without authenticated use; default 86400 |
| `--client-ip-header NAME` | Use this proxy header for sandbox creation limits; unset uses the connection address. Also declares a trusted proxy whose `X-Forwarded-Proto: https` permits [static webhook signals](api.md#static-mode-senders-that-cannot-set-headers) |
| `--require-token` | Require a token for API reads, WebSocket ticks and writes, including loopback; static terminal files remain public |
| `--paper-journal PATH`, `--plan ID`, `--paper-cash`, `--paper-fee`, `--no-paper` | The main paper account (demo defaults to `~/.openport/demo/paper-journal.jsonl`); plan, cash and fee seed a new journal only; the fee also seeds new named accounts and visitor sandboxes. Rules selects optional fill models and an itemized fee schedule for a new attempt. Equity history is kept beside each journal as `.equity.csv`; playbook definitions and modes are in `playbooks.json` beside the main journal |
| `--scenario-dir DIR` | User JSON scenarios, listed after built-ins and overriding matching ids ([format](scenarios.md)) |
| `--backtest PLAYBOOK[@VERSION][,PLAYBOOK[@VERSION]...]`, `--days FILE`, `--recordings DIR`, `--scenarios N --seed S`, `--out REPORT.json` | Headless batch backtest under `--plan`; a comma-separated list selects 2–8 distinct playbooks sharing one account in lexical ID order; choose one day source. `--workers 1..16` defaults to 4; `--playbooks FILE` overrides the saved catalogue. [Inputs and reports](playbooks.md#batch-backtests) |
| `--verify-run JOURNAL` | Reproduce a saved replay or scenario from its recorded input and commands; exit 0 on matching transaction hashes and final equity, 1 otherwise |
| `--record FILE`, `--record-dir DIR` | Recording the feed to a file, or rolling by trading date into a directory the Replay page reads; directory recordings publish only on clean close |
| `--import-day databento\|thetadata\|massive --date YYYY-MM-DD --symbols SPX,SPY` | Import a completed past trading day and exit; accepts `--expiries N` and `--window F` |
| `--import-days YYYY-MM-DD..YYYY-MM-DD` | Inclusive date range with `--import-day PROVIDER`; repeat this or `--date` to combine dates (sorted, deduplicated, at most 3660 days). Skip closures and resume matching complete files; report day/window progress |
| `--record-keep-days N`, `--record-max-gb X` | Optional retention for `--record-dir` without `--record`; oldest complete files first. N calendar days by trading date; X decimal GB. Both default to 0 (unlimited); never remove the file being written. Stale `.record-*` staging folders count toward X and are reported, never deleted |
| `--out DIR` | Import destination, default `./recordings`; select this directory with `--record-dir` to list its days in Replay |
| `--candle-dir DIR`, `--no-history` | Where chart history is kept, and whether Cboe backfills prices and volatility-index proxies ([price history](runtime.md#price-history)) |
| `--series-dir DIR`, `--no-series` | Local minute volatility history; defaults to `series` beside the candle directory, or disables collection ([storage](runtime.md#volatility-series-storage)) |
| `--backfill-series FILE...`, `--force` | Build metric history offline from recordings and exit; keep existing minutes unless forced; refuse demo recordings |
| `--dividends FILE\|massive` | Known cash dividends for held shares and American analytics: `SYMBOL,YYYY-MM-DD,AMOUNT` lines, the ex-date and dollars a share, taken from the fund's own schedule; or `massive` to read them from Massive's API every six hours with `MASSIVE_API_KEY` |
| `--events FILE` | Supplied `YYYY-MM-DD,Label` lines for the implied-session-move table; no event dates are bundled ([volatility](volatility.md)) |
| `--no-cboe-holidays` | Don't read Cboe's published holiday schedule, which lets a special closure it announces apply without a new build ([calendar](runtime.md#product-sessions-and-cboe-clocks)) |

Every value is range-checked; `openportd --help` lists every flag, and the
[runtime notes](runtime.md) cover the details.

`--plan` accepts `practice`; `intraday-*`, `eod-*`, `static-*`, `locking-*`,
`two-step-*`, `two-step-verify-*`, `two-step-funded-*`, `funded-intraday-*` and
`funded-eod-*` at `25k`, `50k` and `100k`; and `funded-scaling-50k`.
`--plan` seeds any preset directly in a new or empty journal, including verification
and funded steps. Existing journals restore their saved rules regardless of `--plan`.
Help lists each ID and marks HTTP prerequisites with `after X`. HTTP account creation
cannot seed locked presets; HTTP resets enforce prerequisites with `PLAN_LOCKED`.
Pass the prerequisite in the same account before resetting into the next step.

## Terminal settings

| Terminal setting | Default | Controls |
| --- | --- | --- |
| Status → Updates → Check for updates | Off | Browser update checks, saved in this browser |

Update checks are opt-in and off by default. When on, the web terminal itself, not
the server, asks `https://api.github.com/repos/armanruzgar/openport/releases/latest` at most
once a day. It compares that release with the running version from `GET /api/status`,
and shows a small dismissible notice with a link to the release when it is newer.
Nothing else is sent. A network failure stays silent.

## Security

For private phone access or an authenticated proxy, see
[remote access](remote-access.md). For data protection and version changes, see
[upgrade and backup](upgrade-and-backup.md).

openportd binds to 127.0.0.1 by default. Reads are public unless `--require-token`
is set; loopback writes without a configured legacy token remain open unless that
flag or `--sandboxes` is set. A public demo with sandboxes often sits behind a local
reverse proxy, so its operator trades with a write token. The existing `OPENPORT_WRITE_TOKEN`, `--write-token TOKEN` and
`--write-token-file PATH` token grants `admin` and requires authentication for writes.
Named tokens from `--token-file FILE` grant `read`, `trade:ACCOUNT`, `trade:*`,
`replay` or `admin`. `replay` permits replay controls and backtest starts and
cancellation. `admin` includes account creation, limits, guardrails, resets,
the kill switch, playbook definitions and history deletion. Writes always check the token they carry;
without `--require-token`, reads ignore one that matches nothing, except expired
sandbox credentials when sandboxes are offered. The server prints
only the link for a token it keeps in a file, as the Docker image does, and never
prints named tokens. The token file must hold a valid token at startup; after that the
server reloads it whenever its modification time or size changes, on `SIGHUP`, or on an
admin `POST /api/tokens/reload`, so edits rotate or revoke tokens at once, without a
restart. A malformed replacement keeps the last good set; an empty one revokes every
named token ([API](api.md)). Each secret is at least eight printable characters,
and a `#` starts a comment only at the start of a field. Protect the file with owner-only permissions. Send Bearer credentials over
HTTPS behind a reverse proxy for remote access. Static
files are confined to the web root, and WebSocket upgrades must come from the same
origin; behind a proxy that rewrites the Host header, list your public origin with
`--allowed-origin`. Every request must address the server by an IP address, `localhost`,
the host of an allowed origin, or a name given with `--allowed-host` (such as a proxy's
upstream name), so a web page cannot reach it through DNS rebinding. Check your data
provider's terms before sharing an instance with anyone else.

Journal repair accepts `openportd --repair-journals --dry-run` to report each
file, bytes that would be cut, and verified records kept without changing files or
creating backups. Add `--file PATH` to inspect or repair only that journal. Without
`--file`, the main journal and its `accounts/` and `replays/` journals are selected.
Stop the server first: dry-run also requires the journal lock. Exit 0 means all
selected journals were inspected/repaired; exit 1 means at least one failed
(including damage before the last line, which is never cut). `--dry-run` and
`--file` require `--repair-journals`; invalid options exit 2. Repair preserves the
original in a timestamped sibling backup.

## Diagnostics

Status → **Copy diagnostics** copies the running version, source revision, compiler,
server build OS/architecture, provider, feed state, browser connection state and
recent server error categories. A selectable preview remains when clipboard access
is denied. **Report a bug on GitHub** opens a prefilled issue for you to review
and submit. Nothing is uploaded automatically: there is no telemetry.

Raw errors can contain credentials or paths, so diagnostics export only known
HTTP/error codes or a generic error category. They exclude URLs, tokens, account
data, local directories and raw provider messages. Recent errors are the last
errors reported by the server, not a complete log. Older servers show unavailable
build fields; builds without Git metadata report `unknown` unless configured with
`-DOPENPORT_BUILD_REVISION=REVISION`. Build revisions are captured at CMake configure
time, with `-dirty` for tracked modifications.

On macOS/Linux, normal server startup opens an owner-only local `crash.log` beside
the journal (`--crash-log PATH` overrides it). Fatal SIGABRT, SIGSEGV, SIGBUS, SIGILL
and SIGFPE append a signal name using async-signal-safe operations, then retain the
normal fatal signal exit. Startup lines identify the version/build/platform.
The log contains no memory dump or stack trace; SIGKILL, power loss, failures
before initialization and some stack overflows cannot be logged. Failure to open
the log produces a warning and does not prevent startup. The log is append-only;
archive/rotate it while the server is stopped. It is never sent anywhere.
