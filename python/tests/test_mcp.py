# Copyright (c) 2026 OpenPort contributors. MIT License.
import asyncio
import json

import pytest
from openport import Client
from openport_mcp import create_server
from conftest import TIME


def call(server, name, **arguments):
    result = asyncio.run(server.call_tool(name, arguments))
    # MCP 2.x serializes the tool result as structured content and JSON text.
    return json.loads(result.content[0].text)


def test_tool_schemas_name_accounts_and_typed_orders(stub):
    server = create_server(Client(stub.url, "secret"))
    tools = {tool.name: tool for tool in asyncio.run(server.list_tools())}
    assert {"status", "symbols", "summary", "chain", "volatility", "volatility_history", "surface_history", "volatility_scanner", "exposure", "account", "positions", "orders", "risk", "risk_profile", "risk_history", "risk_scenarios", "probability", "preview_order", "place_order", "trade_stock", "preview_stock", "cancel_order", "flatten", "list_scenarios", "start_replay", "step_replay", "stop_replay", "replay_from_trade", "scenario_packs", "import_scenario_pack", "start_scenario_pack"} <= set(tools)
    for name in ["start_replay", "step_replay", "stop_replay", "replay_from_trade", "import_scenario_pack", "start_scenario_pack"]:
        assert "account" in tools[name].input_schema["required"]
    schema = tools["place_order"].input_schema
    assert schema["$defs"]["Order"]["properties"]["quantity"]["type"] == "integer"
    assert "limit" in schema["$defs"]["Order"]["properties"]["type"]["enum"]
    assert {"exto", "gtc_exto", "gtd"} <= set(schema["$defs"]["Order"]["properties"]["time_in_force"]["enum"])


def test_extended_orders_and_limit_flatten_reach_replay(stub):
    server = create_server(Client(stub.url, "secret"))
    order = {"quantity": 1, "type": "limit", "time_in_force": "gtd", "good_till": "2026-09-23T14:00:00Z",
             "symbol": "SPXW  261022P05000000", "side": "buy", "limit_price": "0.05"}
    call(server, "place_order", account="main", replay=True, order=order)
    assert stub.requests[-1][1] == "/api/replay/orders?account=main"
    assert stub.requests[-1][3]["good_till"] == order["good_till"]
    call(server, "flatten", account="main", replay=True, type="limit", limit_ticks=2)
    assert stub.requests[-1][1] == "/api/replay/positions/close?account=main"
    assert stub.requests[-1][3] == {"type": "limit", "limit_ticks": 2}


def test_results_have_market_time_delay_simulation_and_agent_tag(stub):
    server = create_server(Client(stub.url, "secret"), "research")
    result = call(server, "chain", symbol="SPX")
    assert result["as_of"] == TIME
    assert result["simulated"] is True
    assert result["feed"]["delayed"] is True
    assert "window=0.03" in stub.requests[-1][1]
    order = {"quantity": 1, "type": "limit", "time_in_force": "gtc", "symbol": "SPXW  261022P05000000", "side": "buy", "limit_price": "0.05"}
    result = call(server, "place_order", account="practice", order=order)
    assert result["account"] == "practice"
    assert result["paper"] is True
    assert stub.requests[-1][3]["tags"] == ["agent:research"]
    assert stub.requests[-1][3]["client_order_id"]
    assert "account=practice" in stub.requests[-1][1]
    # A time trigger is sent without the level and direction it does not have.
    order["trigger"] = {"source": "time", "at": "15:30"}
    call(server, "place_order", account="practice", order=order)
    assert stub.requests[-1][3]["trigger"] == {"source": "time", "at": "15:30"}
    order["trigger"] = {"source": "underlying", "symbol": "VIX", "direction": "at_or_above", "level": "20"}
    call(server, "place_order", account="practice", order=order)
    assert stub.requests[-1][3]["trigger"] == order["trigger"]


@pytest.mark.parametrize("replay", [False, True])
def test_volatility_tools_preserve_source_ranges_and_account_calendars(stub, replay):
    from urllib.parse import parse_qs, urlsplit
    server = create_server(Client(stub.url, "secret"))
    prefix = "/api/replay" if replay else "/api"
    calls = [
        ("volatility", {"symbol": "SPX", "account": "main"}, "/underlyings/SPX/volatility", {"account": ["main"]}),
        ("volatility_history", {"symbol": "SPX", "fields": ["rr25", "bf25"], "from_": "2026-09-01"},
         "/underlyings/SPX/series", {"fields": ["rr25,bf25"], "interval": ["1d"], "from": ["2026-09-01"]}),
        ("surface_history", {"symbol": "SPX"}, "/underlyings/SPX/surface-history", {"compare": ["true"]}),
        ("volatility_scanner", {}, "/volatility/scanner", {}),
    ]
    for name, arguments, path, query in calls:
        response = call(server, name, replay=replay, **arguments)
        assert "error" not in response
        assert response["as_of"] == TIME
        assert response["simulated"] is True
        request = urlsplit(stub.requests[-1][1])
        assert request.path == prefix + path
        assert parse_qs(request.query) == query
    assert all(request[0] == "GET" for request in stub.requests)


