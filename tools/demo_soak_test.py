"""Offline tests for demo_soak.py against a scripted replay API; no server runs."""
from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import tempfile
from unittest.mock import patch
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import demo_soak  # noqa: E402

SHORT_PUT = "SPXW  260918P05900000"


def leg(symbol, delta, bid=1.00, ask=1.20):
    return {"symbol": symbol, "delta": delta, "bid": bid, "ask": ask}


class FakeReplay:
    """The replay routes the soak calls. The day ends after `snapshots` pauses; the
    probe's quote and what an order does can be scripted."""

    def __init__(self, snapshots=3, quote=(2.90, 3.10), fills=False, finish_before_probe=False,
                 accepting=True, status=None, refuse=None):
        self.base, self.timeout = "http://fake-replay", 180
        self.snapshots, self.quote, self.fills = snapshots, quote, fills
        self.finish_before_probe = finish_before_probe
        self.accepting, self.status, self.refuse = accepting, status, refuse or {}
        self.paused, self.polls, self.finished = False, 0, False
        self.started, self.orders, self.cancelled = None, [], []
        self.run_id = "run-123"

    def chain(self):
        bid, ask = self.quote
        return {"strikes": [
            {"strike": 5900, "put": leg(SHORT_PUT, -0.10, bid, ask), "call": leg("SPXW  260918C06200000", 0.10)},
            {"strike": 5800, "put": leg("SPXW  260918P05800000", -0.03), "call": leg("SPXW  260918C06300000", 0.03)}]}

    def call(self, method, path, body=None):
        if path == "/api/replay" and method == "DELETE":
            return 200, {"replay": None}
        if path == "/api/replay" and method == "POST":
            self.started = body
            return 201, {"replay": {"id": self.run_id, "scenario": body["demo"], "seed": body.get("seed", "123")}}
        if path == "/api/replay" and method == "PUT":
            if "paused" in body:
                self.paused = body["paused"]
                if self.paused:
                    self.polls += 1
                    self.finished = self.polls > self.snapshots
            return 200, {"replay": {}}
        if path == "/api/replay" and method == "GET":
            return 200, {"replay": {"finished": self.finished}}
        if path == "/api/replay/status":
            if self.status:
                return self.status
            return 200, {"underlyings": [{"symbol": "SPX", "options": 10, "paper": {"accepting": self.accepting}}]}
        if path == "/api/replay/underlyings/SPX/summary":
            return 200, {"expiries": [{"id": "2026-09-17PM"}, {"id": "2026-09-18PM"}]}
        if path.startswith("/api/replay/underlyings/SPX/chain"):
            return 200, self.chain()
        if path == "/api/replay/risk":
            return 200, {"limits": {"price_band_absolute": "0.50", "price_band_relative": 0.2}}
        if path == "/api/replay/portfolio":
            held = {order["symbol"]: 0 for order in self.orders}
            for order in self.orders:
                if order["filled_quantity"]:
                    held[order["symbol"]] += order["filled_quantity"] * (1 if order["side"] == "buy" else -1)
            return 200, {"time": "t", "valuation_complete": True, "equity": "100000.00",
                         "positions": [{"symbol": s, "quantity": q, "fresh": True} for s, q in held.items() if q]}
        if path == "/api/replay/orders" and method == "POST":
            if body["client_order_id"] in self.refuse:
                return 422, {"error": {"code": self.refuse[body["client_order_id"]], "message": "synthetic refusal"}}
            if self.finish_before_probe and body["client_order_id"].startswith("probe-"):
                return 403, {"error": {"code": "REPLAY_READ_ONLY", "message": "The replay has finished"}}
            order = dict(body, id=str(len(self.orders) + 1), paused=self.paused, filled_quantity=0, status="working")
            if body["type"] == "market" or (self.fills and body["client_order_id"].startswith("probe-")):
                order.update(status="filled", filled_quantity=body["quantity"])
            self.orders.append(order)
            return 201, {"order": order}
        if path.startswith("/api/replay/orders/") and method == "DELETE":
            order = self.orders[int(path.rsplit("/", 1)[1]) - 1]
            order["status"] = "cancelled"
            self.cancelled.append(order["id"])
            return 200, {"order": order}
        raise AssertionError(f"unexpected {method} {path}")


def run(replay, seed=None):
    lines = []
    result = demo_soak.soak(replay, speed=300, seed=seed, out=lambda *parts: lines.append(" ".join(map(str, parts))), pause=0)
    return result, "\n".join(lines)


