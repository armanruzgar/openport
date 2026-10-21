# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Offline integration: actual daemon, synthetic market, temporary paper journal."""
import asyncio
import os
import socket
import subprocess
import time
from urllib.error import URLError

import pytest
from openport import ApiError, Client, events
from openport.bot import Runner, Strategy
from conftest import ROOT, PLAYBOOK


def test_demo_daemon_signals_stream_and_strategy(tmp_path):
    binary = ROOT / "build/apps/openportd"
    if not binary.exists():
        pytest.skip("Build openportd for offline daemon integration")
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    env = {key: value for key, value in os.environ.items() if not key.startswith("OPENPORT_NOTIFY_")}
    token = "synthetic-integration-secret"
    with (tmp_path / "daemon.log").open("w+") as log:
        process = subprocess.Popen([str(binary), "--provider", "demo", "--symbols", "SPX", "--port", str(port),
                                    "--write-token", token, "--paper-journal", str(tmp_path / "paper.jsonl"),
                                    "--candle-dir", str(tmp_path / "candles"), "--record-dir", str(tmp_path / "recordings"),
                                    "--no-history", "--no-series", "--no-cboe-holidays"],
                                   cwd=tmp_path, env=env, stdout=log, stderr=log)
        try:
            client = Client(f"http://127.0.0.1:{port}", token, "main", timeout=10, retries=0)
            deadline = time.monotonic() + 60
            while True:
                if process.poll() is not None:
                    log.seek(0)
                    pytest.fail(log.read())
                try:
                    status = client.status()
                    break
                except (URLError, ApiError):
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.1)
            assert status["provider"]["simulated"] is True
            definition = {**PLAYBOOK, "structure": {"template": {"kind": "vertical", "type": "put", "direction": "credit",
                          "target": {"mode": "delta", "value": 0.15}, "width": 5}, "expiry": {"min": 0, "max": 1}}}
            client.create_playbook(definition)
            # The nonce guard rejects signatures from before process startup.
            time.sleep(1.1)
            stamp = int(time.time())
            with pytest.raises(ApiError) as off:
                client.signal("morning", timestamp=stamp, nonce="integration")
            assert off.value.code == "INVALID_PLAYBOOK" and "off" in str(off.value)
            with pytest.raises(ApiError) as duplicate:
                client.signal("morning", timestamp=stamp, nonce="integration")
            assert duplicate.value.code == "SIGNAL_REPLAY"
            async def exercise():
                ready = asyncio.Event()
                async def receive():
                    async for event in events(f"ws://127.0.0.1:{port}/ws/events", token=token, account="main", ready=ready):
                        if event.event == "rule_trip":
                            return event
                pending = asyncio.create_task(receive())
                try:
                    await asyncio.wait_for(ready.wait(), 10)
                    await asyncio.to_thread(client.kill, "trip", "synthetic integration test")
                    event = await asyncio.wait_for(pending, 10)
                    assert event.account == "main" and event.source == "live"
                    assert event.simulated is True and event.version == 1
                finally:
                    pending.cancel()
                    await asyncio.gather(pending, return_exceptions=True)
                calls = []
                class Observe(Strategy):
                    def on_start(self): calls.append("start")
                    def on_tick(self, tick):
                        assert tick["provider"]["simulated"] is True
                        calls.append("tick")
                    def on_stop(self): calls.append("stop")
                await Runner(client, Observe(), use_events=False).run(max_ticks=1)
                assert calls == ["start", "tick", "stop"]
            asyncio.run(exercise())
        finally:
            process.terminate()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
