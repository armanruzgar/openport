# Copyright (c) 2026 OpenPort contributors. MIT License.
import asyncio
import json
from urllib.parse import parse_qs, urlsplit

import pytest
from openport import ApiError, Client, chain_frame, smile_frame, ticks, trades_frame
from conftest import TIME


def test_urls_auth_accounts_and_unknown_fields(stub):
    client = Client(stub.url, "secret", "practice")
    stub.chain["future_field"] = {"new": True}
    assert client.chain("SPX", "2026-10-22PM")["future_field"] == {"new": True}
    client.orders("open")
    method, path, headers, _ = stub.requests[-1]
    assert method == "GET"
    assert parse_qs(urlsplit(path).query) == {"status": ["open"], "account": ["practice"]}
    assert headers["Authorization"] == "Bearer secret"
    client.orders(client_order_id="c1 &")
    assert parse_qs(urlsplit(stub.requests[-1][1]).query) == {"status": ["all"], "client_order_id": ["c1 &"], "account": ["practice"]}
    client.chain("A/B &", "expiry &+")
    assert "/A%2FB%20%26/chain?" in stub.requests[-1][1]
    assert parse_qs(urlsplit(stub.requests[-1][1]).query)["expiry"] == ["expiry &+"]
    client.accounts()
    assert "account=" not in stub.requests[-1][1]
    assert client.symbols() == ["SPX"]
    assert client.summary("SPX")["expiries"][0]["id"] == "2026-10-22PM"


def test_series_reads_metric_history_from_its_route(stub):
    client = Client(stub.url, account="practice")
    series = client.series("SPX")
    assert set(series) >= {"symbol", "interval", "from", "to", "fields", "rows"}
    assert urlsplit(stub.requests[-1][1]).path == "/api/underlyings/SPX/series"
    assert urlsplit(stub.requests[-1][1]).query == ""
    client.for_replay().series("SPX", ["mfiv30", "atm30", "rr25"], "1d", start="2026-01-01", end=1758571200)
    target = urlsplit(stub.requests[-1][1])
    assert target.path == "/api/replay/underlyings/SPX/series"
    assert parse_qs(target.query) == {"fields": ["mfiv30,atm30,rr25"], "interval": ["1d"],
                                      "from": ["2026-01-01"], "to": ["1758571200"]}


def test_errors_carry_reason_and_do_not_retry_422(stub):
    stub.failures = [(422, "BUYING_POWER")]
    with pytest.raises(ApiError) as caught:
        Client(stub.url).place_order(quantity=1)
    assert caught.value.status == 422
    assert caught.value.reason_code == "BUYING_POWER"
    assert len(stub.requests) == 1


def test_damaged_account_remains_readable_and_writes_do_not_retry(stub, monkeypatch):
    from conftest import shaped
    original = stub.respond
    damage = {"reason": "JOURNAL_CORRUPT: restore a verified backup", "last_good_seq": 2, "last_good_time": TIME}
    def respond(method, target, headers, body):
        if method == "GET" and urlsplit(target).path == "/api/account":
            return 200, {**shaped("Account"), "damaged": damage}
        return original(method, target, headers, body)
    monkeypatch.setattr(stub, "respond", respond)
    client = Client(stub.url)
    assert client.account()["damaged"] == damage
    stub.failures = [(409, "ACCOUNT_DAMAGED")]
    before = len(stub.requests)
    with pytest.raises(ApiError) as caught:
        client.place_order(quantity=1)
    assert caught.value.reason_code == "ACCOUNT_DAMAGED"
    assert len(stub.requests) == before + 1


def test_bounded_retry_reuses_id_and_body(stub, monkeypatch):
    sleeps = []
    monkeypatch.setattr("openport.client.time.sleep", sleeps.append)
    stub.failures = [(503, "TRADING_UNAVAILABLE"), (503, "TRADING_UNAVAILABLE")]
    client = Client(stub.url, "secret", "main", retries=2)
    request = {"quantity": 1, "type": "limit", "limit_price": "0.05"}
    result = client.place_order(request)
    assert sleeps == [0.1, 0.2]
    bodies = [call[3] for call in stub.requests]
    assert bodies[0] == bodies[1] == bodies[2]
    assert bodies[0]["client_order_id"]
    assert "client_order_id" not in request
    assert client.place_order(bodies[0])["order"]["id"] == result["order"]["id"]
    assert len(stub.order_bodies) == 1
    stub.failures = [(503, "TRADING_UNAVAILABLE")] * 4
    with pytest.raises(ApiError):
        client.status()
    assert len(stub.failures) == 1


