# Copyright (c) 2026 OpenPort contributors. MIT License.
import asyncio

import pytest
from openport import Client
from openport.bot import Runner, Strategy
from conftest import TIME


class Recorder(Strategy):
    def __init__(self):
        self.calls = []

    def on_start(self): self.calls.append(("start",))
    async def on_tick(self, tick): self.calls.append(("tick", tick))
    def on_bar(self, symbol, bar): self.calls.append(("bar", symbol, bar))
    def on_fill(self, fill): self.calls.append(("fill", fill))
    def on_stop(self): self.calls.append(("stop",))


@pytest.mark.parametrize("replay", [False, True])
def test_runner_fallback_deduplicates_fills_and_emits_closed_bars(stub, replay):
    original = stub.respond
    count = 0
    def respond(method, target, headers, body):
        nonlocal count
        status, value = original(method, target, headers, body)
        if "/fills" in target:
            count += 1
            value["fills"] = [] if count == 1 else [{"id": "1", "price": "1.23", "time": TIME}]
        if "/candles" in target:
            value["bars"] = [{"t": n * 60, "o": 1, "h": 1, "l": 1, "c": 1} for n in range(count + 1)]
        return status, value
    stub.respond = respond
    strategy = Recorder()
    client = Client(stub.url)
    if replay:
        client = client.for_replay()
    runner = Runner(client, strategy, symbols=("SPX",), poll_interval=0.001, use_events=False)
    asyncio.run(runner.run(max_ticks=3))
    assert strategy.calls[0] == ("start",) and strategy.calls[-1] == ("stop",)
    assert [item[1]["id"] for item in strategy.calls if item[0] == "fill"] == ["1"]
    assert [item[2]["t"] for item in strategy.calls if item[0] == "bar"] == [120, 180]
    if replay:
        assert all(target.startswith("/api/replay/") for _, target, *_ in stub.requests)


def test_runner_history_walks_recorded_rows_and_always_stops(stub):
    original = stub.respond
    def respond(method, target, headers, body):
        status, value = original(method, target, headers, body)
        if "/fills" in target:
            value["fills"] = [{"id": "1", "time": TIME, "price": "1.23"}]
        if "/equity" in target:
            value["samples"] = [{"time": TIME, "equity": "100.00"}]
        return status, value
    stub.respond = respond
    strategy = Recorder()
    asyncio.run(Runner(Client(stub.url).for_history("run-1"), strategy).run())
    assert [row[0] for row in strategy.calls] == ["start", "tick", "fill", "stop"]
    assert all(method == "GET" for method, *_ in stub.requests)


def test_hook_failure_still_runs_cleanup(stub):
    class Broken(Recorder):
        def on_start(self): raise RuntimeError("synthetic failure")
    strategy = Broken()
    with pytest.raises(RuntimeError, match="synthetic failure"):
        asyncio.run(Runner(Client(stub.url), strategy, use_events=False).run(max_ticks=1))
    assert strategy.calls == [("stop",)]


def test_event_wakeup_and_disconnect_fall_back_to_rest(stub, monkeypatch):
    from openport.stream import Event
    from test_stream import EVENT
    async def source(*args, **kwargs):
        yield Event.parse(EVENT)
        raise OSError("synthetic disconnect")
    monkeypatch.setattr("openport.bot.events", source)
    strategy = Recorder()
    runner = Runner(Client(stub.url), strategy, poll_interval=0.001)
    asyncio.run(runner.run(max_ticks=2))
    assert isinstance(runner.event_error, OSError)
    assert len([c for c in strategy.calls if c[0] == "tick"]) == 2
    assert strategy.calls[-1] == ("stop",)
