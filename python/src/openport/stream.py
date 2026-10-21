# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Versioned paper events and the separate coalescing market tick channel."""
from __future__ import annotations

from dataclasses import dataclass
import json
from typing import AsyncIterator, Literal
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit


@dataclass(frozen=True)
class Event:
    version: int
    sequence: str
    event: str
    account: str
    source: Literal["live", "replay"]
    run_id: str | None
    market_time: str | None
    simulated: bool
    message: str
    details: dict

    @classmethod
    def parse(cls, value: dict) -> Event:
        if value.get("type") != "event" or value.get("version") != 1:
            raise ValueError("Unsupported OpenPort event version or message type")
        if value.get("source") not in {"live", "replay"} or value.get("simulated") is not True:
            raise ValueError("Invalid paper event provenance")
        return cls(**{key: value[key] for key in cls.__dataclass_fields__})


def _connect():
    try:
        from websockets.asyncio.client import connect
    except ImportError as error:
        raise ImportError("Install openport[ws] to stream ticks or events") from error
    return connect


async def ticks(url: str = "ws://127.0.0.1:8080/ws", *, token: str | None = None):
    """Yield tick/replay_tick dictionaries. Requires openport[ws]."""
    headers = {"Authorization": "Bearer " + token} if token else None
    async with _connect()(url, additional_headers=headers) as socket:
        async for message in socket:
            value = json.loads(message)
            if value.get("type") in {"tick", "replay_tick"}:
                yield value


async def events(url: str = "ws://127.0.0.1:8080/ws/events", *, token: str | None = None,
                 account: str | None = None, source: Literal["live", "replay"] = "live",
                 ready=None) -> AsyncIterator[Event]:
    """Yield typed v1 events. No replay/resume guarantee; reconcile REST after disconnect.

    Sequence IDs are process-local and may skip events for other accounts. ready,
    when supplied, is an asyncio.Event set after the server installs the subscription.
    """
    parts = urlsplit(url)
    if parts.scheme not in {"ws", "wss"} or parts.username or parts.fragment:
        raise ValueError("Expected a WS(S) URL without credentials or fragment")
    if source not in {"live", "replay"}:
        raise ValueError("source must be live or replay")
    params = dict(parse_qsl(parts.query))
    params.update(version="1", source=source)
    if account is not None:
        params["account"] = account
    url = urlunsplit(parts._replace(query=urlencode(params)))
    headers = {"Authorization": "Bearer " + token} if token else None
    async with _connect()(url, additional_headers=headers) as socket:
        async for message in socket:
            value = json.loads(message)
            if value.get("type") == "event_stream":
                if value.get("version") != 1:
                    raise ValueError("Unsupported OpenPort event version")
                if ready is not None:
                    ready.set()
                continue
            yield Event.parse(value)