def test_retry_waits_the_servers_retry_after(stub, monkeypatch):
    sleeps = []
    monkeypatch.setattr("openport.client.time.sleep", sleeps.append)
    stub.failures = [(503, "TRADING_UNAVAILABLE", 1), (503, "TRADING_UNAVAILABLE", 60), (503, "TRADING_UNAVAILABLE")]
    Client(stub.url, "secret", "main", retries=3).place_order(quantity=1)
    # Its own seconds, capped at ten; without the header, the backoff.
    assert sleeps == [1.0, 10.0, 0.4]


def test_preview_cancel_and_replay_routing(stub):
    client = Client(stub.url, "secret", "practice")
    assert client.preview_order(quantity=1)["max_units_basis"] == "limits"
    assert stub.requests[-1][3]["client_order_id"]
    preview = client.preview_change("1", limit_price="4.20")
    assert preview["simulated"]
    assert preview["max_units_basis"] == "limits"
    assert stub.requests[-1][1].startswith("/api/orders/1/preview?")
    assert stub.requests[-1][3] == {"limit_price": "4.20"}
    client.cancel_order("1")
    assert stub.requests[-1][3] is None
    assert client.what_if([{"name": "close", "orders": [{"symbol": "X", "side": "sell"}]}])["simulated"]
    assert stub.requests[-1][1].startswith("/api/orders/what-if?")
    assert client.preview_flatten("SPX")["simulated"]
    assert stub.requests[-1][1].startswith("/api/positions/close/preview?")
    assert stub.requests[-1][3] == {"underlying": "SPX"}
    client.for_replay().orders()
    assert stub.requests[-1][1].startswith("/api/replay/orders?")
    assert "account=main" in stub.requests[-1][1]
    client.for_history("run-1").trades()
    assert stub.requests[-1][1].startswith("/api/replay/history/run-1/trades?")
    client.start_replay(scenario="fixture")
    client.restart_replay("run-1", at="10:30:15", speed=60)
    assert stub.requests[-1][1] == "/api/replay"
    assert stub.requests[-1][3] == {"restart": "run-1", "at": "10:30:15", "speed": 60}
    client.for_history("run-1").restart_replay("run-1")
    assert stub.requests[-1][1] == "/api/replay"
    assert stub.requests[-1][3] == {"restart": "run-1"}
    client.resume_replay("run-1", speed=0)
    assert stub.requests[-1][0] == "POST" and stub.requests[-1][1] == "/api/replay"
    assert stub.requests[-1][3] == {"resume": "run-1", "speed": 0}
    assert client.step_replay("15:00")["settled_through"] == TIME
    assert stub.requests[-1][1] == "/api/replay"
    assert stub.requests[-1][3] == {"until": "15:00"}
    for target in ("+15s", "+1m", "+1h", "next"):
        client.step_replay(target)
        assert stub.requests[-1][3] == {"until": target}
    client.control_replay(skip=True)
    assert stub.requests[-1][3] == {"skip": True}
    client.control_replay(skip=False)
    assert stub.requests[-1][3] == {"skip": False}
    client.play_replay_until("+1m", speed=60)
    assert stub.requests[-1][3] == {"play_until": "+1m", "speed": 60}
    client.play_replay_until("14:30:15")
    assert stub.requests[-1][3] == {"play_until": "14:30:15"}
    client.abort_replay()
    assert stub.requests[-1][1] == "/api/replay"
    assert stub.requests[-1][3] == {"abort": True}
    assert client.stop_replay()["replay"] is None


def test_frames_preserve_missing_data_and_money(stub):
    stub.chain["strikes"][0]["put"]["iv"] = None
    frame = chain_frame(stub.chain)
    assert len(frame) == 1
    assert frame.iloc[0]["side"] == "put"
    assert frame.iloc[0]["iv"] is None
    smile = smile_frame({"as_of": TIME, "expiries": [{"id": "expiry", "points": [{"iv": None, "strike": 5000}]}]})
    assert smile.iloc[0]["expiry"] == "expiry"
    assert trades_frame({"trades": [{"net": "0.000001", "id": "1"}]}).iloc[0]["net"] == "0.000001"
    assert chain_frame({"strikes": [], "expiry": {"id": "e"}}).empty
    assert smile_frame({"expiries": []}).empty
    assert trades_frame({"trades": []}).empty


