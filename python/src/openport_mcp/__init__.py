# Copyright (c) 2026 OpenPort contributors. MIT License.
"""MCP 2.x tools for openport's paper simulator."""
from __future__ import annotations

from dataclasses import asdict, dataclass
import re
from typing import Literal

from mcp.server.mcpserver import MCPServer
from openport import ApiError, Client
from openport.types import ExposureConvention, ExposureSlice


@dataclass
class Leg:
    symbol: str
    side: Literal["buy", "sell"]
    ratio: int = 1


@dataclass
class Trail:
    unit: Literal["amount", "percent", "ticks"]
    value: str | int


@dataclass
class Trigger:
    """A level to arm an order until reached. underlying may name another underlying
    (symbol); study watches iv30, iv7 or term_ratio; time takes at ("15:30") and no level."""
    source: Literal["option", "underlying", "combo", "study", "time"]
    direction: Literal["at_or_below", "at_or_above"] | None = None
    level: str | None = None
    symbol: str | None = None
    study: Literal["iv30", "iv7", "term_ratio"] | None = None
    at: str | None = None
    reference: Literal["bid_ask", "mid", "mark"] | None = None
    trail: Trail | None = None


@dataclass
class Exit:
    trigger: Trigger | None = None
    limit_price: str | None = None


@dataclass
class Bracket:
    stop_loss: Exit | None = None
    take_profit: Exit | None = None


@dataclass
class Order:
    quantity: int
    type: Literal["limit", "market"]
    time_in_force: Literal["day", "gtc", "ioc", "exto", "gtc_exto", "gtd"]
    symbol: str | None = None
    side: Literal["buy", "sell"] | None = None
    legs: list[Leg] | None = None
    limit_price: str | None = None
    client_order_id: str | None = None
    trigger: Trigger | None = None
    bracket: Bracket | None = None
    note: str | None = None
    tags: list[str] | None = None
    exits_only: bool | None = None
    good_till: str | None = None
    then: Order | None = None
    oco: Order | None = None


def _body(value):
    if isinstance(value, dict):
        return {key: _body(item) for key, item in value.items() if item is not None}
    if isinstance(value, list):
        return [_body(item) for item in value]
    return value


