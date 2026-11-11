# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Small async strategy lifecycle for paper accounts, replay and saved history."""
from __future__ import annotations

import asyncio
import inspect
from urllib.parse import urlsplit, urlunsplit

from .client import Client
from .stream import events


class Strategy:
    """Override any hook, synchronously or asynchronously. All hooks run in order.

    client is assigned before on_start. Preview orders and use stable client order
    IDs in your strategy; the runner never places orders on your behalf.
    """
    client: Client

    def on_start(self):
        pass

    def on_tick(self, tick: dict):
        pass

    def on_bar(self, symbol: str, bar: dict):
        pass

    def on_fill(self, fill: dict):
        pass

    def on_stop(self):
        pass


class Runner:
    def __init__(self, client: Client, strategy: Strategy, *, symbols: tuple[str, ...] = (),
                 interval: str = "1m", poll_interval: float = 1, use_events: bool = True):
        if poll_interval <= 0:
            raise ValueError("poll_interval must be positive")
        self.client = client
        self.strategy = strategy
        self.strategy.client = client
        self.symbols = symbols
        self.interval = interval
        self.poll_interval = poll_interval
        self.use_events = use_events
        self.event_error: Exception | None = None
        self._stop = asyncio.Event()
        self._seen_fills: set[str] = set()
        # Per symbol, the newest bar time not to deliver: closed before the start, or delivered.
        self._bars: dict[str, float] = {}
        self._events: asyncio.Queue = asyncio.Queue(maxsize=256)

    def stop(self):
        """Request a graceful stop after the active hook."""
        self._stop.set()

    async def _hook(self, name, *args):
        result = getattr(self.strategy, name)(*args)
        if inspect.isawaitable(result):
            await result

    async def _read(self, method, *args, **kwargs):
        return await asyncio.to_thread(method, *args, **kwargs)

    async def _candles(self, symbol):
        return (await self._read(self.client.candles, symbol, self.interval))["bars"]

    async def _prime_bars(self, symbol):
        # Bars a newer bar has already closed are history. The newest may still be
        # open, so it is delivered once a bar follows it; before any bar, every bar
        # that closes is new. Bar times are whole seconds, so one second earlier
        # sorts just before the newest.
        bars = await self._candles(symbol)
        if len(bars) > 1:
            return bars[-2]["t"]
        return bars[0]["t"] - 1 if bars else float("-inf")

    async def _fill(self, fill):
        key = str(fill.get("attempt", "")) + ":" + str(fill["id"])
        if key not in self._seen_fills:
            self._seen_fills.add(key)
            await self._hook("on_fill", fill)

    async def _stream(self):
        parts = urlsplit(self.client.base_url)
        url = urlunsplit(("wss" if parts.scheme == "https" else "ws", parts.netloc,
                          parts.path + "/ws/events", "", ""))
        source = "replay" if self.client._prefix == "/api/replay" else "live"
        try:
            async for event in events(url, token=self.client.token, account=self.client.account_id, source=source):
                # Bounded independently from the socket queue. REST catches any gap.
                self._events.put_nowait(event)
        except Exception as error:
            self.event_error = error

    async def _history(self):
        fills = (await self._read(self.client.fills))["fills"]
        equity = await self._read(self.client.equity)
        timeline = [(fill.get("time"), 1, fill) for fill in fills]
        timeline += [(point.get("time"), 0, point) for point in equity.get("samples", [])]
        # Missing timestamps remain missing; do not assign wall time to saved data.
        for _time, kind, row in sorted(timeline, key=lambda item: (item[0] or "", item[1])):
            if self._stop.is_set():
                break
            if kind:
                await self._fill(row)
            else:
                await self._hook("on_tick", {"type": "history_tick", "simulated": True, "equity": row})

    async def run(self, *, max_ticks: int | None = None):
        """Run until stop/cancellation/max_ticks. History runs finish after recorded rows.

        Live/replay starts prime existing fills and bars before on_start, then deliver
        new fills once per run, and each bar that closes after the start once a newer
        bar proves it closed, including bars a hook's replay step closed before the
        first poll. No missing bars or quotes are synthesized. A paused replay
        produces no new bars.
        """
        if max_ticks is not None and max_ticks < 1:
            raise ValueError("max_ticks must be positive")
        stream = None
        history = self.client._prefix.startswith("/api/replay/history/")
        try:
            if not history:
                self._seen_fills = {str(fill.get("attempt", "")) + ":" + str(fill["id"]) for fill in (await self._read(self.client.fills))["fills"]}
                for symbol in self.symbols:
                    self._bars[symbol] = await self._prime_bars(symbol)
                if self.use_events:
                    stream = asyncio.create_task(self._stream())
            await self._hook("on_start")
            if history:
                await self._history()
                return
            count = 0
            while not self._stop.is_set():
                tick = await self._read(self.client.status)
                await self._hook("on_tick", tick)
                # Reconcile full REST fills, preserving quote evidence and avoiding
                # duplicate callbacks when a socket event and a poll see the same fill.
                while not self._events.empty():
                    self._events.get_nowait()
                for fill in reversed((await self._read(self.client.fills))["fills"]):
                    await self._fill(fill)
                for symbol in self.symbols:
                    for bar in (await self._candles(symbol))[:-1]:
                        if bar["t"] > self._bars[symbol]:
                            self._bars[symbol] = bar["t"]
                            await self._hook("on_bar", symbol, bar)
                count += 1
                if max_ticks is not None and count >= max_ticks:
                    break
                # A socket event wakes reconciliation immediately, polling is the fallback.
                waiters = [asyncio.create_task(self._events.get()), asyncio.create_task(self._stop.wait())]
                try:
                    await asyncio.wait(waiters, timeout=self.poll_interval, return_when=asyncio.FIRST_COMPLETED)
                finally:
                    for waiter in waiters:
                        waiter.cancel()
                    await asyncio.gather(*waiters, return_exceptions=True)
        finally:
            if stream:
                stream.cancel()
                await asyncio.gather(stream, return_exceptions=True)
            await self._hook("on_stop")


async def run(strategy: Strategy, client: Client, *, max_ticks: int | None = None, **settings):
    """Convenience entry point; asyncio.run(run(MyStrategy(), client))."""
    await Runner(client, strategy, **settings).run(max_ticks=max_ticks)