def test_missing_optional_dependencies_are_explicit(monkeypatch):
    import builtins
    original = builtins.__import__
    def blocked(name, *args, **kwargs):
        if name == "pandas" or name.startswith("websockets"):
            raise ImportError("not installed")
        return original(name, *args, **kwargs)
    monkeypatch.setattr(builtins, "__import__", blocked)
    with pytest.raises(ImportError, match=r"openport\[pandas\]"):
        trades_frame({"trades": []})
    async def read():
        with pytest.raises(ImportError, match=r"openport\[ws\]"):
            await anext(ticks())
    asyncio.run(read())


def test_ticks_preserve_messages_and_use_bearer(monkeypatch):
    captured = {}
    class Socket:
        async def __aenter__(self): return self
        async def __aexit__(self, *_args): pass
        def __aiter__(self): return self
        async def __anext__(self):
            if captured.get("sent"):
                raise StopAsyncIteration
            captured["sent"] = True
            return json.dumps({"type": "tick", "future": 1})
    def connect(url, **kwargs):
        captured.update(url=url, **kwargs)
        return Socket()
    monkeypatch.setattr("websockets.asyncio.client.connect", connect)
    async def read(): return [item async for item in ticks("ws://localhost/ws", token="secret")]
    assert asyncio.run(read()) == [{"type": "tick", "future": 1}]
    assert captured["additional_headers"] == {"Authorization": "Bearer secret"}


@pytest.mark.parametrize("url", ["ftp://localhost", "http://user:pass@localhost", "http://localhost?token=a", "http://localhost#x"])
def test_base_url_rejects_ambiguous_credentials(url):
    with pytest.raises(ValueError): Client(url)


