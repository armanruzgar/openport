# Copyright (c) 2026 OpenPort contributors. MIT License.
"""MCP 2.x tools for openport's paper simulator."""
from __future__ import annotations

from dataclasses import asdict, dataclass
import re
from typing import Literal

from mcp.server.mcpserver import MCPServer
from openport import ApiError, Client


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
        "Writes require OPENPORT_WRITE_TOKEN and an account, except sandbox tokens default to their own account. Preview before placing an order. "
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
    def volatility(symbol: str, replay: bool = False) -> dict:
        """Model-free IV, skew and realized volatility; missing values remain null."""
        target = selected(replay=replay)
        return result(target, lambda: target.volatility(symbol))

    @server.tool()
    def probability(symbol: str, days: list[float] | None = None, prices: list[float] | None = None,
                    replay: bool = False) -> dict:
        """Implied probability cones by horizon (days; 0 is today's close) and odds of finishing beyond or touching prices."""
        target = selected(replay=replay)
        return result(target, lambda: target.probability(symbol, days, prices))

    @server.tool()
    def exposure(symbol: str, replay: bool = False) -> dict:
        """Gamma and vanna exposure in the default display window."""
        target = selected(replay=replay)
        return result(target, lambda: target.exposure(symbol))

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
                     days: list[float | str] | None = None, iv: float | None = None, replay: bool = False) -> dict:
        """The held book's P&L curves over dates (days, or "expiry") and an IV offset, against one underlying
        or beta-weighted to SPY or SPX, with the levels where each date reaches the floors."""
        target = selected(account, replay)
        return result(target, lambda: target.risk_profile(underlying, benchmark, days, iv))

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

    return server
