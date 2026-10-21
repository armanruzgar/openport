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
            issued = client.issue_signal_secret("morning")
            secret = issued["secret"]
            assert secret.startswith("opsig_") and client.signal_secret("morning")["configured"] is True
            stored = (tmp_path / "signal-secrets.json").read_text()
            assert secret not in stored and oct((tmp_path / "signal-secrets.json").stat().st_mode & 0o777) == "0o600"
            # Signatures from before process startup are refused; wait past it.
            time.sleep(1.1)
            stamp = int(time.time())
            with pytest.raises(ApiError) as off:
                client.send_signal("morning", secret, timestamp=stamp, nonce="integration")
            assert off.value.code == "INVALID_PLAYBOOK" and "off" in str(off.value)
            with pytest.raises(ApiError) as duplicate:
                client.send_signal("morning", secret, timestamp=stamp, nonce="integration")
            assert duplicate.value.code == "SIGNAL_REPLAY"
            with pytest.raises(ApiError) as ahead:  # clocks may run ahead by up to 300 seconds
                client.send_signal("morning", secret, timestamp=stamp + 250, nonce="ahead")
            assert ahead.value.code == "INVALID_PLAYBOOK"
            with pytest.raises(ApiError) as wrong:
                client.send_signal("morning", token, nonce="api-token-is-not-a-signal-secret")
            assert wrong.value.code == "INVALID_SIGNAL_SIGNATURE"
            with pytest.raises(ApiError) as bearer:
                client.signal("morning")
            assert bearer.value.code == "INVALID_PLAYBOOK"
            # A TradingView-style static body works from loopback and still dedupes.
            import json
            from urllib.request import Request, urlopen
            from urllib.error import HTTPError
            static = json.dumps({"action": "evaluate", "nonce": "SPX-2026-10-21T14:30:00Z", "secret": secret}).encode()
            url = f"http://127.0.0.1:{port}/api/signals/morning?account=main"
            codes = []
            for _ in range(2):
                try:
                    urlopen(Request(url, static, method="POST", headers={"Content-Type": "application/json"}), timeout=10)
                except HTTPError as error:
                    codes.append(json.loads(error.read())["error"]["code"])
            assert codes == ["INVALID_PLAYBOOK", "SIGNAL_REPLAY"]
            client.revoke_signal_secret("morning")
            with pytest.raises(ApiError) as revoked:
                client.send_signal("morning", secret, nonce="revoked")
            assert revoked.value.code == "INVALID_SIGNAL_SIGNATURE"
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
