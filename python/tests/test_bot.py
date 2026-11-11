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
    # The start saw bars 0 and 60: 0 had closed, 60 was still open and closes later.
    assert [item[2]["t"] for item in strategy.calls if item[0] == "bar"] == [60, 120, 180]
    if replay:
        assert all(target.startswith("/api/replay/") for _, target, *_ in stub.requests)


def bars(*times):
    return [{"t": t, "o": 1, "h": 1, "l": 1, "c": 1} for t in times]


@pytest.mark.parametrize("polls,delivered", [
    ([[], [0, 60], [0, 60, 120]], [0, 60]),  # no bar yet when the runner starts
    ([[0], [0, 60], [0, 60, 120]], [0, 60]),  # one bar, still open
    ([[0, 60], [0, 60, 120, 180], [0, 60, 120, 180, 240]], [60, 120, 180]),  # two close between polls
])
def test_runner_delivers_every_bar_that_closes_after_it_starts(stub, polls, delivered):
    original = stub.respond
    reads = 0
    def respond(method, target, headers, body):
        nonlocal reads
        status, value = original(method, target, headers, body)
        if "/candles" in target:
            value["bars"] = bars(*polls[min(reads, len(polls) - 1)])
            reads += 1
        return status, value
    stub.respond = respond
    strategy = Recorder()
    runner = Runner(Client(stub.url), strategy, symbols=("SPX",), poll_interval=0.001, use_events=False)
    # The first read is before on_start; each tick reads once more.
    asyncio.run(runner.run(max_ticks=len(polls) - 1))
    assert reads == len(polls)
    assert [item[2]["t"] for item in strategy.calls if item[0] == "bar"] == delivered


@pytest.mark.parametrize("hook", ["on_start", "on_tick"])
def test_runner_delivers_the_bars_a_replay_step_closes_before_its_first_poll(stub, hook):
    original = stub.respond
    stepped = False
    def respond(method, target, headers, body):
        nonlocal stepped
        status, value = original(method, target, headers, body)
        stepped = stepped or (method == "PUT" and target == "/api/replay")
        if "/candles" in target:
            # A paused replay's first bar, open; then the four bars its step closed.
            value["bars"] = bars(*(range(0, 300, 60) if stepped else [0]))
        return status, value
    stub.respond = respond
    strategy = Recorder()
    setattr(strategy, hook, lambda *_: strategy.client.step_replay("09:34"))
    runner = Runner(Client(stub.url).for_replay(), strategy, symbols=("SPX",), poll_interval=0.001, use_events=False)
    asyncio.run(runner.run(max_ticks=1))
    assert stepped
    assert [item[2]["t"] for item in strategy.calls if item[0] == "bar"] == [0, 60, 120, 180]


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