def test_orders_chain_and_trail(stub):
    server = create_server(Client(stub.url, "secret"), "research")
    stop = {"source": "option", "direction": "at_or_below", "level": "0.02", "reference": "mid", "trail": {"unit": "ticks", "value": 2}}
    target = {"quantity": 1, "type": "limit", "time_in_force": "gtc", "symbol": "SPXW  261022P05000000", "side": "sell", "limit_price": "0.10",
              "oco": {"quantity": 1, "type": "market", "time_in_force": "ioc", "symbol": "SPXW  261022P05000000", "side": "sell", "trigger": stop}}
    order = {"quantity": 1, "type": "limit", "time_in_force": "gtc", "symbol": "SPXW  261022P05000000", "side": "buy", "limit_price": "0.05", "then": target}
    call(server, "place_order", account="practice", order=order)
    sent = stub.requests[-1][3]
    assert sent["then"]["oco"]["trigger"] == stop
    assert "client_order_id" not in sent["then"] and "tags" not in sent["then"]


def test_write_tools_require_token_even_on_loopback(stub):
    server = create_server(Client(stub.url))
    result = call(server, "flatten", account="main")
    assert result["error"]["code"] == "WRITE_TOKEN_REQUIRED"
    assert all(request[0] == "GET" for request in stub.requests)
    assert result["as_of"] == TIME


def test_missing_quote_time_is_not_replaced_by_another_status_timestamp(stub):
    stub.chain["as_of"] = None
    result = call(create_server(Client(stub.url)), "chain", symbol="SPX")
    assert result["as_of"] is None
    assert result["simulated"] is True


def test_replay_metadata_and_window_bounds(stub):
    server = create_server(Client(stub.url, "secret"))
    assert call(server, "chain", symbol="SPX", window=0)["error"]["code"] == "INVALID_WINDOW"
    assert call(server, "start_replay", account="other", scenario="fixture")["error"]["code"] == "INVALID_ACCOUNT"
    assert call(server, "start_replay", account="main", scenario="fixture")["simulated"] is True
    assert call(server, "step_replay", account="main", until="15:00")["as_of"] == TIME
    assert call(server, "stop_replay", account="main")["data"]["replay"] is None


@pytest.mark.parametrize("name", ["BAD", "../agent", "a" * 27, ""])
def test_agent_names_fit_server_tags(name):
    with pytest.raises(ValueError): create_server(Client(), name)


def test_replay_receipt_clock_is_not_reported_as_market_time(stub):
    server = create_server(Client(stub.url, "secret"))
    result = call(server, "start_replay", account="main", file="recording.oprec")
    assert result["as_of"] == TIME
    stub.replay["time"] = "2026-09-22T14:15:00Z"
    result = call(server, "step_replay", account="main", until="10:15")
    assert result["data"]["replay"]["time"] != TIME
    assert result["as_of"] == TIME
    assert call(server, "stop_replay", account="practice")["error"]["code"] == "INVALID_ACCOUNT"
    assert stub.replay is not None


def test_share_tools_forward_scoped_and_replay_requests(stub):
    server = create_server(Client(stub.url, "secret"))
    for tool, suffix in [("trade_stock", ""), ("preview_stock", "/preview")]:
        call(server, tool, account="main", replay=True, symbol="SPY", side="buy", shares=100)
        assert stub.requests[-1][1] == "/api/replay/stocks/trade" + suffix + "?account=main"
        assert stub.requests[-1][3] == {"symbol": "SPY", "side": "buy", "shares": 100}


def test_sandbox_tools_default_to_own_account_but_other_writers_still_name_one(stub):
    server = create_server(Client(stub.url, "sandbox_secret"))
    tools = {tool.name: tool for tool in asyncio.run(server.list_tools())}
    for name in ["account", "positions", "orders", "risk", "risk_profile", "risk_history", "risk_scenarios", "place_order", "preview_order", "cancel_order", "flatten"]:
        assert "account" not in tools[name].input_schema.get("required", [])
    assert "error" not in call(server, "account")
    assert stub.requests[-1][1] == "/api/account"
    order = {"quantity": 1, "type": "limit", "time_in_force": "gtc", "symbol": "SPXW  261022P05000000", "side": "buy", "limit_price": "0.05"}
    assert "error" not in call(server, "place_order", order=order)
    assert stub.requests[-1][1] == "/api/orders"
    assert "error" not in call(server, "flatten")
    assert stub.requests[-1][1] == "/api/positions/close"
    ordinary = create_server(Client(stub.url, "named-secret"))
    assert call(ordinary, "place_order", order=order)["error"]["code"] == "INVALID_ACCOUNT"


