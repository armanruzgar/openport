# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Analyze helpers: the strategy optimizer, hypothetical profile legs and hedge suggestions."""
import asyncio
import json
from typing import get_type_hints
from urllib.parse import parse_qs, urlsplit

from openport import Client
from openport.types import Hedge, HedgeSuggestions, StrategyCandidate, StrategySearch, StrategySearchLeg
from openport_mcp import create_server
from conftest import SPEC


def test_wire_types_name_every_documented_field():
    schemas = SPEC["components"]["schemas"]
    for wire in (StrategySearch, StrategyCandidate, StrategySearchLeg, Hedge, HedgeSuggestions):
        assert set(get_type_hints(wire)) == set(schemas[wire.__name__]["properties"]), wire.__name__


def test_optimizer_and_profile_legs_encode_their_queries(stub):
    client = Client(stub.url, "secret", "practice")
    client.optimize_strategies("SPX", 5050, 1500, date="2026-10-22", expiries=["2026-10-22PM", "2026-11-20AM"],
                               structures=["vertical", "condor"], rank="pop", vol_points=-2, price="mid", limit=5)
    target = urlsplit(stub.requests[-1][1])
    assert target.path == "/api/underlyings/SPX/optimizer"
    assert parse_qs(target.query) == {"target": ["5050"], "budget": ["1500"], "date": ["2026-10-22"],
                                      "expiry_ids": ["2026-10-22PM,2026-11-20AM"], "structures": ["vertical,condor"],
                                      "rank": ["pop"], "vol_points": ["-2"], "price": ["mid"], "limit": ["5"]}
    client.risk_profile("SPX", days=[0, 7], legs={"SPXW  261022C05000000": 1, "SPXW  261022C05050000": -1}, held=False)
    query = parse_qs(urlsplit(stub.requests[-1][1]).query)
    assert query["legs"] == ["SPXW  261022C05000000:1,SPXW  261022C05050000:-1"]
    assert query["held"] == ["false"] and query["account"] == ["practice"]
    client.risk_profile("SPX")
    assert "held" not in parse_qs(urlsplit(stub.requests[-1][1]).query)
    client.hedges("grid", scope="SPX", utilisation=0.9)
    target = urlsplit(stub.requests[-1][1])
    assert target.path == "/api/risk/hedges"
    assert parse_qs(target.query) == {"target": ["grid"], "scope": ["SPX"], "utilisation": ["0.9"], "account": ["practice"]}


def test_mcp_tools_forward_to_the_market_and_the_account(stub):
    server = create_server(Client(stub.url, "secret"))
    def call(name, **arguments):
        return json.loads(asyncio.run(server.call_tool(name, arguments)).content[0].text)
    response = call("optimize_strategies", symbol="SPX", target=5050, budget=1000, structures=["butterfly"], replay=True)
    assert "error" not in response
    assert urlsplit(stub.requests[-1][1]).path == "/api/replay/underlyings/SPX/optimizer"
    assert parse_qs(urlsplit(stub.requests[-1][1]).query)["structures"] == ["butterfly"]
    response = call("hedges", target="vega", account="main", scope="SPX")
    assert "error" not in response
    assert stub.requests[-1][1].startswith("/api/risk/hedges?")
    call("risk_profile", account="main", underlying="SPX", legs={"SPXW  261022C05000000": 2}, held=False)
    assert parse_qs(urlsplit(stub.requests[-1][1]).query)["legs"] == ["SPXW  261022C05000000:2"]