@pytest.mark.parametrize("method,arguments,verb,path,schema", [
    ("summary", ("SPX",), "GET", "/api/underlyings/SPX/summary", "Summary"),
    ("exposure", ("SPX",), "GET", "/api/underlyings/SPX/exposure", "ExposureMatrix"),
    ("surface", ("SPX",), "GET", "/api/underlyings/SPX/surface", "Surface"),
    ("volatility", ("SPX",), "GET", "/api/underlyings/SPX/volatility", "Volatility"),
    ("surface_history", ("SPX",), "GET", "/api/underlyings/SPX/surface-history", "SurfaceHistory"),
    ("volatility_history", ("SPX",), "GET", "/api/underlyings/SPX/series", "VolatilitySeries"),
    ("volatility_scanner", (), "GET", "/api/volatility/scanner", "VolatilityScanner"),
    ("heston", ("SPX",), "GET", "/api/underlyings/SPX/heston", "Heston"),
    ("candles", ("SPX",), "GET", "/api/underlyings/SPX/candles", "Candles"),
    ("account", (), "GET", "/api/account", "Account"),
    ("portfolio", (), "GET", "/api/portfolio", "Portfolio"),
    ("fills", (), "GET", "/api/fills", "FillsResponse"),
    ("settlements", (), "GET", "/api/settlements", "SettlementsResponse"),
    ("trades", (), "GET", "/api/trades", "TradesResponse"),
    ("alerts", (), "GET", "/api/alerts", "AlertsResponse"),
    ("create_alert", ("account", "equity", "at_or_below", "99000"), "POST", "/api/alerts", "AlertResponse"),
    ("delete_alert", ("1",), "DELETE", "/api/alerts/1", "AlertDeleted"),
    ("risk", (), "GET", "/api/risk", "Risk"),
    ("risk_history", (), "GET", "/api/risk/history", "HistoricalRisk"),
    ("risk_scenarios", (), "GET", "/api/risk/scenarios", "ScenarioAnalysis"),
    ("risk_profile", ("SPX",), "GET", "/api/risk/profile", "RiskProfile"),
    ("probability", ("SPX", [0, 7], [5900]), "GET", "/api/underlyings/SPX/probability", "Probability"),
    ("plans", (), "GET", "/api/plans", "PlansResponse"),
    ("equity", (), "GET", "/api/account/equity", "EquityHistory"),
    ("cancel_all", ("SPX",), "POST", "/api/orders/cancel", "CancelAllResponse"),
    ("cancel_orders", (["7", "8"],), "POST", "/api/orders/cancel", "CancelAllResponse"),
    ("flatten", ("SPX",), "POST", "/api/positions/close", "ClosePositionsResponse"),
    ("note", ("1", "review", ["test"]), "PUT", "/api/trades/1/note", "TradeNoteResponse"),
    ("day_note", ("2026-09-22", "plan", "review"), "PUT", "/api/days/2026-09-22/note", "DayNoteResponse"),
    ("group_trades", (["1", "3"],), "POST", "/api/trades/group", "TradeGroupResponse"),
    ("ungroup_trades", (["3"],), "POST", "/api/trades/ungroup", "TradeGroupResponse"),
    ("create_account", ("Practice",), "POST", "/api/accounts", "CreateAccountResponse"),
    ("reset_account", ("new attempt",), "POST", "/api/account/reset", "Account"),
    ("transfer_cash", ("cash-1", "-100.00"), "POST", "/api/account/cash-flows", "Account"),
    ("halt_underlying", ("SPY", 5), "POST", "/api/market/halt", "Account"),
    ("payout", ("100.00",), "POST", "/api/account/payout", "Account"),
    ("limits", ("1", {}), "PUT", "/api/risk/limits", "Risk"),
    ("guardrails", ("1", {}), "PUT", "/api/risk/guardrails", "Risk"),
    ("kill", ("trip", "review"), "POST", "/api/risk/kill", "KillResponse"),
    ("settle", ("SPXW  261022P05000000", "5000.00"), "POST", "/api/settlements", "SettlementResponse"),
    ("exercise", ("SPY   261022C00500000", 1), "POST", "/api/positions/exercise", "Portfolio"),
    ("abandon", ("SPY   261022C00500000",), "POST", "/api/positions/abandon", "Portfolio"),
    ("exercise_instruction", ("SPY   261022C00500000", True), "POST", "/api/positions/instruction", "Portfolio"),
    ("close_stock", ("SPY", 100), "POST", "/api/stocks/close", "Portfolio"),
    ("trade_stock", ("SPY", "buy", 100), "POST", "/api/stocks/trade", "Portfolio"),
    ("preview_stock", ("SPY", "sell", 50), "POST", "/api/stocks/trade/preview", "StockPreview"),
    ("verify_replay", ("run-1",), "POST", "/api/replay/history/run-1/verify", "RunVerification"),
    ("replay_verification", ("run-1",), "GET", "/api/replay/history/run-1/verify", "RunVerification"),
    ("delete_replay", ("run-1",), "DELETE", "/api/replay/history/run-1", "DeletedReplay"),
])
def test_each_route_uses_the_terminal_method_and_path(stub, method, arguments, verb, path, schema):
    from conftest import shaped
    original = stub.respond
    def respond(request_method, target, headers, body):
        original(request_method, target, headers, body)
        return 200, shaped(schema)
    stub.respond = respond
    getattr(Client(stub.url, "secret", "practice"), method)(*arguments)
    request_method, target, headers, _body = stub.requests[-1]
    assert request_method == verb
    assert urlsplit(target).path == path
    assert headers["Authorization"] == "Bearer secret"
    scoped = path.endswith("/volatility") or (not path.startswith("/api/underlyings/") and path not in {"/api/plans", "/api/accounts", "/api/volatility/scanner"} and not path.startswith("/api/replay"))
    assert ("account" in parse_qs(urlsplit(target).query)) == scoped


def test_last_hour_example_waits_for_replay_preparation(stub, monkeypatch):
    import runpy
    from conftest import ROOT
    monkeypatch.syspath_prepend(str(ROOT / "python/examples"))
    wait = runpy.run_path(str(ROOT / "python/examples/last_hour.py"))["wait_until_ready"]
    client = Client(stub.url, "secret")
    client.start_replay(scenario="fixture", paused=True)
    original = stub.respond
    polls = 0
    def respond(method, target, headers, body):
        nonlocal polls
        status, value = original(method, target, headers, body)
        if method == "GET" and target == "/api/replay":
            polls += 1
            value["replay"]["fast_forwarding"] = polls == 1
        return status, value
    stub.respond = respond
    wait(client)
    assert polls == 2
    polls = 0
    with pytest.raises(TimeoutError, match="preparing"):
        wait(client, timeout=0)