def create_server(client: Client, agent_name: str = "openport") -> MCPServer:
    if not re.fullmatch(r"[a-z0-9_-]{1,26}", agent_name):
        raise ValueError("agent name must be 1–26 lowercase letters, digits, underscores or hyphens")
    server = MCPServer("openport", instructions=(
        "Trades a paper simulator only, on data openportd has, often delayed. "
        "Check as_of, feed and simulated on every result. Money is decimal text. "
        "Writes require OPENPORT_WRITE_TOKEN and an account, except sandbox tokens default to their own account. Preview before placing an order; preview_change before modifying one. Review staged orders before sending. "
        "Reuse client_order_id if repeating an order after an uncertain response."))

    def selected(account: str | None = None, replay: bool = False):
        return Client(client.base_url, client.token, account, client.timeout,
                      retries=client.retries, backoff=client.backoff, replay=replay)

    def result(target, action):
        # Capture the source even if a stop command removes the replay.
        try:
            status = target.status()
        except ApiError:
            status = {}
        provider = status.get("provider", {})
        times = [item["as_of"] for item in status.get("underlyings", []) if item.get("as_of")]
        envelope = {"as_of": max(times) if times else None,
                    "feed": {"provider": provider.get("name"),
                             "delayed": provider["delay_seconds"] > 0 if "delay_seconds" in provider else None,
                             "delay_seconds": provider.get("delay_seconds")},
                    "simulated": provider.get("simulated")}
        try:
            data = action()
            envelope["data"] = data
            if isinstance(data, dict):
                if "as_of" in data:
                    envelope["as_of"] = data["as_of"]
                elif "time" in data:
                    envelope["as_of"] = data["time"]
                if isinstance(data.get("replay"), dict):
                    state = data["replay"]
                    # Replay time/settled_through are receipt clocks. Market
                    # time comes only from the replay's published quote timestamps.
                    try:
                        replay_status = client.for_replay().status()
                        market_times = [item["as_of"] for item in replay_status["underlyings"] if item.get("as_of")]
                        envelope["as_of"] = max(market_times) if market_times else None
                    except ApiError:
                        envelope["as_of"] = None
                    envelope["feed"] = {"provider": state["provider"], "delay_seconds": state["delay_seconds"],
                                        "delayed": state["delay_seconds"] > 0}
                    envelope["simulated"] = state.get("demo", provider.get("simulated"))
        except ApiError as error:
            envelope["error"] = {"code": error.code, "message": str(error), "status": error.status}
        return envelope

    def write(account, replay, action):
        target = selected(account, replay)
        def authorized():
            if not client.token:
                raise ApiError(403, {"error": {"code": "WRITE_TOKEN_REQUIRED", "message": "Set OPENPORT_WRITE_TOKEN"}})
            if replay and account != "main":
                raise ApiError(400, {"error": {"code": "INVALID_ACCOUNT", "message": "Replay's isolated account is main"}})
            sandbox_default = account is None and client.token.startswith("sandbox_") and not replay
            if not sandbox_default and (not account or len(account) > 40 or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", account)):
                raise ApiError(400, {"error": {"code": "INVALID_ACCOUNT", "message": "Name the paper account"}})
            return action(target)
        response = result(target, authorized)
        response["account"] = account
        response["paper"] = True
        return response

    @server.tool()
    def trade_stock(symbol: str, side: Literal["buy", "sell"], shares: int, account: str | None = None, replay: bool = False) -> dict:
        """Trade shares at the fresh stock-session price. Not idempotent: inspect positions after an uncertain response."""
        return write(account, replay, lambda target: target.trade_stock(symbol, side, shares))

    @server.tool()
    def preview_stock(symbol: str, side: Literal["buy", "sell"], shares: int, account: str | None = None, replay: bool = False) -> dict:
        """Preview shares, including cost, buying power and dollar delta; does not trade."""
        return write(account, replay, lambda target: target.preview_stock(symbol, side, shares))

    @server.tool()
    def status(replay: bool = False) -> dict:
        """Feed health and market time."""
        target = selected(replay=replay)
        return result(target, target.status)

    @server.tool()
    def symbols(replay: bool = False) -> dict:
        """Subscribed underlying symbols."""
        target = selected(replay=replay)
        return result(target, target.symbols)

    @server.tool()
    def summary(symbol: str, replay: bool = False) -> dict:
        """Underlying and expiry analytics."""
        target = selected(replay=replay)
        return result(target, lambda: target.summary(symbol))

    @server.tool()
    def chain(symbol: str, expiry: str | None = None, window: float = 0.03, replay: bool = False) -> dict:
        """A chain within 3% of spot by default; maximum window 10%."""
        target = selected(replay=replay)
        def read():
            if not 0 < window <= 0.1:
                raise ApiError(400, {"error": {"code": "INVALID_WINDOW", "message": "window must be in (0, 0.1]"}})
            data = target.chain(symbol, expiry, window)
            # Bound responses even on unusually dense chains; retain closest strikes.
            spot = data.get("spot")
            if len(data["strikes"]) > 80:
                if spot is None:
                    raise ApiError(422, {"error": {"code": "MISSING_SPOT", "message": "Cannot center the chain without spot"}})
                data["strikes"] = sorted(sorted(data["strikes"], key=lambda row: abs(row["strike"] - spot))[:80], key=lambda row: row["strike"])
                data["truncated"] = True
            return data
        return result(target, read)

    @server.tool()
    def volatility(symbol: str, replay: bool = False, account: str | None = None) -> dict:
        """IV, skew, history ranks and derived event premiums from the selected account's calendar."""
        target = selected(account, replay)
        return result(target, lambda: target.volatility(symbol))

    @server.tool()
    def volatility_history(symbol: str, fields: list[str] | None = None, interval: Literal["1m", "1d"] = "1d",
                           from_: str | None = None, to: str | None = None, replay: bool = False) -> dict:
        """Stored volatility observations; missing sessions stay missing. Dates or ISO times bound the range."""
        target = selected(replay=replay)
        return result(target, lambda: target.volatility_history(symbol, fields=fields, interval=interval, from_=from_, to=to))

    @server.tool()
    def surface_history(symbol: str, compare: bool = True, from_: str | None = None,
                        to: str | None = None, replay: bool = False) -> dict:
        """Recorded SVI parameters, by default only prior-close, week and month comparisons. Missing captures remain null."""
        target = selected(replay=replay)
        return result(target, lambda: target.surface_history(symbol, compare=compare, from_=from_, to=to))

    @server.tool()
    def volatility_scanner(replay: bool = False) -> dict:
        """IV, rank, VRP, skew, term and today's implied move for all subscriptions; each row has its own market time."""
        target = selected(replay=replay)
        return result(target, target.volatility_scanner)

    @server.tool()
    def probability(symbol: str, days: list[float] | None = None, prices: list[float] | None = None,
                    replay: bool = False) -> dict:
        """Implied probability cones by horizon (days; 0 is today's close) and odds of finishing beyond or touching prices."""
        target = selected(replay=replay)
        return result(target, lambda: target.probability(symbol, days, prices))

    @server.tool()
    def flow(symbol: str, limit: int = 50, type: Literal["call", "put"] | None = None,
             only: Literal["sweep", "block", "multi_leg"] | None = None, replay: bool = False) -> dict:
        """Recent option prints classified at bid/ask/mid/outside, with net premium by expiry and strike.
        source "none" means the feed delivers no trades; "simulated" prints come only from the demo market."""
        target = selected(replay=replay)
        return result(target, lambda: target.flow(symbol, limit, type, only=only))

    @server.tool()
    def earnings(symbol: str | None = None) -> dict:
        """Earnings implied (front straddle) versus realized moves and how often the straddle priced more."""
        return result(client, lambda: client.earnings(symbol))

    @server.tool()
    def analysis_portfolios() -> dict:
        """Imported outside portfolios (needs an admin token). Analysis only: they are not paper accounts and cannot be traded."""
        return result(client, client.analysis_portfolios)

    @server.tool()
    def analysis_risk(portfolio: str, view: Literal["holdings", "profile", "grid"] = "holdings",
                      underlying: str | None = None) -> dict:
        """An imported portfolio's holdings and Greeks, risk profile or scenario grid. Read only."""
        if view == "holdings":
            return result(client, lambda: client.analysis_portfolio(portfolio))
        params = {"underlying": underlying} if view == "profile" and underlying else {}
        return result(client, lambda: client.analysis_risk(portfolio, view, **params))

    @server.tool()
    def exposure(symbol: str, replay: bool = False, convention: ExposureConvention = "long_calls_short_puts",
                 slice: ExposureSlice = "all", expiries: int = 8, window: float = 0.08) -> dict:
        """Derived OI-weighted GEX, VEX, DEX, daily charm and vomma, with walls/flips.

        Slice uses snapshot New York date. Customer flow is unavailable without
        aggressor direction; unsigned volume is not a positioning signal.
        Summary covers the full selected slice; arrays use the display limits.
        """
        target = selected(replay=replay)
        return result(target, lambda: target.exposure(symbol, expiries, window,
                                                      convention=convention, slice=slice))

    @server.tool()
    def account(account: str | None = None, replay: bool = False) -> dict:
        """Paper account rules and evaluation."""
        target = selected(account, replay)
        return result(target, target.account)

    @server.tool()
    def positions(account: str | None = None, replay: bool = False) -> dict:
        """Paper positions, cash and equity."""
        target = selected(account, replay)
        return result(target, target.portfolio)

    @server.tool()
    def orders(account: str | None = None, replay: bool = False) -> dict:
        """Working paper orders."""
        target = selected(account, replay)
        return result(target, lambda: target.orders("open"))

    @server.tool()
    def risk(account: str | None = None, replay: bool = False) -> dict:
        """Paper account limits, Greeks and breach estimates."""
        target = selected(account, replay)
        return result(target, target.risk)

    @server.tool()
    def risk_history(account: str | None = None, replay: bool = False) -> dict:
        """Derived historical stress P&L, margin and floor breaches, and one-day historical VaR/ES."""
        target = selected(account, replay)
        return result(target, target.risk_history)

    @server.tool()
    def risk_scenarios(account: str | None = None, mode: str = "parallel", days: list[float] | None = None,
                       benchmark: str | None = None, vols: list[float] | None = None,
                       term_scaled: bool = False, replay: bool = False) -> dict:
        """Read-only spot × volatility × time scenarios, optionally beta-weighted and term-scaled."""
        target = selected(account, replay)
        return result(target, lambda: target.risk_scenarios(mode, days, benchmark, vols, term_scaled))

    @server.tool()
    def risk_profile(account: str | None = None, underlying: str | None = None, benchmark: str | None = None,
                     days: list[float | str] | None = None, iv: float | None = None,
                     dynamics: str | None = None, replay: bool = False) -> dict:
        """The held book's P&L curves over dates (days, or "expiry") and an IV offset, against one underlying
        or beta-weighted to SPY or SPX, with the levels where each date reaches the floors. dynamics is
        sticky_strike (default), sticky_moneyness or sticky_delta, the last two along the SSVI surface."""
        target = selected(account, replay)
        return result(target, lambda: target.risk_profile(underlying, benchmark, days, iv, dynamics=dynamics))

    def order_body(order):
        body = _body(asdict(order))
        tag = "agent:" + agent_name
        tags = body.get("tags", [])
        if tag not in tags:
            tags.append(tag)
        body["tags"] = tags
        return body

    @server.tool()
    def preview_order(order: Order, account: str | None = None, replay: bool = False) -> dict:
        """Preview a paper order without changing the account."""
        return write(account, replay, lambda target: target.preview_order(order_body(order)))

    @server.tool()
    def place_order(order: Order, account: str | None = None, replay: bool = False) -> dict:
        """Place a paper order tagged with this agent's name."""
        return write(account, replay, lambda target: target.place_order(order_body(order)))

    @server.tool()
    def cancel_order(order_id: str, account: str | None = None, replay: bool = False) -> dict:
        """Cancel one paper order."""
        return write(account, replay, lambda target: target.cancel_order(order_id))

    @server.tool()
    def flatten(account: str | None = None, underlying: str | None = None, replay: bool = False,
                type: Literal["market", "limit"] = "market", limit_ticks: int | None = None) -> dict:
        """Close paper positions in one underlying, or the whole named account."""
        return write(account, replay, lambda target: target.flatten(underlying, type=type, limit_ticks=limit_ticks))

    @server.tool()
    def list_scenarios() -> dict:
        """Available simulated scenarios and recorded sessions, with their source labels."""
        def listing():
            data = client.list_replays()
            for scenario in data.get("demos", []):
                scenario["simulated"] = True
            if data.get("demo"):
                data["demo"]["simulated"] = True
            return data
        return result(client, listing)

    @server.tool()
    def start_replay(account: str, scenario: str | None = None, file: str | None = None,
                     seed: str | None = None, plan: str = "practice", start_at: str | None = None) -> dict:
        """Start a paused scenario or recording. Its isolated paper account is main."""
        def start(target):
            if account != "main":
                raise ApiError(400, {"error": {"code": "INVALID_ACCOUNT", "message": "Replay's isolated account is main"}})
            if bool(scenario) == bool(file):
                raise ApiError(400, {"error": {"code": "INVALID_REQUEST", "message": "Give one scenario or file"}})
            return target.start_replay(**_body({"scenario": scenario, "file": file, "seed": seed, "plan": plan, "start_at": start_at, "paused": True}))
        return write(account, False, start)

    @server.tool()
    def replay_from_trade(account: str, run_id: str, trade_id: str) -> dict:
        """Start a fresh paused paper account before a source trade; unchanged source data is required."""
        def start(target):
            if account != "main":
                raise ApiError(400, {"error": {"code": "INVALID_ACCOUNT", "message": "Replay's isolated account is main"}})
            return target.replay_from_trade(run_id, trade_id)
        return write(account, False, start)

    @server.tool()
    def scenario_packs() -> dict:
        """Export simulated scenario packs, seeds, plans and declared objectives."""
        response = result(client, client.scenario_packs)
        response["as_of"] = None  # Definitions have no market observation time.
        response["simulated"] = True
        return response

    @server.tool()
    def import_scenario_pack(account: str, pack: dict) -> dict:
        """Import a local JSON scenario pack. Requires replay scope and writable storage."""
        return write(account, False, lambda target: target.import_scenario_pack(pack))

    @server.tool()
    def start_scenario_pack(account: str, pack_id: str) -> dict:
        """Start a fixed pack or challenge paused, in the isolated main paper account."""
        def start(target):
            if account != "main":
                raise ApiError(400, {"error": {"code": "INVALID_ACCOUNT", "message": "Replay's isolated account is main"}})
            return target.start_scenario_pack(pack_id)
        return write(account, False, start)

    @server.tool()
    def step_replay(account: str, until: str) -> dict:
        """Step the scenario to a time, then pause.

        Use New York HH:MM[:SS] (in a run of several sessions, its next occurrence
        at or after the replay's time), or a date and time such as 2026-09-17T10:30,
        New York unless it ends in Z or a UTC offset.
        """
        return write(account, True, lambda target: target.step_replay(until))

    @server.tool()
    def stop_replay(account: str) -> dict:
        """Stop the replay; its journal remains available in history."""
        return write(account, True, lambda target: target.stop_replay())

    @server.tool()
    def fills(account: str | None = None, replay: bool = False) -> dict:
        """Paper fills, including exact prices and observed quote times."""
        target = selected(account, replay)
        return result(target, lambda: target.fills())

    @server.tool()
    def trades(status: str = "all", account: str | None = None, replay: bool = False) -> dict:
        """Journal trades and their notes."""
        target = selected(account, replay)
        return result(target, lambda: target.trades(status))

    @server.tool()
    def equity(start: str | None = None, end: str | None = None, limit: int = 500, account: str | None = None, replay: bool = False) -> dict:
        """Account equity samples; money stays decimal text."""
        target = selected(account, replay)
        return result(target, lambda: target.equity(start, end, limit=limit))

    @server.tool()
    def candles(symbol: str, interval: str = "5m", limit: int = 500, account: str | None = None, replay: bool = False) -> dict:
        """Observed OHLC bars, oldest first, with market timestamps."""
        target = selected(account, replay)
        return result(target, lambda: target.candles(symbol, interval, limit))

    @server.tool()
    def surface(symbol: str, expiries: int = 4, window: float = 0.1, account: str | None = None, replay: bool = False) -> dict:
        """Volatility surface and fit diagnostics; missing estimates stay null."""
        target = selected(account, replay)
        return result(target, lambda: target.surface(symbol, expiries, window))

    @server.tool()
    def calendar_events(start: str | None = None, end: str | None = None, account: str | None = None, replay: bool = False) -> dict:
        """Supplied calendar events; no inferred event times."""
        target = selected(account, replay)
        return result(target, lambda: target.calendar_events(start, end))

    @server.tool()
    def playbooks(account: str | None = None, replay: bool = False) -> dict:
        """Versioned playbooks, modes, stages and forward reports. Review a stage before sending it."""
        target = selected(account, replay)
        return result(target, lambda: target.playbooks())

    @server.tool()
    def get_playbook(playbook_id: str, version: int | None = None, account: str | None = None, replay: bool = False) -> dict:
        """Read a playbook definition, optionally an older version."""
        target = selected(account, replay)
        return result(target, lambda: target.get_playbook(playbook_id, version))

    @server.tool()
    def pass_odds(days: int = 20, samples: int = 1000, playbook: str | None = None, seed: str | None = None, account: str | None = None, replay: bool = False) -> dict:
        """Seeded historical pass estimate, not a prediction. Requires sufficient history."""
        target = selected(account, replay)
        return result(target, lambda: target.pass_odds(days, samples, playbook=playbook, seed=seed))

    @server.tool()
    def strategy_template(symbol: str, expiry: str, template: dict, account: str | None = None, replay: bool = False) -> dict:
        """Select option legs without placing an order. Preview the resulting order before sending."""
        target = selected(account, replay)
        return result(target, lambda: target.strategy_template(symbol, expiry, template))

    @server.tool()
    def alerts(account: str | None = None, replay: bool = False) -> dict:
        """Account alert definitions and trip history."""
        target = selected(account, replay)
        return result(target, lambda: target.alerts())

    @server.tool()
    def what_if(candidates: list[dict], account: str | None = None, replay: bool = False) -> dict:
        """Compare up to six candidate adjustments on private copies; never trades."""
        return write(account, replay, lambda target: target.what_if(candidates))

    @server.tool()
    def preview_change(order_id: str, change: dict, account: str | None = None, replay: bool = False) -> dict:
        """Preview changed order terms before calling modify_order."""
        return write(account, replay, lambda target: target.preview_change(order_id, **change))

    @server.tool()
    def modify_order(order_id: str, change: dict, account: str | None = None, replay: bool = False) -> dict:
        """Modify a paper order. Call preview_change with the same terms first."""
        return write(account, replay, lambda target: target.modify_order(order_id, **change))

    @server.tool()
    def note(trade_id: str, note: str = "", tags: list[str] | None = None, account: str | None = None, replay: bool = False) -> dict:
        """Save a trade journal note and tags."""
        return write(account, replay, lambda target: target.note(trade_id, note, tags))

    @server.tool()
    def day_note(day: str, plan: str = "", review: str = "", account: str | None = None, replay: bool = False) -> dict:
        """Save a New York trading day plan and review."""
        return write(account, replay, lambda target: target.day_note(day, plan, review))

    @server.tool()
    def create_alert(scope: str, metric: str, direction: str, level: str, terms: dict | None = None, account: str | None = None, replay: bool = False) -> dict:
        """Create a paper account, contract, spread or underlying alert."""
        return write(account, replay, lambda target: target.create_alert(scope, metric, direction, level, **(terms or {})))

    @server.tool()
    def delete_alert(alert_id: str, account: str | None = None, replay: bool = False) -> dict:
        """Remove an account alert."""
        return write(account, replay, lambda target: target.delete_alert(alert_id))

    @server.tool()
    def create_playbook(definition: dict, account: str | None = None, replay: bool = False) -> dict:
        """Create a versioned definition; requires admin scope and starts off."""
        return write(account, replay, lambda target: target.create_playbook(definition))

    @server.tool()
    def version_playbook(definition: dict, account: str | None = None, replay: bool = False) -> dict:
        """Save a new version using the current version as concurrency guard; admin scope."""
        return write(account, replay, lambda target: target.version_playbook(definition))

    @server.tool()
    def archive_playbook(playbook_id: str, version: int, account: str | None = None, replay: bool = False) -> dict:
        """Archive a playbook at its current version; admin scope."""
        return write(account, replay, lambda target: target.archive_playbook(playbook_id, version))

    @server.tool()
    def playbook_mode(playbook_id: str, mode: Literal["off", "stage", "auto"], account: str | None = None, replay: bool = False) -> dict:
        """Set mode; auto permits future paper orders. Requires admin scope. Prefer stage for review."""
        return write(account, replay, lambda target: target.playbook_mode(playbook_id, mode))

    @server.tool()
    def send_staged_order(stage_id: str, account: str | None = None, replay: bool = False) -> dict:
        """Send a reviewed stage through fresh engine risk checks. Read playbooks and preview its order first."""
        return write(account, replay, lambda target: target.send_staged_order(stage_id))

    @server.tool()
    def dismiss_staged_order(stage_id: str, account: str | None = None, replay: bool = False) -> dict:
        """Dismiss a staged playbook order."""
        return write(account, replay, lambda target: target.dismiss_staged_order(stage_id))

    @server.tool()
    def notifications() -> dict:
        """Nonsecret delivery status; no destinations or credentials."""
        return result(client, client.notifications)

    def batch(response):
        # Batch inputs have their own per-day provenance. The live daemon's
        # latest quote is not the market time or feed of a historical report.
        response["as_of"] = None
        response["feed"] = {"provider": None, "delayed": None, "delay_seconds": None}
        response["simulated"] = True
        return response

    @server.tool()
    def list_backtests(playbook: str | None = None) -> dict:
        """Saved simulated batch runs, optionally filtered by playbook ID/version."""
        return batch(result(client, lambda: client.list_backtests(playbook)))

    @server.tool()
    def get_backtest(run_id: str) -> dict:
        """Progress and saved report for a simulated batch."""
        return batch(result(client, lambda: client.get_backtest(run_id)))

    @server.tool()
    def compare_backtests(run_ids: list[str]) -> dict:
        """Compare 2–8 saved runs; mismatched input/plan warnings remain visible."""
        return batch(result(client, lambda: client.compare_backtests(run_ids)))

    @server.tool()
    def start_backtest(playbook: str | list[str], plan: str | dict, settings: dict | None = None) -> dict:
        """Start a simulated batch; requires replay scope. settings supplies days/scenarios, seed and workers."""
        return batch(write("main", False, lambda target: target.start_backtest(playbook, plan, **(settings or {}))))

    @server.tool()
    def cancel_backtest(run_id: str) -> dict:
        """Cancel a running batch; requires replay scope."""
        return batch(write("main", False, lambda target: target.cancel_backtest(run_id)))

    @server.tool()
    def start_sweep(playbook: str, plan: str | dict, parameter_grid: dict, settings: dict | None = None) -> dict:
        """Start 2–8 simulated variants over JSON-pointer playbook fields; replay scope required."""
        return batch(write("main", False, lambda target: target.start_sweep(playbook, plan, parameter_grid, **(settings or {}))))

    @server.tool()
    def list_sweeps() -> dict:
        """Saved simulated parameter sweeps."""
        return batch(result(client, client.list_sweeps))

    @server.tool()
    def get_sweep(run_id: str) -> dict:
        """Sweep progress, exact variant definitions and historical ranking; not a prediction."""
        return batch(result(client, lambda: client.get_sweep(run_id)))

    @server.tool()
    def cancel_sweep(run_id: str) -> dict:
        """Cancel an active parameter sweep; replay scope required."""
        return batch(write("main", False, lambda target: target.cancel_sweep(run_id)))

    @server.tool()
    def evaluate_playbook(playbook_id: str, account: str) -> dict:
        """Evaluate a live paper playbook now with the bearer token's trade scope (not a webhook
        secret, which this server never handles). Respects off/stage/auto; auto may trade. Never retries."""
        return write(account, False, lambda target: target.signal(playbook_id))

    return server
