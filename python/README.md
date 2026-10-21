# Scripting and agents

The Python client and MCP server use the terminal's HTTP API. They trade a paper
simulator on the data openportd has, often delayed. They cannot send live orders.
Scenario and demo prices are generated, and are labelled simulated. Account money
is decimal text; use `decimal.Decimal` for calculations.

The client supports Python 3.10+ and has no runtime dependencies. The intended
release command is `pip install openport`; install this unreleased client from a
checkout with `python -m pip install ./python`. Optional extras are `./python[pandas]`
for frames, `./python[ws]` for WebSockets, and `./python[mcp]` for the MCP server
(`mcp>=2.2,<3`). A source checkout also works with `PYTHONPATH=python/src`.

```python
from openport import Client, ApiError

client = Client("http://127.0.0.1:8080", token="YOUR_TOKEN", account="practice", timeout=30)
print(client.status())                 # provider, delay and per-symbol market times
print(client.volatility("SPX"))        # includes missing-value reasons
print(client.portfolio())
print(client.preview_order(symbol="SPXW  261022P05000000", side="buy",
    type="limit", time_in_force="day", quantity=1, limit_price="0.05"))
```

Discover current contract symbols with `chain()` before submitting an order; the
symbol above is an example. `place_order` and `preview_order` generate a UUID
`client_order_id` if omitted. The client retries HTTP 503 up to three times,
reusing the same body and ID: after the server's `Retry-After` seconds (at most 10;
a full command inbox sends 1), or else after 0.1, 0.2 and 0.4 seconds. Other HTTP failures
raise `ApiError`, carrying `status`, `code` / `reason_code`, and the server's
`actual`, `limit` and `scope`. Transport failures are not retried. If retrying an
order after an uncertain response, supply the original ID and identical terms.
Other writes do not have the order endpoint's idempotency guarantee.

Results are dictionaries annotated by small `TypedDict`s in `openport.types`.
Unknown fields are preserved. The client never replaces missing quotes or P&L
with zero. It does not coerce monetary strings into floats.

| Methods | HTTP routes |
| --- | --- |
| `status`, `symbols` | `/api/status`; symbols are its underlying names |
| `summary`, `series` | `/api/underlyings/{symbol}/summary`; series are its expiries |
| `chain`, `exposure`, `surface`, `volatility`, `candles`, `probability` | The corresponding underlying views |
| `account`, `portfolio`, `orders`, `fills`, `trades`, `risk`, `risk_profile`, `plans`, `accounts`, `equity` | The terminal's paper reads |
| `place_order`, `preview_order`, `modify_order`, `cancel_order`, `cancel_all`, `cancel_orders`, `flatten` | Paper orders and position closure |
| `settlements` | Settlement references, proceeds and provenance, newest first (live, replay or history) |
| `note`, `day_note`, `export_csv` | Trade/day annotations and fills/trades exports |
| `group_trades`, `ungroup_trades` | Join open round trips into one whole trade, or take one out |
| `create_account`, `reset_account`, `payout`, `limits`, `guardrails`, `kill`, `settle`, `exercise`, `close_stock`, `trade_stock`, `preview_stock` | Remaining terminal commands |
| `list_replays`, `start_replay`, `resume_replay(run_id)`, `control_replay`, `step_replay(until)`, `stop_replay`, `delete_replay` | Replay controls and history deletion |

`client.for_replay("main")` reads and trades the active replay's isolated account.
`client.for_history(run_id)` reads a finished run. Replay controls always address
`/api/replay` and never receive the client's `account` query parameter. Historical
runs retain accounts and trades, not complete market analytics.

```python
from openport import chain_frame, smile_frame, trades_frame, ticks

chain_df = chain_frame(client.chain("SPX"))       # one row per strike and side
smile_df = smile_frame(client.surface("SPX"))    # one row per expiry and smile point
trades_df = trades_frame(client.trades())        # option round trips; money stays text

async def watch():
    async for tick in ticks("ws://127.0.0.1:8080/ws", token="YOUR_TOKEN"):
        print(tick)
```

Frame helpers do not add or estimate missing values. The WebSocket iterator yields
raw tick dictionaries and closes with its async context; it does not reconnect or
reconstruct messages missed during a disconnect.

## MCP clients

Set `OPENPORT_WRITE_TOKEN` in the MCP process's environment. Use a named token with
`read,trade:practice,replay` for an agent that may read, trade `practice` and run
replays. Use `--require-token` on openportd if reads must also be authenticated.
Without a token the MCP tools can read a public instance, but cannot write even if
the server permits unauthenticated loopback writes.

After installing `./python[mcp]`, add it to Claude Code:

```sh
claude mcp add openport -- python -m openport_mcp --url http://127.0.0.1:8080
```

Ensure the launched process inherits `OPENPORT_WRITE_TOKEN`. Other MCP clients
can launch the same stdio process:

```json
{
  "mcpServers": {
    "openport": {
      "command": "python",
      "args": ["-m", "openport_mcp", "--url", "http://127.0.0.1:8080", "--agent-name", "research"],
      "env": {"OPENPORT_WRITE_TOKEN": "YOUR_NAMED_TOKEN_SECRET"}
    }
  }
}
```