def test_backtest_calls_use_global_routes_even_from_replay_and_history(stub):
    for client in (Client(stub.url), Client(stub.url).for_replay(), Client(stub.url, history="old-run")):
        client.list_backtests()
        client.start_backtest("morning@2", "eod-50k", scenarios=3, seed="18446744073709551610")
        assert stub.requests[-1][1] == "/api/backtests"
        assert stub.requests[-1][3]["seed"] == "18446744073709551610"
        client.get_backtest("000001")
        client.cancel_backtest("000001")
        assert [(row[0], row[1]) for row in stub.requests[-4:]] == [
            ("GET", "/api/backtests"), ("POST", "/api/backtests"),
            ("GET", "/api/backtests/000001"), ("DELETE", "/api/backtests/000001")]


@pytest.mark.parametrize("replay", [False, True])
def test_position_disposal_bodies_and_scope(stub, replay):
    from conftest import shaped
    original = stub.respond
    def respond(method, target, headers, body):
        original(method, target, headers, body)
        return 200, shaped("Portfolio")
    stub.respond = respond
    client = Client(stub.url, "secret", "practice")
    if replay:
        client = client.for_replay()
    prefix = "/api/replay" if replay else "/api"
    symbol = "SPXW  261022C05000000"
    client.abandon(symbol)
    method, target, _, body = stub.requests[-1]
    assert method == "POST"
    assert urlsplit(target).path == prefix + "/positions/abandon"
    assert body == {"symbol": symbol}
    assert parse_qs(urlsplit(target).query) == {"account": ["main" if replay else "practice"]}
    for instructed in (True, False):
        client.exercise_instruction(symbol, instructed)
        method, target, _, body = stub.requests[-1]
        assert method == "POST"
        assert urlsplit(target).path == prefix + "/positions/instruction"
        assert body == {"symbol": symbol, "do_not_exercise": instructed}


def test_extended_orders_and_limit_flatten(stub):
    for client in (Client(stub.url), Client(stub.url).for_replay()):
        for tif in ("exto", "gtc_exto", "gtd"):
            terms = {"type": "limit", "limit_price": "4.10", "time_in_force": tif, "quantity": 1}
            if tif == "gtd":
                terms["good_till"] = "2026-09-23T14:15:00Z"
            client.place_order(**terms)
            assert all(stub.requests[-1][3][key] == value for key, value in terms.items())
        for method in (client.flatten, client.preview_flatten):
            method("SPX", type="limit", limit_ticks=2)
            assert stub.requests[-1][3] == {"underlying": "SPX", "type": "limit", "limit_ticks": 2}
            for ticks in (-1, 11, True, 1.5):
                with pytest.raises(ValueError):
                    method(type="limit", limit_ticks=ticks)
            with pytest.raises(ValueError):
                method(type="market", limit_ticks=0)


def test_margin_settings_pass_through_account_requests(stub):
    from conftest import shaped
    original = stub.respond
    def respond(method, target, headers, body):
        original(method, target, headers, body)
        return 200, shaped("Account" if "/reset" in target else "CreateAccountResponse")
    stub.respond = respond
    client = Client(stub.url)
    settings = {"plan": "practice", "margin": "portfolio", "account_type": "margin",
                "house_margin_percent": 25, "pm_vol_shock": 5}
    client.reset_account("margin test", **settings)
    assert stub.requests[-1][3] == {"reason": "margin test", **settings}
    client.create_account("Portfolio", **settings)
    assert stub.requests[-1][3] == {"name": "Portfolio", **settings}


def test_share_trade_does_not_retry_an_uncertain_write(stub):
    def unavailable(method, target, headers, body):
        stub.requests.append((method, target, headers, body))
        return 503, {"error": {"code": "TRADING_UNAVAILABLE", "message": "unavailable"}}
    stub.respond = unavailable
    with pytest.raises(ApiError):
        Client(stub.url, "secret", retries=3, backoff=0).trade_stock("SPY", "buy", 100)
    assert len(stub.requests) == 1


