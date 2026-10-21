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
    assert set(tools) == {"status", "symbols", "summary", "chain", "volatility", "exposure", "account", "positions", "orders", "risk", "risk_profile", "probability", "preview_order", "place_order", "trade_stock", "preview_stock", "cancel_order", "flatten", "list_scenarios", "start_replay", "step_replay", "stop_replay"}
    for name in ["start_replay", "step_replay", "stop_replay"]:
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
    for name in ["account", "positions", "orders", "risk", "risk_profile", "place_order", "preview_order", "cancel_order", "flatten"]:
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


def test_place_order_returns_the_client_order_id_to_retry_after_an_uncertain_response(stub, monkeypatch):
    server = create_server(Client(stub.url, "secret", retries=0))
    order = {"quantity": 1, "type": "limit", "time_in_force": "day", "symbol": "SPXW  261022P05000000", "side": "buy", "limit_price": "0.05"}
    # The status read and then the order both meet an unavailable server.
    stub.failures = [(503, "TRADING_UNAVAILABLE"), (503, "TRADING_UNAVAILABLE")]
    failed = call(server, "place_order", account="practice", order=order)
    assert failed["error"]["status"] == 503
    sent = stub.requests[-1][3]["client_order_id"]
    assert failed["client_order_id"] == sent
    retried = call(server, "place_order", account="practice", order={**order, "client_order_id": failed["client_order_id"]})
    assert "error" not in retried
    assert stub.requests[-1][3]["client_order_id"] == sent == retried["client_order_id"]
    # A timeout is an uncertain response too, not a bare tool error.
    def timed_out(self, order=None, **fields):
        raise TimeoutError("timed out")
    monkeypatch.setattr(Client, "place_order", timed_out)
    lost = call(server, "place_order", account="practice", order=order)
    assert lost["error"]["code"] == "NO_RESPONSE"
    assert lost["client_order_id"] and lost["client_order_id"] != sent