Store the actual secret in your client's protected environment configuration. It
is never a tool argument. The installed SDK is MCP 2.x: `MCPServer`, not `FastMCP`.

Tools include status, symbols, summary, chain, volatility, exposure, account,
positions, orders, risk, risk profile, probability cones, order preview/place/cancel, flatten, scenario listing and
replay start/step/stop (scenarios or recordings). Chain results default to a 3% strike window and at most 80
strikes; a capped response says `truncated: true`. Windows must be in `(0, 0.1]`.

Batch tools label simulated trading and leave aggregate market time/feed null;
each report day retains its own input provenance. Other tool results include `as_of` from market data, `feed` with provider and delay,
and `simulated`. Unavailable source metadata is null. Source timestamps precede a
command unless its response contains a newer market timestamp; replay controls
refresh the replay's published quote times. Replay receipt clocks are not used as
market timestamps. Scenario entries
are also explicitly labelled simulated. Errors retain reason codes. Writes name
an account and return `paper: true`; replay writes must name `main`, the isolated
replay account. Placed and previewed orders carry `agent:NAME`. The authenticated
token's name is the journal actor; an agent tag is descriptive, not authentication.

The MCP server defaults to stdio. It delegates scope enforcement to openportd and
does not have any broker or live-trading connection.

## Examples and checks

`examples/volatility.py` prints SPX's 30-day model-free IV and skew.
`examples/put_spread.py --account practice` discovers quoted puts, previews a
vertical and places a paper limit with a bracket. `examples/last_hour.py SCENARIO`
starts a paused simulated day, steps to 15:00 New York time, places that spread,
steps to 15:59 and flattens. They use `OPENPORT_URL` and `OPENPORT_WRITE_TOKEN`.
The last-hour example requires an SPX scenario that spans those times. Quotes,
liquidity, the plan and session can reject these examples; no result is promised.

From the repository root, using the prepared environment:

```sh
.venv-py/bin/python -m pytest python/tests
.venv-py/bin/python tools/contract_test.py http://127.0.0.1:8080 --token "$OPENPORT_WRITE_TOKEN" --spec docs/openapi.yaml
```

Tests use a threaded `http.server` with synthetic JSON. When a sandbox denies TCP
binding, the same HTTP handler runs over a local socket pair. If the C++ test
binary is built, Python also validates real handler responses captured without a
listener. The contract command checks every API GET and a preview, resting limit
and cancellation in a new simulated replay. It refuses to replace an active run,
requires durable replay history and leaves the completed test run in history.
Static files and WebSocket transport are described in OpenAPI; the HTTP contract
command checks the JSON API and CSV routes. The first mismatch prints its route
and field and exits nonzero. CI runs it after the Docker smoke test.

Batch backtests use the global routes even on a replay client. Results are simulated
trading on recorded or generated days, not predictions or investment advice:

```python
run = client.start_backtest("morning-put@2", "eod-50k", scenarios=20, seed="81723")
# Or pass 2–8 distinct IDs to trade one shared account, in lexical ID order:
# run = client.start_backtest(["morning-put@2", "afternoon-call"], "eod-50k", scenarios=20, seed="81723")
progress = client.get_backtest(run["id"])
reports = client.list_backtests()
client.cancel_backtest(run["id"])  # only while running
comparison = client.compare_backtests(["000001", "000002"])
client.keep_backtest("000001")  # exempt from automatic retention
client.delete_backtest("000002")  # finished runs only; permanently removes all files
```