def test_alerts_keep_decimal_levels_and_scope_in_live_replay_and_history(stub):
    from conftest import shaped
    original = stub.respond
    def respond(method, target, headers, body):
        original(method, target, headers, body)
        return 201, shaped("AlertResponse")
    stub.respond = respond
    client = Client(stub.url, "secret", "practice")
    legs = [{"symbol": "SPXW  261022C05000000", "side": "buy", "ratio": 1},
            {"symbol": "SPXW  261022C05100000", "side": "sell", "ratio": 2}]
    client.create_alert("spread", "mark", "at_or_below", "-0.123456", legs=legs, repeat=True, label="Credit")
    assert stub.requests[-1][3] == {"scope": "spread", "metric": "mark", "direction": "at_or_below",
                                  "level": "-0.123456", "legs": legs, "repeat": True, "label": "Credit"}
    client.for_replay().create_alert("underlying", "iv30", "at_or_above", "25", symbol="SPX")
    assert stub.requests[-1][1] == "/api/replay/alerts?account=main"
    client.for_history("run-1").alerts()
    assert stub.requests[-1][1] == "/api/replay/history/run-1/alerts?account=main"
    client.delete_alert("7/8")
    assert stub.requests[-1][0:2] == ("DELETE", "/api/alerts/7%2F8?account=practice")


def test_account_lifecycle_routes(stub):
    client = Client(stub.url, "secret", "unrelated")
    client.accounts(archived=True)
    assert stub.requests[-1][1] == "/api/accounts?archived=true"
    client.rename_account("swing", "Renamed")
    assert stub.requests[-1][0:2] == ("PATCH", "/api/accounts/swing")
    assert stub.requests[-1][3] == {"name": "Renamed"}
    client.archive_account("swing")
    assert stub.requests[-1][3] == {"archived": True}
    client.archive_account("swing", False)
    assert stub.requests[-1][3] == {"archived": False}
    client.delete_account("swing")
    assert stub.requests[-1][0:2] == ("DELETE", "/api/accounts/swing")
    assert stub.requests[-1][3] is None


def test_copy_active_settings_requests(stub):
    client = Client(stub.url, "secret")
    client.create_account("Copy", plan="eod-50k", copy_settings_from="main")
    assert stub.requests[-1][3] == {"name": "Copy", "plan": "eod-50k", "copy_settings_from": "main"}
    client.start_replay(scenario="reversal", plan="eod-50k", copy_settings_from="evaluation")
    assert stub.requests[-1][3] == {"scenario": "reversal", "plan": "eod-50k", "copy_settings_from": "evaluation"}


def test_replay_verification_receipt_is_authenticated_and_not_account_scoped(stub):
    original = stub.respond
    result = {"status": "passed", "message": "Run shared verified", "equity": "100000.00"}
    def respond(method, target, headers, body):
        original(method, target, headers, body)
        return 200, result
    stub.respond = respond
    client = Client(stub.url, "secret", "practice").for_history("another-run")
    assert client.replay_verification("shared run", receipt=True) == result
    method, path, headers, _ = stub.requests[-1]
    assert method == "GET"
    assert path == "/api/replay/history/shared%20run/verify?format=receipt"
    assert headers["Authorization"] == "Bearer secret"
    stub.failures = [(409, "VERIFICATION_RUNNING")]
    stub.respond = original
    with pytest.raises(ApiError) as caught:
        client.verify_replay("shared")
    assert caught.value.reason_code == "VERIFICATION_RUNNING"


def test_underlying_limit_overrides_live_and_replay(stub):
    client = Client(stub.url, "secret")
    limits = {"underlying_overrides": {"SPX": {"dollar_delta": 500000, "vega": 5000},
                                       "QQQ": {"dollar_delta": 2000000, "vega": 20000}}}
    for target, prefix in [(client, "/api"), (client.for_replay(), "/api/replay")]:
        target.limits("7", limits)
        assert urlsplit(stub.requests[-1][1]).path == prefix + "/risk/limits"
        assert stub.requests[-1][3] == {"expected_revision": "7", "limits": limits}


