# Copyright (c) 2026 OpenPort contributors. MIT License.
import asyncio
import json
from typing import get_type_hints
from urllib.parse import parse_qs, urlsplit

import pandas as pd
from jsonschema import Draft202012Validator
from openport import Client, exposure_frame
from openport.types import Exposure, ExposureExpiry, ExposureSummary, ExposureLevels, ExposureMetricLevels
from openport_mcp import create_server
from conftest import SPEC, TIME, shaped


def test_exposure_wire_types_and_nullable_flow_contract():
    for wire in (Exposure, ExposureExpiry, ExposureSummary, ExposureLevels, ExposureMetricLevels):
        name = "ExposureMatrix" if wire is Exposure else wire.__name__
        assert set(get_type_hints(wire)) == set(SPEC["components"]["schemas"][name]["properties"])
    data = shaped("ExposureMatrix", convention="customer_flow", slice="0dte")
    data["exposure"] = shaped("ExposureSummary", available=False, unavailable_reason="No aggressor side")
    for metric in ("gex", "vex", "dex", "cex", "vomex"):
        data["exposure"][metric] = None
    Draft202012Validator({"$ref": "#/components/schemas/ExposureMatrix", **SPEC}).validate(data)


def test_exposure_client_passes_modes_on_live_and_replay(stub):
    client = Client(stub.url)
    for target in (client, client.for_replay()):
        target.exposure("SPX", 4, .02, convention="short_all", slice="ex_0dte")
        query = parse_qs(urlsplit(stub.requests[-1][1]).query)
        assert query == {"expiries": ["4"], "window": ["0.02"], "convention": ["short_all"], "slice": ["ex_0dte"]}
    assert "/api/replay/underlyings/SPX/exposure?" in stub.requests[-1][1]
    client.exposure("SPX")
    assert parse_qs(urlsplit(stub.requests[-1][1]).query)["convention"] == ["open_interest"]


def test_exposure_frame_keeps_alignment_missing_values_and_context():
    data = {"strikes": [100, 110], "as_of": TIME, "convention": "customer_flow", "slice": "0dte",
            "expiries": [{"id": "expiry", "days": .1, "gex": [None, 0], "dex": [None, 10],
                          "cex": [None, -2], "vomex": [None, .5]}],
            "exposure": {"available": False, "unavailable_reason": "No signed volume"}}
    frame = exposure_frame(data)
    assert list(frame.strike) == [100, 110]
    assert pd.isna(frame.iloc[0].dex)
    assert frame.iloc[1].gex == 0
    assert frame.iloc[1].cex == -2
    assert frame.iloc[1].vomex == .5
    assert frame.vex.isna().all()
    assert frame.iloc[0].as_of == TIME
    assert frame.iloc[0].convention == "customer_flow"
    assert frame.iloc[0]["slice"] == "0dte"
    assert not frame.iloc[0].available
    assert frame.iloc[0].unavailable_reason == "No signed volume"
    assert exposure_frame({"expiries": [], "strikes": []}).empty


def test_mcp_exposure_schema_and_mode_forwarding(stub):
    server = create_server(Client(stub.url, "secret"))
    tools = {tool.name: tool for tool in asyncio.run(server.list_tools())}
    schema = tools["exposure"].input_schema["properties"]
    assert set(schema["convention"]["enum"]) == {"open_interest", "short_all", "long_calls_short_puts", "customer_flow"}
    assert set(schema["slice"]["enum"]) == {"all", "0dte", "ex_0dte"}
    response = asyncio.run(server.call_tool("exposure", {"symbol": "SPX", "replay": True,
        "convention": "short_all", "slice": "0dte", "expiries": 4, "window": .02}))
    result = json.loads(response.content[0].text)
    assert result["as_of"] == TIME
    request = next(target for _, target, _, _ in reversed(stub.requests) if "/exposure?" in target)
    assert request.startswith("/api/replay/underlyings/SPX/exposure?")
    assert parse_qs(urlsplit(request).query)["slice"] == ["0dte"]