Pass `days=[{"file": "day.oprec"}, ...]` for recordings, including imports, or
explicit scenario entries with `scenario`, `date` and `seed`. Dates must increase.
The `replay` scope permits starts and cancellation. See the
[batch contract](../docs/playbooks.md#batch-backtests) for attempt rules and journals.

Finished replay runs can be verified without stopping live playback:
`client.verify_replay(run_id)` starts the background job;
`client.replay_verification(run_id)` reads `idle/running/passed/failed`, progress
and the message. `client.replay_verification(run_id, receipt=True)` returns the
shareable JSON result with run inputs, plan, final equity/count/head, build and
verification times. A changed journal invalidates the saved result. Starts need
replay write scope; reads and receipts need read access.

Sandbox tokens default to their own account: `Client(url, token)` and MCP account
tools can omit `account`. Explicit account selections remain scope checked. Other
MCP writes require an account, and replay controls require `main`. Named-token
administrators can call `Client.reload_tokens()` to reload `--token-file` without
restarting; the response contains names, count and load time, never secrets.

`client.equity(start, end, limit=2000, cursor=page["next"])` optionally pages
history; omit limit for the full range. Continue with the same filters until
`next` is null. Live, replay and history clients use the same method. Responses
keep `error` as string/null and add failure wall/market timestamps and
`error_recovered`. `account()` and account listings expose a nullable `damaged`
object with the last verified sequence/time and recovery instructions; a damaged
account remains readable and refuses writes with `ACCOUNT_DAMAGED`.

Client parity includes `get_order`, `playbooks`, `get_playbook(id, version)`,
`create_playbook`, `version_playbook` (pass the current version), `archive_playbook`,
`playbook_mode`, `send_staged_order`, `dismiss_staged_order`, `notifications`,
`test_notification`, `configure_notification`, `create_sandbox`, `calendar_events`,
`pass_odds`, `strategy_template`, `replay_history` and `get_replay`. Notification
status comes from `status`; configuration/test require admin. Sandbox creation
returns a secret token; store it privately. MCP intentionally omits credential
issuance and notification administration. It exposes the other copilot reads and
playbook, alert, journal, what-if, backtest and order-change tools. Preview before
placing or modifying orders; review staged terms before sending. Playbook
definition/mode changes require admin; ordinary trade scope cannot enable auto.

## Strategies, events and exports

`openport.bot.Strategy` has `on_start()`, `on_tick(tick)`, `on_bar(symbol, bar)`,
`on_fill(fill)` and `on_stop()` hooks. Hooks can be sync or async and run serially;
`self.client` is assigned before start. Run with
`asyncio.run(openport.bot.run(strategy, client, symbols=("SPX",)))`, or construct
`Runner` to request `stop()` and inspect `event_error`. `poll_interval` defaults
to one second; `max_ticks` bounds an offline test. The runner never places orders
by itself: preview and use stable client IDs in your hooks.

Live and `for_replay()` clients prime existing fills, observe new fills once per
runner, and emit closed bars only when a newer observed bar exists. `on_tick`
receives a status snapshot on each poll or event wakeup. They do not
fabricate missing bars. Events wake fill reconciliation; REST polling also runs
when WebSockets are unavailable or disconnected. `event_error` retains a streaming
failure. Create a new runner to reconnect; REST remains the source of full fill
quote evidence. Slow hooks may delay callbacks. Keep a replay runner attached to
one run; create a new runner after restarting the replay. History clients walk
saved equity/fills in timestamp order and finish, without inventing unavailable
historical market bars. `on_stop` runs on cancellation and hook errors too.

`openport.stream.events(url, token=..., account=..., source="live")` yields typed
v1 `Event` instances, separately from coalescing `ticks()`. It closes on context
exit and does not reconnect automatically. The stream is bounded, not a durable
log; reconcile fills/orders after disconnect. See [event semantics](../docs/api.md#paper-event-stream-v1).
`examples/bot_monitor.py` observes live/history; `examples/bot_replay.py SCENARIO`
uses the previewed spread example on a synthetic replay, leaving it paused.

Install `openport[pandas]` for `fills_frame`, `trades_frame`, `equity_frame`,
`backtest_frame`, `chain_frame` and `smile_frame`. Install `openport[arrow]` for
`arrow_table(data, kind)`, `write_parquet(data, path, kind)` and
`write_arrow(data, path, kind)`, which do not need pandas. Kinds are `fills`,
`trades`, `equity`, `backtest`, `chain`; backtests take `section="days"` or
`"attempts"`. Select a sweep variant's report before export.

Dtype policy: decimal money strings, IDs and ISO timestamps stay strings; pandas
uses nullable string columns, Arrow uses UTF-8. Numeric market analytics remain
numeric. Missing values stay null; no timestamp is inferred. Nested lists/objects
become canonical JSON strings in Arrow/Parquet, preserving their decimal text;
frames retain them as Python objects. All-null columns have Arrow null type until
data establishes a type, and empty exports have no inferred columns. Schema
metadata records the money/nesting/time policy. Use `Decimal` explicitly for money
arithmetic, or cast a known column to a suitably sized Arrow decimal type yourself.
The offline `examples/columnar.ipynb` demonstrates exact round trips.

```python
job = client.start_sweep("morning@2", "eod-50k",
    {"/structure/template/width": [5, 10], "/management/take_profit_percent": [25, 50]},
    scenarios=5, seed="81723", workers=2)
report = client.get_sweep(job["id"])
# Signed evaluation, obeying the saved playbook mode; never automatically retried:
result = client.signal("morning")
```

## Python releases

The intended release installation is `pip install openport` (or `pip install 'openport[ws,mcp,arrow]'`).
The [PyPI name is already registered](https://pypi.org/pypi/openport/json); confirm
project ownership before tagging. Until this engine client is verified there,
install the checkout as above. The
`python-release.yml` workflow builds and checks an sdist and wheel, then uses
[PyPI trusted publishing](https://docs.pypi.org/trusted-publishers/using-a-publisher/)
with OIDC, without a stored API token. It triggers on `python-vVERSION` tags and
requires an exact match to `python/pyproject.toml`. Build/test and publish use
separate jobs; only the publish job can request an identity token.

Before the first release, the package owner must confirm access to the existing
`openport` project and register a trusted publisher for repository `38st/openport`,
workflow `python-release.yml` and environment `pypi` on PyPI, and configure that GitHub
environment. No package has been published as part of this change.