def test_sandbox_token_omits_account_by_default(stub):
    client = Client(stub.url, "sandbox_secret")
    client.account()
    assert stub.requests[-1][1] == "/api/account"
    assert stub.requests[-1][2]["Authorization"] == "Bearer sandbox_secret"
    client.cancel_all()
    assert stub.requests[-1][1] == "/api/orders/cancel"


def test_surface_requests_sabr_only_when_asked(stub):
    client = Client(stub.url, "secret")
    client.surface("SPX")
    assert "sabr_beta" not in parse_qs(urlsplit(stub.requests[-1][1]).query)
    client.surface("SPX", 6, 0.1, sabr_beta=0.5)
    assert parse_qs(urlsplit(stub.requests[-1][1]).query) == {"expiries": ["6"], "window": ["0.1"], "sabr_beta": ["0.5"]}
    client.surface("SPX", sabr_beta="fit")
    assert parse_qs(urlsplit(stub.requests[-1][1]).query)["sabr_beta"] == ["fit"]


def test_risk_reads_pass_smile_dynamics_only_when_chosen(stub):
    client = Client(stub.url, "secret", "practice")
    client.risk()
    assert parse_qs(urlsplit(stub.requests[-1][1]).query) == {"account": ["practice"]}
    client.risk("sticky_delta")
    assert parse_qs(urlsplit(stub.requests[-1][1]).query) == {"dynamics": ["sticky_delta"], "account": ["practice"]}
    client.risk_profile("SPX", dynamics="sticky_moneyness")
    query = parse_qs(urlsplit(stub.requests[-1][1]).query)
    assert query["dynamics"] == ["sticky_moneyness"] and query["underlying"] == ["SPX"]
    client.risk_scenarios("beta", [0, 5], term_scaled=True, dynamics="sticky_delta")
    query = parse_qs(urlsplit(stub.requests[-1][1]).query)
    assert query["dynamics"] == ["sticky_delta"] and query["mode"] == ["beta"] and query["term_scaled"] == ["true"]
    client.risk_scenarios()
    assert "dynamics" not in parse_qs(urlsplit(stub.requests[-1][1]).query)


def test_reload_tokens_uses_live_admin_route_even_from_replay(stub):
    result = Client(stub.url, "admin-secret", "practice").for_replay().reload_tokens()
    assert result == {"names": ["operator"], "count": 1, "loaded_at": TIME}
    assert stub.requests[-1][0:2] == ("POST", "/api/tokens/reload")
    assert stub.requests[-1][3] == {}


def test_equity_paging_keeps_account_and_range(stub):
    client = Client(stub.url, "secret", "practice")
    client.equity("2026-09-22T00:00:00Z", "2026-09-23T00:00:00Z", limit=2, cursor="123:2")
    assert parse_qs(urlsplit(stub.requests[-1][1]).query) == {
        "account": ["practice"], "from": ["2026-09-22T00:00:00Z"],
        "to": ["2026-09-23T00:00:00Z"], "limit": ["2"], "cursor": ["123:2"],
    }
    client.equity()
    assert parse_qs(urlsplit(stub.requests[-1][1]).query) == {"account": ["practice"]}


def test_backtest_compare_keep_delete_use_global_routes(stub):
    original = stub.respond
    def respond(method, target, headers, body):
        if target.startswith("/api/backtests/compare?"):
            stub.requests.append((method, target, dict(headers), body))
            return 200, {"runs": []}
        return original(method, target, headers, body)
    stub.respond = respond
    for client in (Client(stub.url), Client(stub.url).for_replay(), Client(stub.url, history="saved")):
        client.compare_backtests(["000001", "000002"])
        assert stub.requests[-1][0:2] == ("GET", "/api/backtests/compare?ids=000001%2C000002")
        client.keep_backtest("000001")
        assert stub.requests[-1][0:2] == ("PUT", "/api/backtests/000001")
        assert stub.requests[-1][3] == {"keep": True}
        client.delete_backtest("000001")
        assert stub.requests[-1][0:2] == ("DELETE", "/api/backtests/000001?purge=true")


def test_joint_backtests_use_global_routes_and_array_selectors(stub):
    for client in (Client(stub.url), Client(stub.url).for_replay(), Client(stub.url, history="old-run")):
        client.start_backtest(["beta@2", "alpha"], "eod-50k", scenarios=2, seed="1")
        assert stub.requests[-1][0:2] == ("POST", "/api/backtests")
        assert stub.requests[-1][3] == {"playbooks": ["beta@2", "alpha"], "plan": "eod-50k", "scenarios": 2, "seed": "1"}