class DemoSoakTest(unittest.TestCase):
    def test_http_timeout_covers_generation_and_replay_controls(self):
        for timeout in (None, 420):
            client = demo_soak.Client("http://fake-replay") if timeout is None else demo_soak.Client("http://fake-replay", timeout)
            for method, path, body in (("POST", "/api/replay", {"demo": "reversal"}),
                                       ("PUT", "/api/replay", {"step": True}),
                                       ("PUT", "/api/replay", {"paused": True}),
                                       ("GET", "/api/replay/portfolio", None)):
                with self.subTest(timeout=timeout, method=method, body=body), \
                        patch.object(demo_soak.urllib.request, "urlopen") as urlopen:
                    response = urlopen.return_value.__enter__.return_value
                    response.status, response.read.return_value = 200, b'{"ok": true}'
                    self.assertEqual(client.call(method, path, body), (200, {"ok": True}))
                    request = urlopen.call_args.args[0]
                    self.assertEqual(request.full_url, client.base + path)
                    self.assertEqual(request.method, method)
                    self.assertEqual(urlopen.call_args.kwargs["timeout"], 180 if timeout is None else timeout)

    def test_timeout_option_reaches_http_client(self):
        with patch.object(demo_soak, "Client", return_value=FakeReplay(snapshots=0)) as client, \
                patch.object(demo_soak.time, "sleep"), redirect_stdout(io.StringIO()):
            self.assertEqual(demo_soak.main(["http://fake-replay", "--timeout", "420"]), 0)
        client.assert_called_once_with("http://fake-replay", timeout=420, token=None)

    def test_token_is_sent_as_a_bearer_credential(self):
        with patch.object(demo_soak, "Client", return_value=FakeReplay(snapshots=0)) as client, \
                patch.object(demo_soak.time, "sleep"), redirect_stdout(io.StringIO()):
            self.assertEqual(demo_soak.main(["http://fake-replay", "--token", "s3cret"]), 0)
        client.assert_called_once_with("http://fake-replay", timeout=180, token="s3cret")
        for token, expected in ((None, None), ("s3cret", "Bearer s3cret")):
            with self.subTest(token=token), patch.object(demo_soak.urllib.request, "urlopen") as urlopen:
                response = urlopen.return_value.__enter__.return_value
                response.status, response.read.return_value = 201, b'{"replay": {}}'
                demo_soak.Client("http://fake-replay", token=token).call("POST", "/api/replay", {"demo": "reversal"})
                self.assertEqual(urlopen.call_args.args[0].get_header("Authorization"), expected)

    def test_readiness_wait_gives_up_at_the_timeout(self):
        clock = [0.0]
        def sleep(seconds):
            clock[0] += seconds
        replay = FakeReplay(accepting=False)
        replay.timeout = 5
        with patch.object(demo_soak.time, "sleep", sleep), patch.object(demo_soak.time, "monotonic", lambda: clock[0]), \
                self.assertRaisesRegex(demo_soak.SoakError, "SPX did not accept orders within 5 s"):
            run(replay)
        self.assertLessEqual(clock[0], 5.5)
        self.assertEqual(replay.orders, [])

    def test_an_error_from_replay_status_is_reported_not_a_traceback(self):
        missing = (404, {"error": {"code": "NO_REPLAY", "message": "No replay is running"}})
        stderr = io.StringIO()
        with patch.object(demo_soak, "Client", return_value=FakeReplay(status=missing)), \
                patch.object(demo_soak.time, "sleep"), redirect_stdout(io.StringIO()), redirect_stderr(stderr):
            self.assertEqual(demo_soak.main(["http://fake-replay"]), 2)
        self.assertIn("GET /api/replay/status: HTTP 404", stderr.getvalue())
        self.assertIn("NO_REPLAY", stderr.getvalue())

    def test_a_condor_leg_that_does_not_fill_fails_the_soak_before_probing(self):
        replay = FakeReplay(refuse={"condor-short-put": "BUYING_POWER"})
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.object(demo_soak, "Client", return_value=replay), patch.object(demo_soak.time, "sleep"), \
                redirect_stdout(stdout), redirect_stderr(stderr):
            self.assertEqual(demo_soak.main(["http://fake-replay", "--json"]), 2)
        self.assertIn("the condor did not form: short put (BUYING_POWER) did not fill", stderr.getvalue())
        self.assertEqual(stdout.getvalue(), "")
        self.assertFalse(any(order["client_order_id"].startswith("probe-") for order in replay.orders))

    def test_invalid_timeout_is_rejected_before_starting_replay(self):
        for value in ("0", "-1", "nan", "inf"):
            with self.subTest(value=value), patch.object(demo_soak, "Client") as client, \
                    redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                demo_soak.main(["http://fake-replay", "--timeout", value])
            self.assertEqual(error.exception.code, 2)
            client.assert_not_called()

    def test_json_writer_retains_summary_and_server_identity(self):
        result, _ = run(FakeReplay())
        result["refused"] = {"Z_RULE": 2, "A_RULE": 1}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "soak.json"
            demo_soak.write_json(result, path)
            text = path.read_text()
        self.assertEqual(json.loads(text), result)
        self.assertEqual(text, json.dumps(result, sort_keys=True, indent=2) + "\n")
        self.assertEqual(result["seed"], "123")
        self.assertEqual((result["url"], result["speed"], result["plan"], result["run_id"], result["scenario"]),
                         ("http://fake-replay", 300, "practice", "run-123", "reversal"))
        self.assertEqual(result["expiry"], "2026-09-18PM")
        self.assertEqual(len(result["legs"]), 4)
        self.assertEqual(result["legs"]["short put"], SHORT_PUT)
        self.assertEqual((result["start_market_time"], result["end_market_time"]), ("t", "t"))
        self.assertEqual(result["exit_status"], 0)

    def test_json_stdout_and_progress_stderr(self):
        for option in (["--json"], ["--json", "-"]):
            stdout, stderr = io.StringIO(), io.StringIO()
            with patch.object(demo_soak, "Client", return_value=FakeReplay(snapshots=1, fills=True)), \
                    patch.object(demo_soak.time, "sleep"), redirect_stdout(stdout), redirect_stderr(stderr):
                status = demo_soak.main(["http://fake-replay", "300", "practice", "--seed", "18446744073709551615", *option])
            summary = json.loads(stdout.getvalue())
            self.assertEqual(summary["seed"], "18446744073709551615")
            self.assertEqual((summary["exit_status"], status, summary["probe_fills"]), (1, 1, 1))
            self.assertIn("demo day reversal run run-123 seed 18446744073709551615", stderr.getvalue())
            self.assertIn("probes 1", stderr.getvalue())

    def test_text_output_and_json_file_keep_progress_on_stdout(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "soak.json"
            outputs = []
            for option in ([], ["--json", str(path)]):
                stdout, stderr = io.StringIO(), io.StringIO()
                with patch.object(demo_soak, "Client", return_value=FakeReplay(snapshots=0)), \
                        patch.object(demo_soak.time, "sleep"), redirect_stdout(stdout), redirect_stderr(stderr):
                    self.assertEqual(demo_soak.main(["http://fake-replay", *option]), 0)
                self.assertEqual(stderr.getvalue(), "")
                outputs.append(stdout.getvalue())
            self.assertEqual(outputs[0], outputs[1])
            self.assertIn("portfolio samples 1", outputs[0])
            self.assertEqual(json.loads(path.read_text())["exit_status"], 0)

    def test_probe_price_is_under_the_ask_and_inside_the_band(self):
        for bid, ask, expected in ((2.90, 3.10, 2.90), (3.00, 3.10, 3.00), (3.05, 3.20, 3.00),
                                   (0.10, 0.20, 0.10), (1.00, 3.00, 1.50), (5.00, 5.00, None), (0, 0.05, None)):
            price = demo_soak.probe_price(bid, ask)
            self.assertEqual(price, expected, (bid, ask))
            if price is not None:
                mid = (bid + ask) / 2
                self.assertLess(price, ask)
                self.assertLessEqual(mid - price, max(0.5, 0.2 * mid) + 1e-9)

    def test_probes_go_out_paused_and_leave_the_condor_whole(self):
        replay = FakeReplay()
        result, text = run(replay, seed=81723)
        self.assertEqual(replay.started["seed"], "81723")
        probes = [order for order in replay.orders if order["client_order_id"].startswith("probe-")]
        self.assertEqual(len(probes), 3)
        self.assertTrue(all(order["paused"] and order["limit_price"] == "2.90" for order in probes))
        self.assertEqual(replay.cancelled, [order["id"] for order in probes])
        self.assertEqual((result["probe_fills"], result["refused"]), (0, {}))
        self.assertTrue(result["condor_intact"])
        self.assertIn("seed 81723", text)

    def test_a_filled_probe_is_reported_and_sold_back(self):
        replay = FakeReplay(snapshots=1, fills=True)
        result, text = run(replay)
        self.assertEqual((result["probe_fills"], result["restored"]), (1, 1))
        self.assertTrue(result["condor_intact"])
        self.assertIn("filled 1 (sold back 1)", text)

    def test_a_probe_after_the_day_ends_is_not_a_refusal(self):
        result, _ = run(FakeReplay(finish_before_probe=True))
        self.assertEqual((result["probes"], result["refused"]), (0, {}))
        replay = FakeReplay(snapshots=0)
        result, _ = run(replay)
        self.assertFalse(any(order["client_order_id"].startswith("probe-") for order in replay.orders))


if __name__ == "__main__":
    unittest.main()
