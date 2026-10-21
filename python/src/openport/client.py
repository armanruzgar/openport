# Copyright (c) 2026 OpenPort contributors. MIT License.
from __future__ import annotations

import json
import time
import uuid
from urllib.error import HTTPError
from urllib.parse import quote, urlencode, urlsplit
from urllib.request import Request, build_opener, HTTPRedirectHandler

from .types import (JSON, Account, Chain, Fills, OrderResult, Orders, Portfolio,
                    ReplayListing, ReplayResult, RunVerification, Status, Summary, Surface, Trades, Settlements,
                    Exposure, Volatility, Candles, Risk, OrderPreview, SubmitResult,
                    Plans, Accounts, EquityHistory, CancelAllResult, FlattenResult,
                    WhatIfResult, FlattenPreview, StockPreview, RiskProfile, Probability)


def _list(values) -> str | None:
    return ",".join(str(value) for value in values) if values else None


class ApiError(Exception):
    """An HTTP failure, including the server's reason code and evidence."""

    def __init__(self, status: int, body: JSON | str):
        error = body.get("error", {}) if isinstance(body, dict) else {}
        if not isinstance(error, dict):
            error = {"message": str(error)}
        self.status = status
        self.code = error.get("code", "HTTP_ERROR")
        self.reason_code = self.code
        self.body = body
        self.actual = error.get("actual")
        self.limit = error.get("limit")
        self.scope = error.get("scope")
        super().__init__(f"{self.code}: {error.get('message', f'HTTP {status}')}")


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # Never forward credentials or repeat a paper command to another location.
        return None


