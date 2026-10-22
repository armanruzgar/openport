# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Every documented operation must have an exercised client entry point."""
import re

from openport import Client
from conftest import SPEC

# Aliases have the same semantics; clients intentionally use the canonical route.
ALIASES = {"/api/underlyings/{symbol}": "/api/underlyings/{symbol}/summary"}
EXCLUSIONS = {
    ("GET", "/"): "Static terminal, not the JSON client",
    ("GET", "/{path}"): "Static assets, not the JSON client",
    ("GET", "/ws/events"): "openport.stream.events owns typed WebSocket events",
    ("GET", "/ws"): "openport.stream owns WebSocket transport",
}

# Arguments are synthetic. Calls run through the public methods into _request.
CALLS = [
    ("signal", "{id}"), ("send_signal", "{playbook}", "opsig_test"), ("issue_signal_secret", "{id}"),
    ("signal_secret", "{id}"), ("revoke_signal_secret", "{id}"),
    ("start_sweep", "test", "practice", {}), ("list_sweeps",),
    ("get_sweep", "{id}"), ("cancel_sweep", "{id}"), ("purge_sweep", "{id}"),
    ("status",), ("summary", "{symbol}"), ("chain", "{symbol}"), ("exposure", "{symbol}"),
    ("surface", "{symbol}"), ("volatility", "{symbol}"), ("candles", "{symbol}"), ("probability", "{symbol}"),
    ("account",), ("portfolio",), ("orders",), ("get_order", "{id}"), ("fills",), ("trades",), ("risk",),
    ("risk_profile",), ("plans",), ("accounts",), ("equity",), ("settlements",),
    ("export_csv", "fills"), ("export_csv", "trades"),
    ("place_order", {}), ("preview_order", {}), ("preview_change", "{id}"), ("what_if", []),
    ("modify_order", "{id}"), ("cancel_order", "{id}"), ("cancel_all",), ("flatten",), ("preview_flatten",),
    ("note", "{id}"), ("day_note", "{day}"), ("group_trades", []), ("ungroup_trades", []),
    ("create_account", "test"), ("rename_account", "{id}", "test"), ("delete_account", "{id}"),
    ("reset_account", "test"), ("payout", "1.00"), ("limits", "1", {}), ("guardrails", "1", {}),
    ("kill", "on", "test"), ("settle", "test", "1.00"), ("exercise", "test", 1), ("abandon", "test"),
    ("exercise_instruction", "test"), ("trade_stock", "SPY", "buy", 1), ("preview_stock", "SPY", "buy", 1),
    ("close_stock", "SPY"), ("alerts",), ("create_alert", "account", "equity", "at_or_below", "1"), ("delete_alert", "{id}"),
    ("playbooks",), ("get_playbook", "{id}"), ("create_playbook", {}), ("version_playbook", {"id": "{id}"}),
    ("archive_playbook", "{id}", 1), ("playbook_mode", "{id}", "stage"),
    ("send_staged_order", "{stage}"), ("dismiss_staged_order", "{stage}"),
    ("pass_odds",), ("strategy_template", "SPX", "expiry", {}), ("calendar_events",),
    ("test_notification", "channel"), ("configure_notification", "{channel}"), ("create_sandbox",),
    ("list_backtests",), ("start_backtest", "test", "practice"), ("get_backtest", "{id}"),
    ("compare_backtests", ["1", "2"]), ("keep_backtest", "{id}"), ("cancel_backtest", "{id}"), ("cancel_backtest",),
    ("list_replays",), ("start_replay",), ("control_replay",), ("stop_replay",), ("replay_history",),
    ("get_replay", "{run_id}"), ("delete_replay", "{run_id}"), ("verify_replay", "{run_id}"),
    ("replay_verification", "{run_id}"), ("reload_tokens",),
    ("scenario_packs",), ("scenario_pack", "{id}"), ("import_scenario_pack", {}),
    ("transfer_cash", "test", "1.00"), ("halt_underlying", "SPY", 5),
    ("risk_history",), ("risk_scenarios",), ("optimize_strategies", "{symbol}", 5000, 1000), ("hedges", "delta"),
    ("volatility_history", "{symbol}"), ("surface_history", "{symbol}"), ("volatility_scanner",),
    ("heston", "{symbol}"), ("flow", "{symbol}"), ("earnings",), ("import_earnings", ""), ("delete_imported_earnings",),
    ("analysis_portfolios",), ("import_portfolio", "test", ""), ("analysis_portfolio", "{id}"),
    ("delete_analysis_portfolio", "{id}"), ("analysis_risk", "{id}", "profile"), ("analysis_risk", "{id}", "grid"),
    ("analysis_risk", "{id}", "stress"), ("analysis_risk", "{id}", "var"),
]


