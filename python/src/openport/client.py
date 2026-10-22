# Copyright (c) 2026 OpenPort contributors. MIT License.
from __future__ import annotations

import json
import hashlib
import hmac
import time
import uuid
from urllib.error import HTTPError
from urllib.parse import quote, urlencode, urlsplit
from urllib.request import Request, build_opener, HTTPRedirectHandler

from .types import (JSON, Account, Chain, Fills, OrderResult, Orders, Portfolio,
                    ReplayListing, ReplayResult, RunVerification, Status, Summary, Surface, Trades, Settlements,
                    Exposure, ExposureConvention, ExposureSlice, Volatility, Candles, Series, Risk, OrderPreview, SubmitResult,
                    Plans, Accounts, EquityHistory, CancelAllResult, FlattenResult,
                    WhatIfResult, FlattenPreview, StockPreview, RiskProfile, Probability, HistoricalRisk, ScenarioAnalysis, Heston,
                    StrategySearch, HedgeSuggestions, FillQuality, Calibration)


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


def sign_signal(secret: str, timestamp: str | int, method: str, target: str, body: bytes) -> str:
    """Hex HMAC-SHA256 for X-OpenPort-Signature: TIMESTAMP, METHOD, exact target and raw body, newline-joined."""
    message = f"{timestamp}\n{method}\n{target}\n".encode() + body
    return hmac.new(secret.encode(), message, hashlib.sha256).hexdigest()


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
                 params: JSON | None = None, scoped: bool = False, control: bool = False, retry: bool = True,
                 signal_secret: str | None = None, timestamp: int | None = None):
        query = {key: value for key, value in (params or {}).items() if value is not None}
        if scoped and self.account_id is not None:
            query["account"] = self.account_id
        url = self.base_url + ("/api" if control else self._prefix) + path
        if query:
            url += "?" + urlencode(query)
        headers = {"Accept": "application/json"}
        # Webhook signals carry only their own secret; the server refuses bearer tokens there.
        if self.token and signal_secret is None:
            headers["Authorization"] = "Bearer " + self.token
        data = None if body is None else json.dumps(body, allow_nan=False).encode()
        if data is not None:
            headers["Content-Type"] = "application/json"
        if signal_secret is not None:
            parts = urlsplit(url)
            stamp = str(int(time.time()) if timestamp is None else timestamp)
            headers["X-OpenPort-Timestamp"] = stamp
            headers["X-OpenPort-Signature"] = "sha256=" + sign_signal(
                signal_secret, stamp, method, parts.path + ("?" + parts.query if parts.query else ""), data or b"")
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

    def series(self, symbol: str, fields: list[str] | None = None, interval: str | None = None,
               start: int | str | None = None, end: int | str | None = None) -> Series:
        """Metric history such as mfiv30, atm30 and rr25 at 1m or 1d, oldest first. start and end
        are Unix seconds, ISO timestamps or New York dates; the server defaults to every field,
        daily rows and the last 366 days. An underlying's expiries are in summary()."""
        return self._market(symbol, "series", fields=_list(fields), interval=interval, **{"from": start, "to": end})

    def chain(self, symbol: str, expiry: str | None = None, window: float = 0) -> Chain:
        return self._market(symbol, "chain", expiry=expiry, window=window)

    def exposure(self, symbol: str, expiries: int = 8, window: float = 0.08, *,
                 convention: ExposureConvention = "long_calls_short_puts", slice: ExposureSlice = "all") -> Exposure:
        """OI-weighted GEX/VEX/DEX/CEX/VOMEX; customer flow is unavailable without signed data."""
        return self._market(symbol, "exposure", expiries=expiries, window=window,
                            convention=convention, slice=slice)

    def surface(self, symbol: str, expiries: int = 12, window: float = 0.2,
                sabr_beta: float | str | None = None) -> Surface:
        """SVI and SSVI fits; sabr_beta (0 to 1 in steps of 0.1, or "fit") adds the optional SABR overlay."""
        return self._market(symbol, "surface", expiries=expiries, window=window, sabr_beta=sabr_beta)

    def heston(self, symbol: str, expiries: int | None = None) -> Heston:
        """Research only: one Heston fit over the leading expiries, beside their SVI and SSVI RMSE."""
        return self._market(symbol, "heston", expiries=expiries)

    def volatility(self, symbol: str) -> Volatility:
        return self._request("GET", "/underlyings/" + quote(symbol, safe="") + "/volatility", scoped=True)

    def volatility_history(self, symbol: str, *, fields: list[str] | None = None,
                           interval: str = "1d", from_: str | None = None, to: str | None = None) -> JSON:
        """Stored observations; signed skew ranks are returned by volatility()."""
        return self._market(symbol, "series", fields=_list(fields), interval=interval, **{"from": from_, "to": to})

    def surface_history(self, symbol: str, *, compare: bool = True,
                        from_: str | None = None, to: str | None = None) -> JSON:
        """Recorded SVI parameters. By default only the prior-close, week and month
        comparisons; compare=False lists the range too, at most ``limit`` newest
        snapshots with ``truncated`` set when older ones were left out."""
        return self._market(symbol, "surface-history", compare="true" if compare else "false", **{"from": from_, "to": to})

    def volatility_scanner(self) -> JSON:
        """One batch of volatility metrics for every subscribed underlying."""
        return self._request("GET", "/volatility/scanner")

    def candles(self, symbol: str, interval: str = "5m", limit: int = 500) -> Candles:
        return self._market(symbol, "candles", interval=interval, limit=limit)

    def probability(self, symbol: str, days: list[float] | None = None,
                    prices: list[float] | None = None) -> Probability:
        """Probability cones and each price's odds of finishing beyond or touching it by each horizon."""
        return self._market(symbol, "probability", days=_list(days), prices=_list(prices))

    def flow(self, symbol: str, limit: int | None = None, type: str | None = None, side: list[str] | None = None,
             expiry: str | None = None, min_size: float | None = None, min_premium: float | None = None,
             only: str | None = None) -> JSON:
        """Time and sales with net premium by expiry and strike; source "none" means the feed has no trades."""
        return self._market(symbol, "flow", limit=limit, type=type, side=_list(side), expiry=expiry,
                            min_size=min_size, min_premium=min_premium, only=only)

    def earnings(self, symbol: str | None = None) -> JSON:
        """Earnings implied versus realized moves, recorded here or imported, with the over-priced rate."""
        return self._request("GET", "/earnings", params={"symbol": symbol}, control=True)

    def import_earnings(self, csv: str) -> JSON:
        """Admin: seed past moves from symbol,date,session,implied_move,realized_move[,label] (percent)."""
        return self._request("POST", "/earnings/import", {"csv": csv}, control=True, retry=False)

    def delete_imported_earnings(self) -> JSON:
        """Admin: remove every imported earnings move; recorded moves stay."""
        return self._request("DELETE", "/earnings/imported", control=True, retry=False)

    def analysis_portfolios(self) -> JSON:
        """Admin, reads included: imported outside portfolios."""
        return self._request("GET", "/analysis/portfolios", control=True)

    def import_portfolio(self, name: str, csv: str, format: str = "auto") -> JSON:
        """Admin: an analysis-only portfolio from a positions CSV. It is never traded."""
        return self._request("POST", "/analysis/portfolios", {"name": name, "csv": csv, "format": format},
                             control=True, retry=False)

    def analysis_portfolio(self, portfolio: str) -> JSON:
        """Admin: one analysis portfolio's holdings, Greeks and risk views."""
        return self._request("GET", "/analysis/portfolios/" + quote(portfolio, safe=""), control=True)

    def analysis_risk(self, portfolio: str, view: str = "profile", **params) -> JSON:
        """Admin: a risk view of an analysis portfolio: profile, grid, stress or var (the portfolio's views lists them)."""
        return self._request("GET", "/analysis/portfolios/" + quote(portfolio, safe="") + "/risk/" + quote(view, safe=""),
                             params=params, control=True)

    def delete_analysis_portfolio(self, portfolio: str) -> JSON:
        return self._request("DELETE", "/analysis/portfolios/" + quote(portfolio, safe=""), control=True, retry=False)

    def optimize_strategies(self, symbol: str, target: float, budget: float, *, date: str | None = None,
                            expiries: list[str] | None = None, structures: list[str] | None = None,
                            rank: str | None = None, vol_points: float | None = None, price: str | None = None,
                            limit: int | None = None, max_candidates: int | None = None) -> StrategySearch:
        """Search listed strategies within a loss budget for a target price and date, ranked by expected P&L under
        the user's shifted distribution (rank="expected"), the risk-neutral one, probability of profit or return on risk.
        Derived estimates from current quotes; a busy search pool answers 503 SEARCH_BUSY, retried like other 503s."""
        return self._market(symbol, "optimizer", target=target, budget=budget, date=date, expiry_ids=_list(expiries),
                            structures=_list(structures), rank=rank, vol_points=vol_points, price=price,
                            limit=limit, max_candidates=max_candidates)

    def account(self) -> Account:
        return self._request("GET", "/account", scoped=True)

    def portfolio(self) -> Portfolio:
        return self._request("GET", "/portfolio", scoped=True)

    def orders(self, status: str = "all", client_order_id: str | None = None) -> Orders:
        """Orders, newest first; client_order_id finds the order(s) that used that ID, one per attempt."""
        return self._request("GET", "/orders", scoped=True, params={"status": status, "client_order_id": client_order_id})

    def get_order(self, order_id: str) -> OrderResult:
        return self._request("GET", "/orders/" + quote(str(order_id), safe=""), scoped=True)

    def fills(self) -> Fills:
        return self._request("GET", "/fills", scoped=True)

    def fill_quality(self, attempt: str = "current") -> FillQuality:
        """Paper fills by place on their book (mid, mid plus a tick, natural) by time of day, spread and DTE."""
        return self._request("GET", "/fills/quality", scoped=True, params={"attempt": attempt})

    def calibrate_fills(self, csv: str, format: str = "auto", max_quote_age_seconds: int | None = None) -> Calibration:
        """Compare real broker fills (a CSV export's text) with the paper fill models. Read-only: nothing is stored."""
        body: JSON = {"csv": csv, "format": format}
        if max_quote_age_seconds is not None:
            body["max_quote_age_seconds"] = max_quote_age_seconds
        return self._request("POST", "/fills/calibration", body, control=True)

    def settlements(self) -> Settlements:
        """Settlement references and provenance, newest first across attempts."""
        return self._request("GET", "/settlements", scoped=True)

    def trades(self, status: str = "all", attempt: str = "current") -> Trades:
        return self._request("GET", "/trades", scoped=True, params={"status": status, "attempt": attempt})

    def risk(self, dynamics: str | None = None) -> Risk:
        """Limits, Greeks and the spot x vol grid; dynamics ("sticky_moneyness" or "sticky_delta")
        recomputes the grid moving each smile along its SSVI surface instead of sticky strike."""
        return self._request("GET", "/risk", scoped=True, params={"dynamics": dynamics})

    def risk_history(self) -> HistoricalRisk:
        """Derived historical stress and one-day VaR/ES; incomplete inputs remain unavailable."""
        return self._request("GET", "/risk/history", scoped=True)

    def risk_scenarios(self, mode: str = "parallel", days: list[float] | None = None,
                       benchmark: str | None = None, vols: list[float] | None = None,
                       term_scaled: bool = False, betas: dict[str, float] | None = None,
                       dynamics: str | None = None) -> ScenarioAnalysis:
        """Read-only spot × vol × time grid; does not change trading-rule scenarios.
        dynamics (sticky_strike, sticky_moneyness, sticky_delta) applies in every mode."""
        return self._request("GET", "/risk/scenarios", scoped=True, params={
            "mode": mode, "days": _list(days), "benchmark": benchmark, "vols": _list(vols),
            "term_scaled": "true" if term_scaled else "false",
            "betas": ",".join(f"{symbol}:{beta}" for symbol, beta in betas.items()) if betas else None,
            "dynamics": dynamics})

    def risk_profile(self, underlying: str | None = None, benchmark: str | None = None,
                     days: list[float | str] | None = None, iv: float | None = None,
                     range: float | None = None, steps: int | None = None,
                     betas: dict[str, float] | None = None, term_scaled: bool = False,
                     dynamics: str | None = None, legs: dict[str, int] | None = None, held: bool = True) -> RiskProfile:
        """The held book's P&L curves over dates, against one underlying or beta-weighted to SPY or SPX.
        legs adds hypothetical contracts {padded OSI: signed quantity} (with underlying); held=False values them alone."""
        return self._request("GET", "/risk/profile", scoped=True, params={
            "underlying": underlying, "benchmark": benchmark, "days": _list(days), "iv": iv, "range": range,
            "term_scaled": "true" if term_scaled else None, "steps": steps,
            "betas": ",".join(f"{symbol}:{beta}" for symbol, beta in betas.items()) if betas else None,
            "dynamics": dynamics,
            "legs": ",".join(f"{symbol}:{quantity}" for symbol, quantity in legs.items()) if legs else None,
            "held": None if held else "false"})

    def hedges(self, target: str, scope: str = "aggregate", utilisation: float | None = None,
               betas: dict[str, float] | None = None) -> HedgeSuggestions:
        """The smallest share, ATM option or SPX/SPY beta hedge bringing a bucket's delta, vega or worst grid cell
        (target) inside its limit or above the floor, ranked by spread cost then residual risk. For a delta target,
        beta hedges are sized on the beta-weighted dollar delta to SPY; betas overrides each underlying's beta.
        Nothing is traded."""
        return self._request("GET", "/risk/hedges", scoped=True, params={
            "target": target, "scope": scope, "utilisation": utilisation,
            "betas": ",".join(f"{symbol}:{beta}" for symbol, beta in betas.items()) if betas else None})

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

    def note(self, trade_id: str, note: str = "", tags: list[str] | None = None, *, mistakes: list[str] | None = None) -> JSON:
        return self._request("PUT", "/trades/" + quote(str(trade_id), safe="") + "/note",
                             {"note": note, "tags": tags or [], **({"mistakes": mistakes} if mistakes is not None else {})}, scoped=True)

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

    def account_groups(self) -> JSON:
        """Named groups of live accounts that copy_order mirrors one order to."""
        return self._request("GET", "/account-groups", control=True)

    def put_account_group(self, name: str, allocation: str, members: list[JSON]) -> JSON:
        """Create or replace a group. allocation is "ratio" (members carry a decimal "ratio") or "equity"."""
        return self._request("PUT", "/account-groups/" + quote(name, safe=""),
                             {"allocation": allocation, "members": members}, control=True)

    def delete_account_group(self, name: str) -> JSON:
        return self._request("DELETE", "/account-groups/" + quote(name, safe=""), control=True)

    def copy_order(self, group: str, order: JSON | None = None, **fields) -> JSON:
        """Submit one order to every account of a group, each as its own submission; see results per account."""
        return self._request("POST", "/orders/copy", {"group": group, "order": self._order(order, fields)}, control=True)

    def preview_copy_order(self, group: str, order: JSON | None = None, **fields) -> JSON:
        return self._request("POST", "/orders/copy/preview", {"group": group, "order": self._order(order, fields)}, control=True)

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

    def transfer_cash(self, client_id: str, amount: str) -> Account:
        """Practice cash only: positive deposit or negative withdrawal; reuse client_id on retry."""
        return self._request("POST", "/account/cash-flows", {"client_id": client_id, "amount": amount}, scoped=True)

    def halt_underlying(self, symbol: str, minutes: int) -> Account:
        """Replay only: simulate a stock/ETF pause at replay time, capped at the regular close."""
        return self._request("POST", "/market/halt", {"symbol": symbol, "minutes": minutes}, scoped=True)

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

    def stock_orders(self, status: str = "all") -> JSON:
        """Share orders, newest first; status="open" for the working ones."""
        return self._request("GET", "/stocks/orders", params={"status": status}, scoped=True)

    def place_stock_order(self, order: JSON | None = None, **fields) -> JSON:
        """A working share order: type market, limit, stop or stop_limit; time_in_force day or gtc;
        optional extended_hours (limits), bracket {"stop_loss", "take_profit"} and oco partner.
        Retries with the same client_order_id and terms are answered as first accepted."""
        return self._request("POST", "/stocks/orders", self._order(order, fields), scoped=True)

    def preview_stock_order(self, order: JSON | None = None, **fields) -> StockPreview:
        return self._request("POST", "/stocks/orders/preview", self._order(order, fields), scoped=True)

    def cancel_stock_order(self, order_id: str) -> JSON:
        return self._request("DELETE", "/stocks/orders/" + quote(str(order_id), safe=""), scoped=True)

    def close_stock(self, symbol: str, shares: int | None = None) -> Portfolio:
        return self._request("POST", "/stocks/close", {"symbol": symbol, **({"shares": shares} if shares is not None else {})}, scoped=True)

    def list_backtests(self, playbook: str | None = None) -> JSON:
        return self._request("GET", "/backtests", params={"playbook": playbook}, control=True)

    def start_backtest(self, playbook: str | list[str], plan: str | JSON, **settings) -> JSON:
        """Pin one selector, or 2–8 distinct IDs for a joint account (ID order)."""
        selection = {"playbooks" if isinstance(playbook, list) else "playbook": playbook}
        return self._request("POST", "/backtests", {**selection, "plan": plan, **settings}, control=True)

    def get_backtest(self, run_id: str) -> JSON:
        return self._request("GET", "/backtests/" + quote(run_id, safe=""), control=True)

    def compare_backtests(self, run_ids: list[str]) -> JSON:
        return self._request("GET", "/backtests/compare", params={"ids": ",".join(run_ids)}, control=True)

    def start_sweep(self, playbook: str, plan: str | JSON, parameter_grid: JSON, **settings) -> JSON:
        """2–8 variants over JSON-pointer playbook fields; inputs stay fixed across variants."""
        return self._request("POST", "/backtests/sweeps", {"playbook": playbook, "plan": plan,
                             "parameter_grid": parameter_grid, **settings}, control=True, retry=False)

    def list_sweeps(self) -> JSON:
        return self._request("GET", "/backtests/sweeps", control=True)

    def get_sweep(self, run_id: str) -> JSON:
        return self._request("GET", "/backtests/sweeps/" + quote(run_id, safe=""), control=True)

    def cancel_sweep(self, run_id: str) -> JSON:
        return self._request("DELETE", "/backtests/sweeps/" + quote(run_id, safe=""), control=True)

    def purge_sweep(self, run_id: str) -> JSON:
        """Permanently remove a finished sweep and its variant files."""
        return self._request("DELETE", "/backtests/sweeps/" + quote(run_id, safe=""), params={"purge": "true"}, control=True)

    def signal(self, playbook_id: str) -> JSON:
        """Evaluate a live paper playbook now with this bearer token (trade scope). Never automatically retried."""
        if self._prefix != "/api":
            raise ValueError("Signals address live paper accounts; use replay controls for replay")
        return self._request("POST", "/playbooks/" + quote(playbook_id, safe="") + "/evaluate", {}, scoped=True, retry=False)

    def send_signal(self, playbook_id: str, secret: str, *, nonce: str | None = None,
                    timestamp: int | None = None) -> JSON:
        """Send a webhook signal signed with the playbook's signal secret, without the bearer token.

        The secret authorises only this playbook on this client's account. Never automatically retried."""
        if self._prefix != "/api":
            raise ValueError("Signals address live paper accounts; use replay controls for replay")
        return self._request("POST", "/signals/" + quote(playbook_id, safe=""),
                             {"action": "evaluate", "nonce": str(uuid.uuid4()) if nonce is None else nonce},
                             scoped=True, retry=False, signal_secret=secret, timestamp=timestamp)

    def issue_signal_secret(self, playbook_id: str) -> JSON:
        """Admin: create or rotate the webhook secret for this playbook and account. Returned only once."""
        return self._request("POST", "/playbooks/" + quote(playbook_id, safe="") + "/signal-secret", {},
                             scoped=True, control=True, retry=False)

    def signal_secret(self, playbook_id: str) -> JSON:
        """Admin: whether a webhook secret exists and when it was issued; never the secret."""
        return self._request("GET", "/playbooks/" + quote(playbook_id, safe="") + "/signal-secret", scoped=True, control=True)

    def revoke_signal_secret(self, playbook_id: str) -> JSON:
        """Admin: revoke the webhook secret for this playbook and account."""
        return self._request("DELETE", "/playbooks/" + quote(playbook_id, safe="") + "/signal-secret", scoped=True, control=True)

    def keep_backtest(self, run_id: str, keep: bool = True) -> JSON:
        return self._request("PUT", "/backtests/" + quote(run_id, safe=""), {"keep": keep}, control=True)

    def delete_backtest(self, run_id: str) -> JSON:
        return self._request("DELETE", "/backtests/" + quote(run_id, safe=""), params={"purge": "true"}, control=True)

    def cancel_backtest(self, run_id: str | None = None) -> JSON:
        return self._request("DELETE", "/backtests" + ("/" + quote(run_id, safe="") if run_id else ""), control=True)

    def playbooks(self) -> JSON:
        """Definitions, account modes, staged orders and forward reports."""
        return self._request("GET", "/playbooks", scoped=True)

    def get_playbook(self, playbook_id: str, version: int | None = None) -> JSON:
        return self._request("GET", "/playbooks/" + quote(playbook_id, safe=""),
                             params={"version": version}, scoped=True)

    def create_playbook(self, definition: JSON) -> JSON:
        return self._request("POST", "/playbooks", definition, scoped=True, retry=False)

    def version_playbook(self, definition: JSON) -> JSON:
        """Save the next version; definition includes its ID and expected current version."""
        return self._request("PUT", "/playbooks/" + quote(definition["id"], safe=""),
                             definition, scoped=True, retry=False)

    def archive_playbook(self, playbook_id: str, version: int) -> JSON:
        return self._request("DELETE", "/playbooks/" + quote(playbook_id, safe=""),
                             params={"version": version}, scoped=True, retry=False)

    def playbook_mode(self, playbook_id: str, mode: str) -> JSON:
        return self._request("PUT", "/playbooks/" + quote(playbook_id, safe="") + "/mode",
                             {"mode": mode}, scoped=True, retry=False)

    def send_staged_order(self, stage_id: str) -> JSON:
        """Send the reviewed stage through the engine's current risk checks."""
        return self._request("POST", "/playbooks/staged/" + quote(stage_id, safe="") + "/send",
                             {}, scoped=True, retry=False)

    def dismiss_staged_order(self, stage_id: str) -> JSON:
        return self._request("POST", "/playbooks/staged/" + quote(stage_id, safe="") + "/dismiss",
                             {}, scoped=True, retry=False)

    def notifications(self) -> JSON:
        """Nonsecret notification status is part of status, with no separate GET route."""
        return self.status()["notifications"]

    def test_notification(self, channel: str) -> JSON:
        return self._request("POST", "/notifications/test", {"channel": channel}, control=True, retry=False)

    def configure_notification(self, channel: str, **settings) -> JSON:
        return self._request("PUT", "/notifications/channels/" + quote(channel, safe=""),
                             settings, control=True)

    def create_sandbox(self) -> JSON:
        """Create an isolated demo account; the returned token is a secret."""
        return self._request("POST", "/sandboxes", {}, control=True, retry=False)

    def calendar_events(self, start: str | None = None, end: str | None = None) -> JSON:
        return self._request("GET", "/calendar/events", params={"from": start, "to": end}, control=True)

    def pass_odds(self, days: int = 20, samples: int = 1000, *,
                  playbook: str | None = None, seed: str | None = None, economics: bool = False,
                  payout_odds: float | None = None, payout: str | None = None) -> JSON:
        """Derived historical estimate, not a prediction; insufficient history is 422.

        economics=True adds challenge economics (expected value and break-even pass rate)
        for the account's program; payout_odds and payout replace the simulated first payout.
        """
        return self._request("GET", "/account/pass-odds", scoped=True,
                             params={"days": days, "samples": samples, "playbook": playbook, "seed": seed,
                                     "economics": "true" if economics else None,
                                     "payout_odds": payout_odds, "payout": payout})

    def rules_compare(self, plans: list[str] | None = None, rules: list[JSON] | None = None, *,
                      include_current: bool = True) -> JSON:
        """Replay this live account's recorded attempt against presets and custom rule sets
        ({"name", "initial_cash", "rules"}) side by side. Read-only; nothing is re-executed."""
        body: JSON = {"include_current": include_current}
        if plans is not None:
            body["plans"] = plans
        if rules is not None:
            body["rules"] = rules
        return self._request("POST", "/account/rules-compare", body, scoped=True, control=True)

    def validate_rule_pack(self, pack: JSON) -> JSON:
        """Check a versioned rule pack and return it normalized; nothing is stored."""
        return self._request("POST", "/rule-packs/validate", pack, control=True)

    def strategy_template(self, symbol: str, expiry: str, template: JSON, *,
                          min_strike: float | None = None, max_strike: float | None = None) -> JSON:
        return self._request("GET", "/strategy-template", params={"symbol": symbol, "expiry": expiry,
                             "template": json.dumps(template, allow_nan=False),
                             "min_strike": min_strike, "max_strike": max_strike})

    def replay_history(self) -> JSON:
        return self._request("GET", "/replay/history", control=True)

    def get_replay(self, run_id: str) -> JSON:
        return self._request("GET", "/replay/history/" + quote(run_id, safe=""), control=True)

    def list_replays(self) -> ReplayListing:
        return self._request("GET", "/replay", control=True)

    def start_replay(self, **settings) -> ReplayResult:
        return self._request("POST", "/replay", settings, control=True)

    def replay_from_trade(self, run_id: str, trade_id: str) -> ReplayResult:
        """Fresh paused account five minutes before entry, with verified source inputs."""
        return self._request("POST", "/replay", {"replay_from": run_id, "trade": str(trade_id)}, control=True, retry=False)

    def scenario_packs(self) -> JSON:
        return self._request("GET", "/replay/packs", control=True)

    def scenario_pack(self, pack_id: str) -> JSON:
        """One pack exactly as the server stores it; seeds stay decimal strings."""
        return self._request("GET", "/replay/packs/" + quote(pack_id, safe=""), control=True)

    def import_scenario_pack(self, pack: JSON) -> JSON:
        return self._request("POST", "/replay/packs", pack, control=True)

    def start_scenario_pack(self, pack_id: str, *, paused: bool = True, speed: int = 1) -> ReplayResult:
        return self._request("POST", "/replay", {"pack": pack_id, "paused": paused, "speed": speed}, control=True, retry=False)

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