class Client:
    def __init__(self, base_url: str = "http://127.0.0.1:8080", token: str | None = None,
                 account: str | None = None, timeout: float = 30, *, retries: int = 3,
                 backoff: float = 0.1, replay: bool = False, history: str | None = None):
        parts = urlsplit(base_url)
        if parts.scheme not in {"http", "https"} or not parts.netloc or parts.query or parts.fragment or parts.username:
            raise ValueError("base_url must be an HTTP(S) URL without credentials, query or fragment")
        if timeout <= 0 or retries < 0 or backoff < 0:
            raise ValueError("timeout must be positive; retries and backoff must be nonnegative")
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.account_id = account
        self.timeout = timeout
        self.retries = retries
        self.backoff = backoff
        self._prefix = "/api/replay/history/" + quote(history, safe="") if history else "/api/replay" if replay else "/api"
        self._opener = build_opener(_NoRedirect())

    def for_replay(self, account: str = "main") -> Client:
        return Client(self.base_url, self.token, account, self.timeout, retries=self.retries,
                      backoff=self.backoff, replay=True)

    def for_history(self, run_id: str, account: str = "main") -> Client:
        return Client(self.base_url, self.token, account, self.timeout, retries=self.retries,
                      backoff=self.backoff, history=run_id)

    def _request(self, method: str, path: str, body: JSON | None = None, *,
                 params: JSON | None = None, scoped: bool = False, control: bool = False, retry: bool = True):
        query = {key: value for key, value in (params or {}).items() if value is not None}
        if scoped and self.account_id is not None:
            query["account"] = self.account_id
        url = self.base_url + ("/api" if control else self._prefix) + path
        if query:
            url += "?" + urlencode(query)
        headers = {"Accept": "application/json"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        data = None if body is None else json.dumps(body, allow_nan=False).encode()
        if data is not None:
            headers["Content-Type"] = "application/json"
        request = Request(url, data=data, method=method, headers=headers)
        retries = self.retries if retry else 0
        for attempt in range(retries + 1):
            try:
                with self._opener.open(request, timeout=self.timeout) as response:
                    raw = response.read().decode()
                    return raw if response.headers.get_content_type() == "text/csv" else json.loads(raw)
            except HTTPError as error:
                with error:
                    raw = error.read().decode(errors="replace")
                    status = error.code
                    retry_after = error.headers.get("Retry-After") if error.headers else None
                if status == 503 and attempt < retries:
                    time.sleep(self._retry_delay(retry_after, attempt))
                    continue
                try:
                    payload = json.loads(raw)
                except ValueError:
                    payload = raw
                raise ApiError(status, payload) from None
        raise AssertionError("unreachable")

    def _retry_delay(self, retry_after: str | None, attempt: int) -> float:
        """The server's Retry-After seconds (at most 10) when it sends them, else exponential backoff."""
        try:
            seconds = float(retry_after) if retry_after is not None else -1.0
        except ValueError:
            seconds = -1.0
        if 0 <= seconds < float("inf"):
            return min(10.0, seconds)
        return min(2.0, self.backoff * 2 ** attempt)

    def status(self) -> Status:
        return self._request("GET", "/status")

    def reload_tokens(self) -> JSON:
        """Admin: reload named credentials; returns names and load metadata, never secrets."""
        return self._request("POST", "/tokens/reload", {}, control=True, retry=False)

    def symbols(self) -> list[str]:
        return [item["symbol"] for item in self.status()["underlyings"]]

    def _market(self, symbol: str, view: str, **params):
        return self._request("GET", "/underlyings/" + quote(symbol, safe="") + "/" + view, params=params)

    def summary(self, symbol: str) -> Summary:
        return self._market(symbol, "summary")

    def series(self, symbol: str) -> list[JSON]:
        """The expiry series in summary; openportd has no separate series route."""
        return self.summary(symbol)["expiries"]

    def chain(self, symbol: str, expiry: str | None = None, window: float = 0) -> Chain:
        return self._market(symbol, "chain", expiry=expiry, window=window)

    def exposure(self, symbol: str, expiries: int = 8, window: float = 0.08) -> Exposure:
        return self._market(symbol, "exposure", expiries=expiries, window=window)

    def surface(self, symbol: str, expiries: int = 12, window: float = 0.2,
                sabr_beta: float | str | None = None) -> Surface:
        """SVI and SSVI fits; sabr_beta (0 to 1, or "fit") adds the optional SABR overlay."""
        return self._market(symbol, "surface", expiries=expiries, window=window, sabr_beta=sabr_beta)

    def volatility(self, symbol: str) -> Volatility:
        return self._market(symbol, "volatility")

    def candles(self, symbol: str, interval: str = "5m", limit: int = 500) -> Candles:
        return self._market(symbol, "candles", interval=interval, limit=limit)

    def probability(self, symbol: str, days: list[float] | None = None,
                    prices: list[float] | None = None) -> Probability:
        """Probability cones and each price's odds of finishing beyond or touching it by each horizon."""
        return self._market(symbol, "probability", days=_list(days), prices=_list(prices))

    def account(self) -> Account:
        return self._request("GET", "/account", scoped=True)

    def portfolio(self) -> Portfolio:
        return self._request("GET", "/portfolio", scoped=True)

    def orders(self, status: str = "all", client_order_id: str | None = None) -> Orders:
        """Orders, newest first; client_order_id finds the order(s) that used that ID, one per attempt."""
        return self._request("GET", "/orders", scoped=True, params={"status": status, "client_order_id": client_order_id})

    def fills(self) -> Fills:
        return self._request("GET", "/fills", scoped=True)

    def settlements(self) -> Settlements:
        """Settlement references and provenance, newest first across attempts."""
        return self._request("GET", "/settlements", scoped=True)

    def trades(self, status: str = "all", attempt: str = "current") -> Trades:
        return self._request("GET", "/trades", scoped=True, params={"status": status, "attempt": attempt})

    def risk(self, dynamics: str | None = None) -> Risk:
        """Limits, Greeks and the spot x vol grid; dynamics ("sticky_moneyness" or "sticky_delta")
        recomputes the grid moving each smile along its SSVI surface instead of sticky strike."""
        return self._request("GET", "/risk", scoped=True, params={"dynamics": dynamics})

    def risk_profile(self, underlying: str | None = None, benchmark: str | None = None,
                     days: list[float | str] | None = None, iv: float | None = None,
                     range: float | None = None, steps: int | None = None,
                     betas: dict[str, float] | None = None, dynamics: str | None = None) -> RiskProfile:
        """The held book's P&L curves over dates, against one underlying or beta-weighted to SPY or SPX."""
        return self._request("GET", "/risk/profile", scoped=True, params={
            "underlying": underlying, "benchmark": benchmark, "days": _list(days), "iv": iv, "range": range,
            "steps": steps, "betas": ",".join(f"{symbol}:{beta}" for symbol, beta in betas.items()) if betas else None,
            "dynamics": dynamics})

    def equity(self, start: str | None = None, end: str | None = None, *,
               limit: int | None = None, cursor: str | None = None) -> EquityHistory:
        """Equity samples, optionally paged (1..2000); pass next as cursor with the same filters."""
        return self._request("GET", "/account/equity", scoped=True,
                             params={"from": start, "to": end, "limit": limit, "cursor": cursor})

    def plans(self) -> Plans:
        return self._request("GET", "/plans")

    def accounts(self, *, archived: bool = False) -> Accounts:
        return self._request("GET", "/accounts", params={"archived": "true"} if archived else None)

    @staticmethod
    def _order(order: JSON | None, fields: JSON) -> JSON:
        body = {**(order or {}), **fields}
        if not body.get("client_order_id"):
            body["client_order_id"] = str(uuid.uuid4())
        return body

    def place_order(self, order: JSON | None = None, **fields) -> SubmitResult:
        """Submit live/replay terms, including exto, gtc_exto or gtd with a zoned good_till timestamp."""
        return self._request("POST", "/orders", self._order(order, fields), scoped=True)

    def preview_order(self, order: JSON | None = None, **fields) -> OrderPreview:
        return self._request("POST", "/orders/preview", self._order(order, fields), scoped=True)

    def preview_change(self, order_id: str, **change) -> OrderPreview:
        """Preview a change to a resting order (quantity, limit_price, trigger_level, floor_share) without making it."""
        return self._request("POST", "/orders/" + quote(str(order_id), safe="") + "/preview", change, scoped=True)

    def what_if(self, candidates: list[JSON]) -> WhatIfResult:
        """Compare candidate adjustments, each {"name", "orders": [order, ...]}, against the held book without trading."""
        return self._request("POST", "/orders/what-if", {"candidates": candidates}, scoped=True)

    def modify_order(self, order_id: str, **change) -> SubmitResult:
        return self._request("PUT", "/orders/" + quote(str(order_id), safe=""), change, scoped=True)

    def cancel_order(self, order_id: str) -> OrderResult:
        return self._request("DELETE", "/orders/" + quote(str(order_id), safe=""), scoped=True)

    def cancel_all(self, underlying: str | None = None) -> CancelAllResult:
        return self._request("POST", "/orders/cancel", {"underlying": underlying} if underlying else {}, scoped=True)

    def cancel_orders(self, order_ids: list[str]) -> CancelAllResult:
        """Cancel the listed orders together in one transaction, such as both exits of a pair."""
        return self._request("POST", "/orders/cancel", {"orders": [str(i) for i in order_ids]}, scoped=True)

    @staticmethod
    def _flatten_body(underlying, type, limit_ticks):
        if type not in ("market", "limit"):
            raise ValueError("type must be market or limit")
        if limit_ticks is not None and (type != "limit" or isinstance(limit_ticks, bool)
                                       or not isinstance(limit_ticks, int) or not 0 <= limit_ticks <= 10):
            raise ValueError("limit_ticks requires type limit and an integer from 0 to 10")
        body = {"underlying": underlying} if underlying else {}
        if type == "limit":
            body["type"] = type
        if limit_ticks is not None:
            body["limit_ticks"] = limit_ticks
        return body

    def flatten(self, underlying: str | None = None, *, type: str = "market", limit_ticks: int | None = None) -> FlattenResult:
        """Reduce-only closes; limit follows each touch by 0-10 ticks and works extended sessions."""
        return self._request("POST", "/positions/close", self._flatten_body(underlying, type, limit_ticks), scoped=True)

    def preview_flatten(self, underlying: str | None = None, *, type: str = "market", limit_ticks: int | None = None) -> FlattenPreview:
        """What flatten would cancel and close now, and the account after it, without doing it."""
        return self._request("POST", "/positions/close/preview", self._flatten_body(underlying, type, limit_ticks), scoped=True)

    def note(self, trade_id: str, note: str = "", tags: list[str] | None = None) -> JSON:
        return self._request("PUT", "/trades/" + quote(str(trade_id), safe="") + "/note",
                             {"note": note, "tags": tags or []}, scoped=True)

    def group_trades(self, trade_ids: list[str]) -> JSON:
        """Join the open round trips' trades into one whole trade."""
        return self._request("POST", "/trades/group", {"trades": [str(t) for t in trade_ids]}, scoped=True)

    def ungroup_trades(self, trade_ids: list[str]) -> JSON:
        """Take each open round trip out of its whole trade."""
        return self._request("POST", "/trades/ungroup", {"trades": [str(t) for t in trade_ids]}, scoped=True)

    def alerts(self) -> JSON:
        return self._request("GET", "/alerts", scoped=True)

    def create_alert(self, scope: str, metric: str, direction: str, level: str, **terms) -> JSON:
        """terms: label, symbol, legs, repeat. See POST /api/alerts."""
        return self._request("POST", "/alerts", {"scope": scope, "metric": metric, "direction": direction,
                                                 "level": level, **terms}, scoped=True)

    def delete_alert(self, alert_id: str) -> JSON:
        return self._request("DELETE", "/alerts/" + quote(str(alert_id), safe=""), scoped=True)

    def day_note(self, day: str, plan: str = "", review: str = "") -> JSON:
        return self._request("PUT", "/days/" + quote(day, safe="") + "/note",
                             {"plan": plan, "review": review}, scoped=True)

    def export_csv(self, kind: str, **params) -> str:
        if kind not in {"fills", "trades"}:
            raise ValueError("kind must be fills or trades")
        return self._request("GET", "/" + kind + ".csv", params=params, scoped=True)

    def create_account(self, name: str, **settings) -> JSON:
        return self._request("POST", "/accounts", {"name": name, **settings})

    def rename_account(self, account_id: str, name: str) -> JSON:
        return self._request("PATCH", "/accounts/" + quote(account_id, safe=""), {"name": name})

    def archive_account(self, account_id: str, archived: bool = True) -> JSON:
        return self._request("PATCH", "/accounts/" + quote(account_id, safe=""), {"archived": archived})

    def delete_account(self, account_id: str) -> JSON:
        return self._request("DELETE", "/accounts/" + quote(account_id, safe=""))

    def reset_account(self, reason: str, **settings) -> Account:
        return self._request("POST", "/account/reset", {"reason": reason, **settings}, scoped=True)

    def payout(self, amount: str) -> Account:
        return self._request("POST", "/account/payout", {"amount": amount}, scoped=True)

    def limits(self, expected_revision: str, limits: JSON) -> JSON:
        return self._request("PUT", "/risk/limits", {"expected_revision": expected_revision, "limits": limits}, scoped=True)

    def guardrails(self, expected_revision: str, guardrails: JSON) -> JSON:
        return self._request("PUT", "/risk/guardrails", {"expected_revision": expected_revision, "guardrails": guardrails}, scoped=True)

    def kill(self, action: str, reason: str) -> JSON:
        return self._request("POST", "/risk/kill", {"action": action, "reason": reason}, scoped=True)

    def settle(self, symbol: str, value: str) -> JSON:
        return self._request("POST", "/settlements", {"symbol": symbol, "value": value}, scoped=True)

    def exercise(self, symbol: str, quantity: int) -> Portfolio:
        return self._request("POST", "/positions/exercise", {"symbol": symbol, "quantity": quantity}, scoped=True)

    def abandon(self, symbol: str) -> Portfolio:
        return self._request("POST", "/positions/abandon", {"symbol": symbol}, scoped=True)

    def exercise_instruction(self, symbol: str, do_not_exercise: bool = True) -> Portfolio:
        return self._request("POST", "/positions/instruction", {"symbol": symbol, "do_not_exercise": do_not_exercise}, scoped=True)

    def trade_stock(self, symbol: str, side: str, shares: int) -> Portfolio:
        """Open, add, reduce or reverse shares at a fresh stock-session price."""
        return self._request("POST", "/stocks/trade", {"symbol": symbol, "side": side, "shares": shares}, scoped=True, retry=False)

    def preview_stock(self, symbol: str, side: str, shares: int) -> StockPreview:
        """Preview share cost, buying power and dollar delta without trading."""
        return self._request("POST", "/stocks/trade/preview", {"symbol": symbol, "side": side, "shares": shares}, scoped=True)

    def close_stock(self, symbol: str, shares: int | None = None) -> Portfolio:
        return self._request("POST", "/stocks/close", {"symbol": symbol, **({"shares": shares} if shares is not None else {})}, scoped=True)

    def list_backtests(self) -> JSON:
        return self._request("GET", "/backtests", control=True)

    def start_backtest(self, playbook: str | list[str], plan: str | JSON, **settings) -> JSON:
        """Pin one selector, or 2–8 distinct IDs for a joint account (ID order)."""
        selection = {"playbooks" if isinstance(playbook, list) else "playbook": playbook}
        return self._request("POST", "/backtests", {**selection, "plan": plan, **settings}, control=True)

    def get_backtest(self, run_id: str) -> JSON:
        return self._request("GET", "/backtests/" + quote(run_id, safe=""), control=True)

    def compare_backtests(self, run_ids: list[str]) -> JSON:
        return self._request("GET", "/backtests/compare", params={"ids": ",".join(run_ids)}, control=True)

    def keep_backtest(self, run_id: str, keep: bool = True) -> JSON:
        return self._request("PUT", "/backtests/" + quote(run_id, safe=""), {"keep": keep}, control=True)

    def delete_backtest(self, run_id: str) -> JSON:
        return self._request("DELETE", "/backtests/" + quote(run_id, safe=""), params={"purge": "true"}, control=True)

    def cancel_backtest(self, run_id: str) -> JSON:
        return self._request("DELETE", "/backtests/" + quote(run_id, safe=""), control=True)

    def list_replays(self) -> ReplayListing:
        return self._request("GET", "/replay", control=True)

    def start_replay(self, **settings) -> ReplayResult:
        return self._request("POST", "/replay", settings, control=True)

    def resume_replay(self, run_id: str, **settings) -> ReplayResult:
        """Continue a saved run a crash interrupted; settings may give speed and paused."""
        return self._request("POST", "/replay", {"resume": run_id, **settings}, control=True)

    def restart_replay(self, run_id: str, *, at: str | None = None, **settings) -> ReplayResult:
        """New run with source commands through at inclusive; defaults to its start, paused."""
        return self._request("POST", "/replay", {"restart": run_id, **({"at": at} if at is not None else {}), **settings}, control=True)

    def control_replay(self, **settings) -> ReplayResult:
        return self._request("PUT", "/replay", settings, control=True)

    def step_replay(self, until: str) -> ReplayResult:
        return self.control_replay(until=until)

    def play_replay_until(self, until: str, *, speed: int | None = None) -> ReplayResult:
        """Play at the current/selected speed, then pause through the target; returns now."""
        return self.control_replay(play_until=until, **({"speed": speed} if speed is not None else {}))

    def abort_replay(self) -> ReplayResult:
        """Pause at the next settled batch, interrupting a running step."""
        return self.control_replay(abort=True)

    def stop_replay(self) -> ReplayResult:
        return self._request("DELETE", "/replay", control=True)

    def verify_replay(self, run_id: str) -> RunVerification:
        """Start background verification of a finished run (replay write scope)."""
        return self._request("POST", "/replay/history/" + quote(run_id, safe="") + "/verify", {}, control=True)

    def replay_verification(self, run_id: str, *, receipt: bool = False) -> RunVerification:
        """Read verification state, or its shareable final receipt."""
        return self._request("GET", "/replay/history/" + quote(run_id, safe="") + "/verify",
                             params={"format": "receipt"} if receipt else None, control=True)

    def delete_replay(self, run_id: str) -> JSON:
        return self._request("DELETE", "/replay/history/" + quote(run_id, safe=""), control=True)