def operations():
    for path, item in SPEC["paths"].items():
        while "$ref" in item:
            reference = item["$ref"]
            item = SPEC
            for part in reference.split("/")[1:]:
                item = item[part.replace("~1", "/").replace("~0", "~")]
        for method, operation in item.items():
            if method in {"get", "post", "put", "patch", "delete"}:
                yield method.upper(), path, operation.get("operationId")


def test_every_openapi_operation_has_an_exercised_client_method(monkeypatch):
    covered = set()
    def capture(self, method, path, body=None, *, control=False, **kwargs):
        from urllib.parse import unquote
        covered.add((method, unquote(("/api" if control else self._prefix) + path)))
        return {}
    monkeypatch.setattr(Client, "_request", capture)
    client = Client()
    for target in (client, client.for_replay(), client.for_history("{run_id}")):
        for name, *args in CALLS:
            if name in {"signal", "send_signal"} and target._prefix != "/api":
                continue
            getattr(target, name)(*args)
    documented = {(m, p) for m, p, _ in operations()}
    assert set(EXCLUSIONS) <= documented, "Remove stale exclusions"
    missing = [(m, p, op) for m, p, op in operations()
               if (m, p) not in EXCLUSIONS and (m, ALIASES.get(p, p)) not in covered
               and (m, re.sub(r"/underlyings/\{symbol\}$", "/underlyings/{symbol}/summary", p)) not in covered]
    assert not missing, f"Add client methods and CALLS (or an explicit exclusion): {missing}"


def test_parity_methods_scope_encode_and_do_not_retry_staging(monkeypatch):
    calls = []
    def capture(self, *args, **kwargs):
        calls.append((self._prefix, args, kwargs))
        return {}
    monkeypatch.setattr(Client, "_request", capture)
    client = Client(account="practice").for_replay()
    client.send_staged_order("a/b")
    assert calls[-1] == ("/api/replay", ("POST", "/playbooks/staged/a%2Fb/send", {}), {"scoped": True, "retry": False})
    client.calendar_events("2026-10-01", "2026-10-31")
    assert calls[-1][2] == {"params": {"from": "2026-10-01", "to": "2026-10-31"}, "control": True}
    client.strategy_template("SPX", "expiry", {"kind": "vertical"})
    assert '"kind": "vertical"' in calls[-1][2]["params"]["template"]


def test_webhook_signal_signs_timestamp_method_target_and_body_without_bearer(stub):
    import json
    from openport import ApiError, sign_signal
    import pytest
    client = Client(stub.url, "api-token", "practice")
    # The HTTP stub does not evaluate playbooks; inspect its received signed request.
    with pytest.raises(ApiError):
        client.send_signal("morning", "opsig_secret", timestamp=1234, nonce="fixed-nonce")
    method, target, headers, body = stub.requests[-1]
    raw = json.dumps(body, allow_nan=False).encode()
    assert "Authorization" not in headers
    assert headers["X-Openport-Timestamp"] == "1234"
    assert headers["X-Openport-Signature"] == "sha256=" + sign_signal("opsig_secret", 1234, method, target, raw)
    assert target == "/api/signals/morning?account=practice"
    assert body == {"action": "evaluate", "nonce": "fixed-nonce"}
    # Independent check against the documented message layout.
    import hashlib
    import hmac
    expected = hmac.new(b"opsig_secret", b"1234\nPOST\n" + target.encode() + b"\n" + raw, hashlib.sha256).hexdigest()
    assert headers["X-Openport-Signature"] == "sha256=" + expected
    with pytest.raises(ValueError, match="live"):
        client.for_replay().send_signal("morning", "opsig_secret")


def test_bearer_signal_and_secret_management_use_their_own_routes(stub):
    from openport import ApiError
    import pytest
    client = Client(stub.url, "api-token", "practice")
    for call, expected in [(lambda: client.signal("morning"), ("POST", "/api/playbooks/morning/evaluate?account=practice")),
                           (lambda: client.for_replay("practice").issue_signal_secret("morning"),
                            ("POST", "/api/playbooks/morning/signal-secret?account=practice")),
                           (lambda: client.revoke_signal_secret("morning"),
                            ("DELETE", "/api/playbooks/morning/signal-secret?account=practice")),
                           (lambda: client.purge_sweep("000001"), ("DELETE", "/api/backtests/sweeps/000001?purge=true"))]:
        try:
            call()
        except ApiError:
            pass  # The stub does not implement every route; the request is what matters.
        method, target, headers, _ = stub.requests[-1]
        assert (method, target) == expected
        assert headers["Authorization"] == "Bearer api-token"
        assert "X-Openport-Signature" not in headers
    with pytest.raises(ValueError, match="live"):
        client.for_replay().signal("morning")
