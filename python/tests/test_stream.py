# Copyright (c) 2026 OpenPort contributors. MIT License.
import asyncio
import json

import pytest
from openport.stream import Event, events
from conftest import TIME


EVENT = {"type": "event", "version": 1, "sequence": "7", "event": "fill", "account": "main",
         "source": "live", "run_id": None, "market_time": TIME, "simulated": True,
         "message": "Synthetic fill", "details": {"id": "1", "price": "1.23"}}


def test_typed_event_preserves_money_and_missing_time():
    assert Event.parse(EVENT).details["price"] == "1.23"
    assert Event.parse({**EVENT, "market_time": None}).market_time is None
    with pytest.raises(ValueError, match="version"):
        Event.parse({**EVENT, "version": 2})
    with pytest.raises(ValueError, match="provenance"):
        Event.parse({**EVENT, "simulated": False})


def test_event_iterator_uses_real_websocket_and_closes():
    from websockets.asyncio.server import serve
    async def exercise():
        requests = []
        async def handler(socket):
            requests.append(socket.request)
            await socket.send(json.dumps({"type": "event_stream", "version": 1}))
            await socket.send(json.dumps(EVENT))
        async with serve(handler, "127.0.0.1", 0) as server:
            port = server.sockets[0].getsockname()[1]
            ready = asyncio.Event()
            received = [event async for event in events(f"ws://127.0.0.1:{port}/ws/events", token="secret", account="main", ready=ready)]
        assert ready.is_set()
        assert received == [Event.parse(EVENT)]
        assert "version=1" in requests[0].path and "account=main" in requests[0].path
        assert requests[0].headers["Authorization"] == "Bearer secret"
    asyncio.run(exercise())


def test_event_matches_openapi():
    import jsonschema
    from conftest import SPEC
    jsonschema.Draft202012Validator(SPEC["components"]["schemas"]["PaperEvent"]).validate(EVENT)