def test_copilot_parity_tools_and_scoped_mutations(stub):
    server = create_server(Client(stub.url, "secret"))
    tools = {tool.name for tool in asyncio.run(server.list_tools())}
    assert {"start_backtest", "get_backtest", "compare_backtests", "playbooks", "get_playbook",
            "create_playbook", "version_playbook", "playbook_mode", "send_staged_order", "dismiss_staged_order",
            "alerts", "create_alert", "delete_alert", "note", "day_note", "what_if", "candles", "surface",
            "equity", "calendar_events", "modify_order", "preview_change", "pass_odds", "strategy_template"} <= tools
    for name, arguments, method, path in [
        ("playbooks", {}, "GET", "/api/replay/playbooks?account=main"),
        ("preview_change", {"order_id": "1", "change": {"quantity": 2}}, "POST", "/api/replay/orders/1/preview?account=main"),
        ("modify_order", {"order_id": "1", "change": {"quantity": 2}}, "PUT", "/api/replay/orders/1?account=main"),
        ("what_if", {"candidates": []}, "POST", "/api/replay/orders/what-if?account=main"),
    ]:
        response = call(server, name, account="main", replay=True, **arguments)
        assert "error" not in response
        assert response["as_of"] == TIME
        assert stub.requests[-1][:2] == (method, path)
    response = call(server, "start_backtest", playbook="morning", plan="practice", settings={"scenarios": 2})
    assert "error" not in response
    assert stub.requests[-1][1] == "/api/backtests"
    assert stub.requests[-1][3]["scenarios"] == 2


@pytest.mark.parametrize("name,arguments", [
    ("modify_order", {"order_id": "1", "change": {"quantity": 2}}),
    ("create_alert", {"scope": "account", "metric": "equity", "direction": "at_or_below", "level": "1"}),
    ("note", {"trade_id": "1"}), ("create_playbook", {"definition": {}}),
    ("playbook_mode", {"playbook_id": "morning", "mode": "auto"}),
    ("send_staged_order", {"stage_id": "1"}),
    ("start_backtest", {"playbook": "morning", "plan": "practice"}),
    ("start_sweep", {"playbook": "morning", "plan": "practice", "parameter_grid": {"/sizing/units": [1, 2]}}),
    ("evaluate_playbook", {"playbook_id": "morning", "account": "main"}),
])
def test_new_mutating_tools_require_token(stub, name, arguments):
    response = call(create_server(Client(stub.url)), name, **arguments)
    assert response["error"]["code"] == "WRITE_TOKEN_REQUIRED"
    assert all(request[0] == "GET" for request in stub.requests)


def test_mcp_sweep_uses_global_scoped_job_routes(stub):
    server = create_server(Client(stub.url, "secret"))
    grid = {"/sizing/units": [1, 2]}
    response = call(server, "start_sweep", playbook="morning", plan="practice", parameter_grid=grid, settings={"scenarios": 2, "seed": "1"})
    assert response["data"]["kind"] == "sweep"
    assert stub.requests[-1][1] == "/api/backtests/sweeps"
    assert stub.requests[-1][3]["parameter_grid"] == grid
    response = call(server, "get_sweep", run_id="000001")
    assert response["data"]["id"] == "000001"


def test_batch_tools_do_not_borrow_live_quote_provenance(stub):
    stub.provider["simulated"] = False
    response = call(create_server(Client(stub.url, "secret")), "get_backtest", run_id="000001")
    assert response["simulated"] is True
    assert response["as_of"] is None
    assert response["feed"] == {"provider": None, "delayed": None, "delay_seconds": None}


def test_practice_tools_use_global_replay_controls(stub):
    server = create_server(Client(stub.url, "secret"))
    assert "error" not in call(server, "replay_from_trade", account="main", run_id="run-1", trade_id="7")
    assert [r for r in stub.requests if r[0] == "POST"][-1][1] == "/api/replay"
    assert [r for r in stub.requests if r[0] == "POST"][-1][3] == {"replay_from": "run-1", "trade": "7"}
    assert "error" in call(server, "start_scenario_pack", account="wrong", pack_id="practice")
    assert "error" not in call(server, "start_scenario_pack", account="main", pack_id="practice")
    assert [r for r in stub.requests if r[0] == "POST"][-1][3] == {"pack": "practice", "paused": True, "speed": 1}
    assert "error" not in call(server, "scenario_packs")
    assert stub.requests[-1][1] == "/api/replay/packs"