def test_volatility_history_queries_and_account_calendar_scope(stub):
    client = Client(stub.url, account="practice")
    client.surface_history("SPX", compare=True, from_="2026-09-01", to="2026-09-22")
    target = stub.requests[-1][1]
    assert "compare=true" in target and "from=2026-09-01" in target
    client.surface_history("SPX")
    assert "compare=true" in stub.requests[-1][1]
    client.surface_history("SPX", compare=False)
    assert "compare=false" in stub.requests[-1][1]
    client.volatility_history("SPX", fields=["rr25", "bf25"], interval="1d")
    assert "fields=rr25%2Cbf25" in stub.requests[-1][1]
    client.volatility("SPX")
    assert "account=practice" in stub.requests[-1][1]


def test_journal_practice_calls_preserve_explicit_targets_and_uint64_seeds(stub):
    client = Client(stub.url, "secret").for_history("run-1")
    client.replay_from_trade("run-1", "7")
    assert stub.requests[-1][1] == "/api/replay"
    assert stub.requests[-1][3] == {"replay_from": "run-1", "trade": "7"}
    assert client.scenario_packs() == {"packs": [stub.pack]}
    assert stub.requests[-1][1] == "/api/replay/packs"
    assert client.scenario_pack("practice") == stub.pack
    assert stub.requests[-1][1] == "/api/replay/packs/practice"
    pack = {"version": 1, "scenario": {"id": "practice"}, "seed": "18446744073709551615", "plan": "practice"}
    assert client.import_scenario_pack(pack) == pack
    assert stub.requests[-1][1] == "/api/replay/packs"
    client.start_scenario_pack("practice")
    assert stub.requests[-1][3] == {"pack": "practice", "paused": True, "speed": 1}
    client.for_replay().note("1", "review", ["chased"], mistakes=["early exit"])
    assert stub.requests[-1][1] == "/api/replay/trades/1/note?account=main"
    assert stub.requests[-1][3] == {"note": "review", "tags": ["chased"], "mistakes": ["early exit"]}
@pytest.mark.parametrize("method,arguments,verb,path,schema", [
    ("flow", ("SPX", 50, "call", ["bid", "ask"]), "GET", "/api/underlyings/SPX/flow", "Flow"),
    ("earnings", ("AAPL",), "GET", "/api/earnings", "EarningsHistory"),
    ("import_earnings", ("symbol,date,session,implied_move,realized_move\n",), "POST", "/api/earnings/import", "EarningsImport"),
    ("delete_imported_earnings", (), "DELETE", "/api/earnings/imported", "EarningsRemoved"),
    ("analysis_portfolios", (), "GET", "/api/analysis/portfolios", "AnalysisPortfolios"),
    ("import_portfolio", ("IRA", "symbol,quantity\nSPY,1\n"), "POST", "/api/analysis/portfolios", "AnalysisPortfolio"),
    ("analysis_portfolio", ("ira",), "GET", "/api/analysis/portfolios/ira", "AnalysisPortfolio"),
    ("analysis_risk", ("ira", "grid"), "GET", "/api/analysis/portfolios/ira/risk/grid", "AnalysisGrid"),
    ("analysis_risk", ("ira", "stress"), "GET", "/api/analysis/portfolios/ira/risk/stress", "AnalysisStress"),
    ("analysis_risk", ("ira", "var"), "GET", "/api/analysis/portfolios/ira/risk/var", "AnalysisVar"),
    ("delete_analysis_portfolio", ("ira",), "DELETE", "/api/analysis/portfolios/ira", "AnalysisDeleted"),
])
def test_flow_earnings_and_analysis_routes_are_never_account_scoped(stub, method, arguments, verb, path, schema):
    from conftest import shaped
    original = stub.respond
    def respond(request_method, target, headers, body):
        original(request_method, target, headers, body)
        return 200, shaped(schema)
    stub.respond = respond
    getattr(Client(stub.url, "secret", "practice"), method)(*arguments)
    request_method, target, _headers, _body = stub.requests[-1]
    assert request_method == verb
    assert urlsplit(target).path == path
    assert "account" not in parse_qs(urlsplit(target).query)
