# Copyright (c) 2026 OpenPort contributors. MIT License.
import copy

import jsonschema
import pytest
from conftest import SPEC
from tools.contract_test import Contract, ContractMismatch


def test_schema_references_and_required_types():
    for schema in SPEC["components"]["schemas"].values():
        jsonschema.Draft202012Validator.check_schema(schema)
    def walk(value):
        if isinstance(value, dict):
            if "$ref" in value:
                Contract("", None, SPEC).resolve(value)
            for item in value.values(): walk(item)
        elif isinstance(value, list):
            for item in value: walk(item)
    walk(SPEC)


def test_signed_volatility_history_and_tracking_contract():
    contract = Contract("", None, SPEC)
    validator = jsonschema.Draft202012Validator(
        {"$ref": "urn:openport#/components/schemas/HistoryRank"}, registry=contract.registry)
    validator.validate({"current": -3, "rank": -0.5, "percentile": 0, "z_score": -2, "sessions": 5, "window": 252})
    validator.validate({"current": None, "rank": None, "percentile": None, "z_score": None, "sessions": 0, "window": 252})
    with pytest.raises(jsonschema.ValidationError):
        validator.validate({"current": -3, "rank": "missing", "percentile": 0, "z_score": -2, "sessions": 5, "window": 252})
    assert "/api/underlyings/{symbol}/series" in SPEC["paths"]


def test_guardrail_input_rejects_100_but_stored_settings_remain_readable():
    contract = Contract("", None, SPEC)
    def validator(name):
        return jsonschema.Draft202012Validator(
            {"$ref": f"urn:openport#/components/schemas/{name}"}, registry=contract.registry)
    guardrails = {"soft_floor": "0.00", "soft_floor_percent": 99, "max_opening_trades": 1,
                  "cooldown_loss": "0.00", "cooldown_minutes": 5, "profit_lock": "0.00"}
    validator("GuardrailsInput").validate(guardrails)
    historical = {**guardrails, "soft_floor_percent": 100}
    validator("Guardrails").validate(historical)
    with pytest.raises(jsonschema.ValidationError):
        validator("GuardrailsInput").validate(historical)
    for percent in (-1, 100.5):
        with pytest.raises(jsonschema.ValidationError):
            validator("GuardrailsInput").validate({**guardrails, "soft_floor_percent": percent})


def test_what_if_orders_match_submissions_including_chains():
    schemas = SPEC["components"]["schemas"]
    new = schemas["NewOrder"]
    what = schemas["WhatIfOrder"]
    assert what["properties"] == new["properties"]
    assert what["required"] == [key for key in new["required"] if key != "client_order_id"]
    assert what["oneOf"] == new["oneOf"]
    assert what["allOf"] == new["allOf"]
    contract = Contract("", None, SPEC)
    validator = jsonschema.Draft202012Validator(
        {"$ref": "urn:openport#/components/schemas/WhatIfOrder"}, registry=contract.registry)
    order = {"symbol": "SPXW  261022C05000000", "side": "sell", "type": "limit", "quantity": 1,
             "time_in_force": "gtc", "limit_price": "5.00", "exits_only": True,
             "bracket": {"take_profit": {"limit_price": "5.00"}}}
    validator.validate(order)
    for tif in ("exto", "gtc_exto", "gtd"):
        extended = {**order, "time_in_force": tif}
        if tif == "gtd":
            extended["good_till"] = "2026-09-23T19:30:00Z"
        validator.validate(extended)
    child = {"symbol": order["symbol"], "side": "buy", "type": "market", "quantity": 1, "time_in_force": "ioc"}
    for key in ("then", "oco"):
        parent = {key: value for key, value in order.items() if key not in ("exits_only", "bracket")}
        validator.validate({**parent, key: child})
    for key in ("then", "oco", "typo"):
        with pytest.raises(jsonschema.ValidationError):
            validator.validate({**order, key: None})


def test_entire_contract_against_threaded_synthetic_http_server(stub):
    contract = Contract(stub.url, "secret", SPEC)
    contract.run()
    expected = {(path, "get") for path, item in SPEC["paths"].items()
                if path.startswith("/api/") and "get" in contract.resolve(item)}
    assert expected <= contract.checked
    assert {("/api/underlyings/{symbol}/series", "get"), ("/api/replay/underlyings/{symbol}/series", "get")} <= contract.checked


def test_first_mismatch_names_route_and_field(stub):
    contract = Contract(stub.url, None, SPEC)
    bad = copy.deepcopy(stub.chain)
    bad["strikes"][0]["put"]["bid"] = "not a number"
    with pytest.raises(ContractMismatch, match=r"GET /api/underlyings/SPX/chain: strikes.0.put"):
        contract.validate("GET", "/api/underlyings/SPX/chain", 200, bad)
    bad = copy.deepcopy(stub.chain)
    del bad["expiry"]
    with pytest.raises(ContractMismatch, match="expiry"):
        contract.validate("GET", "/api/underlyings/SPX/chain", 200, bad)


def test_refuses_to_replace_an_active_replay(stub):
    stub.replay = {"existing": True}
    # A malformed response also fails before any write.
    with pytest.raises(ContractMismatch):
        Contract(stub.url, "secret", SPEC).run()
    assert all(method == "GET" for method, *_rest in stub.requests)


def test_series_route_describes_metric_history_and_its_errors():
    contract = Contract("", None, SPEC)
    # The shape series_api.cpp writes: per-field values and sources, null gap rows.
    body = {"symbol": "SPX", "interval": "1d", "from": 1726718400, "to": 1758571200, "fields": ["mfiv30", "rv21"],
            "rows": [{"t": 1758571200, "sample_time": 1758570900, "mfiv30": 15.2, "rv21": None,
                      "sources": {"mfiv30": "own", "rv21": None}},
                     {"t": 1758657600, "mfiv30": None, "rv21": 11.0, "sources": {"mfiv30": None, "rv21": "daily_closes"}}]}
    contract.validate("GET", "/api/underlyings/SPX/series?fields=mfiv30,rv21", 200, body)
    contract.validate("GET", "/api/replay/underlyings/SPX/series", 200, {**body, "rows": []})
    with pytest.raises(ContractMismatch, match=r"rows.0.mfiv30"):
        contract.validate("GET", "/api/underlyings/SPX/series", 200, {**body, "rows": [{**body["rows"][0], "mfiv30": "15.2"}]})
    error = lambda code, message: {"error": {"code": code, "message": message, "actual": None, "limit": None, "scope": None}}
    contract.validate("GET", "/api/underlyings/SPX/series?interval=1h", 400, error("INVALID_REQUEST", "interval must be 1m or 1d"))
    contract.validate("GET", "/api/underlyings/XYZ/series", 404, error("NOT_FOUND", "unknown underlying"))
    # A bare message string is not the contract's error shape.
    with pytest.raises(ContractMismatch):
        contract.validate("GET", "/api/underlyings/XYZ/series", 404, {"error": "unknown underlying"})
    assert {("/api/underlyings/{symbol}/series", "get"), ("/api/replay/underlyings/{symbol}/series", "get")} <= contract.checked


def test_a_failed_cleanup_keeps_the_route_that_broke_the_run(stub, monkeypatch, capsys):
    import sys
    from urllib.error import URLError
    from tools import contract_test
    original = Contract.call
    def call(self, method, path, body=None, *, success=False):
        if (method, path) == ("POST", "/api/replay/orders?account=main"):
            raise URLError("orders route crashed the server")
        if (method, path) == ("DELETE", "/api/replay"):
            raise URLError("Connection refused")
        return original(self, method, path, body, success=success)
    monkeypatch.setattr(Contract, "call", call)
    with pytest.raises(URLError, match="orders route crashed the server") as caught:
        Contract(stub.url, "secret", SPEC).run()
    assert caught.value.__notes__ == ["cleanup DELETE /api/replay also failed: <urlopen error Connection refused>"]
    stub.replay = None  # The failed DELETE left the first run's replay up.
    monkeypatch.setattr(sys, "argv", ["contract_test.py", stub.url, "--token", "secret"])
    assert contract_test.main() == 1
    lines = capsys.readouterr().err.splitlines()
    assert lines == ["contract: <urlopen error orders route crashed the server>",
                     "contract: cleanup DELETE /api/replay also failed: <urlopen error Connection refused>"]


def test_real_cpp_handler_responses_without_network(tmp_path):
    import json
    import os
    import subprocess
    from conftest import ROOT
    binary = ROOT / "build/tests/openport_tests"
    if not binary.exists():
        pytest.skip("Build the C++ test executable to check its real handler responses")
    result = subprocess.run([str(binary), "--gtest_filter=Api.ContractFixture:StrategySearch.NetworkRequestsCompleteFromThePool:PaperEngine.RiskProfileLegsAndHedges:Api.EventVolatilityReadsCurrentAccountCalendarsAndDeduplicatesImports:SurfaceHistory.ComparisonsUseExactTargetSessionsAndNeverFutureCloses:PaperEngine.ContractFixture:PaperEngine.SizeScalingAccountViewReportsTheCapitalCreditAndNewReview:PaperEngine.TwoStepProgressionAndProgramCosts:PaperEngine.ProgramCostOverridesAndCustomVerification:PaperEngine.DirectionRulesRoundTripAndHedgingPreviewEvidence:PaperEngine.VolumeRuleRoundTripsAndExplainsUnknownVolumeInPreview:PaperEngine.VolumeFillCancellationReportsRuleEvidenceAndPreservesEarlierFills:ReplayRun.ContractFixture:BacktestApi.ContractFixture:Backtest.AttemptsCarryBalancesDecidePassAndFailureAndLeaveOpenTail:PlaybookApi.LiveForwardPublicationIncludesWindowsAndJournalStats:BacktestApi.ListingSummarizesSavedReportIdentityWithoutCopyingTrades:BacktestApi.ComparisonAcceptsLegacyReportsAndRejectsInvalidSelection:Backtest.JointOpenTradeAttributionUsesOptionMarksAndFees:BacktestApi.JointPostRunsPinnedDefinitionsAndListsEveryMember:PaperEngine.QueueFillsAndStopStressRulesRoundTripAndLeaveThePreset:PaperEngine.FillQualityAndRealFillCalibrationReadWithoutTrading"],
                            env={**os.environ, "OPENPORT_CONTRACT_OUTPUT": str(tmp_path)},
                            capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    files = list(tmp_path.glob("*.jsonl"))
    assert {file.name for file in files} == {
        "analytics.jsonl", "paper.jsonl", "replay.jsonl", "backtests.jsonl", "playbooks.jsonl",
        "time-rules.jsonl", "direction-rules.jsonl", "volume-rule.jsonl",
    }, "Rebuild C++ tests with all contract fixtures, including scaling resets, direction and volume"
    contract = Contract("", None, SPEC)
    for file in files:
        for line in file.read_text().splitlines():
            row = json.loads(line)
            contract.validate(**row)
    assert len(contract.checked) >= 30


def test_cli_token_validation_never_starts_a_provider_or_logs_secrets(tmp_path):
    import os
    import subprocess
    from conftest import ROOT
    binary = ROOT / "build/apps/openportd"
    if not binary.exists():
        pytest.skip("Build openportd to check argument validation")
    env = {key: value for key, value in os.environ.items() if key != "OPENPORT_WRITE_TOKEN"}
    result = subprocess.run([str(binary), "--require-token"], env=env, capture_output=True, text=True, timeout=5)
    assert result.returncode != 0
    assert "--require-token needs a configured token" in result.stderr
    token_file = tmp_path / "tokens"
    token_file.write_text("agent invalid do-not-print-this-secret\n")
    result = subprocess.run([str(binary), "--token-file", str(token_file)], env=env, capture_output=True, text=True, timeout=5)
    assert result.returncode != 0
    assert "Invalid token file at line 1" in result.stderr
    assert "do-not-print-this-secret" not in result.stderr
    token_file.write_text("agent read,trade:main,replay do-not-print-this-secret\n")
    result = subprocess.run([str(binary), "--token-file", str(token_file), "--require-token", "--help"], env=env, capture_output=True, text=True, timeout=5)
    # Help is an answer: stdout and status 0.
    assert result.returncode == 0
    assert "usage: openportd" in result.stdout
    assert "unknown option" not in result.stderr
    assert "do-not-print-this-secret" not in result.stdout + result.stderr


def test_archived_run_without_metadata_is_described(stub):
    listing = stub.respond("GET", "/api/replay", {}, None)[1]
    listing["history"] = [{"id": "old", "file": "old", "demo": False, "result": "open", "pnl": None,
                           "finished": True, "read_only": True, "error": "synthetic damaged journal"}]
    Contract("", None, SPEC).validate("GET", "/api/replay", 200, listing)


def test_real_share_trade_and_preview_responses(tmp_path):
    import json
    import os
    import subprocess
    from conftest import ROOT
    binary = ROOT / "build/tests/openport_tests"
    if not binary.exists():
        pytest.skip("Build the C++ test executable for share response contracts")
    result = subprocess.run([str(binary), "--gtest_filter=PaperStocks.SharesOpenThroughTheApiAtTheUnderlyingsPrice"],
                            env={**os.environ, "OPENPORT_CONTRACT_OUTPUT": str(tmp_path)},
                            capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    files = list(tmp_path.glob("*.jsonl"))
    assert files, "Rebuild C++ tests with the share fixture"
    contract = Contract("", None, SPEC)
    for file in files:
        for line in file.read_text().splitlines():
            contract.validate(**json.loads(line))


def test_joint_backtest_request_contract():
    contract = Contract("", None, SPEC)
    validator = jsonschema.Draft202012Validator(
        {"$ref": "urn:openport#/components/schemas/BacktestRequest"}, registry=contract.registry)
    body = {"playbooks": ["alpha@2", "beta"], "plan": "eod-50k", "scenarios": 2, "seed": "1"}
    validator.validate(body)
    for invalid in ({**body, "playbook": "alpha"}, {**body, "playbooks": ["alpha"]},
                    {**body, "playbooks": ["alpha", "alpha"]}, {**body, "playbooks": list("abcdefghi")},
                    {key: value for key, value in body.items() if key != "playbooks"}):
        with pytest.raises(jsonschema.ValidationError):
            validator.validate(invalid)


def test_replay_recordings_expose_imported_dates_symbols_and_sources(stub):
    listing = stub.respond("GET", "/api/replay", {}, None)[1]
    listing["recordings"] = [
        {"file": f"{provider}-2026-09-22-imported.oprec", "bytes": 512,
         "provider": provider, "date": "2026-09-22", "symbols": ["SPX", "SPY"],
         "started": "2026-09-22T00:15:00.000Z", "imported": True, "simulated": False}
        for provider in ("massive", "thetadata", "databento")
    ]
    contract = Contract("", None, SPEC)
    contract.validate("GET", "/api/replay", 200, listing)
    listing["recordings"][0]["date"] = 20260922
    with pytest.raises(ContractMismatch, match="recordings.0.date"):
        contract.validate("GET", "/api/replay", 200, listing)


def test_programmability_cpp_responses_match_contract(tmp_path):
    import json
    import os
    import subprocess
    from conftest import ROOT
    binary = ROOT / "build/tests/openport_tests"
    if not binary.exists():
        pytest.skip("Build C++ tests for signed signal and sweep response contracts")
    result = subprocess.run([str(binary), "--gtest_filter=PlaybookApi.SignedEvaluationRespectsOffStageAndAutoWithoutChangingMode:BacktestApi.SweepUsesSeparateRoutesAndPreservesNormalListing"],
                            env={**os.environ, "OPENPORT_CONTRACT_OUTPUT": str(tmp_path)},
                            capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    assert {file.name for file in tmp_path.glob("*.jsonl")} == {"signals.jsonl", "sweeps.jsonl"}
    contract = Contract("", None, SPEC)
    for file in tmp_path.glob("*.jsonl"):
        for line in file.read_text().splitlines():
            contract.validate(**json.loads(line))


def test_signal_and_sweep_request_schemas():
    contract = Contract("", None, SPEC)
    def validate(name, body):
        jsonschema.Draft202012Validator({"$ref": f"urn:openport#/components/schemas/{name}"}, registry=contract.registry).validate(body)
    validate("SignalRequest", {"action": "evaluate", "nonce": "test-1"})
    # TradingView-style static body: the dedupe ID uses placeholders such as {{ticker}}-{{timenow}}.
    validate("SignalRequest", {"action": "evaluate", "nonce": "SPY-2026-10-21T14:30:00Z", "secret": "opsig_" + "0" * 64})
    for body in [{"action": "send", "nonce": "test-1"},
                 {"action": "evaluate", "timestamp": 12345, "nonce": "test-1"},
                 {"action": "evaluate", "nonce": "../unsafe"},
                 {"action": "evaluate", "secret": "opsig_" + "0" * 64}]:
        with pytest.raises(jsonschema.ValidationError): validate("SignalRequest", body)
    validate("SignalSecretIssued", {"playbook": "morning", "account": "main", "secret": "opsig_" + "a" * 64,
                                    "created_at": "2026-10-21T14:30:00.000000000Z", "rotated": False})
    with pytest.raises(jsonschema.ValidationError):
        validate("SignalSecretStatus", {"playbook": "morning", "account": "main", "configured": True,
                                        "created_at": None, "secret": "opsig_" + "a" * 64})
    body = {"playbook": "morning", "plan": "practice", "scenarios": 2, "seed": "1",
            "parameter_grid": {"/structure/template/width": [5, 10]}}
    validate("BacktestSweepRequest", body)
    validate("BacktestSweepState", {"id": "000001", "kind": "sweep", "status": "running", "phase": "variants",
             "completed": 2, "total": 4, "report": None,
             "label": "Simulated trading on recorded or generated days. Not a prediction. Not investment advice."})
    for bad in ({**body, "parameter_grid": {}}, {**body, "playbooks": ["morning", "afternoon"]},
                {**body, "parameter_grid": {"/structure/template/width": [5, 5]}}):
        with pytest.raises(jsonschema.ValidationError): validate("BacktestSweepRequest", bad)


def test_practice_contract_rejects_unknown_mistakes_and_invalid_objectives():
    contract = Contract("", None, SPEC)
    def validator(name):
        return jsonschema.Draft202012Validator(
            {"$ref": f"urn:openport#/components/schemas/{name}"}, registry=contract.registry)
    validator("TradeNote").validate({"note": "", "tags": [], "mistakes": ["chased", "A+ setup"]})
    with pytest.raises(jsonschema.ValidationError):
        validator("TradeNote").validate({"note": "", "tags": [], "mistakes": ["anything"]})
    validator("ScenarioObjective").validate({"id": "flat", "type": "flat_by", "at": "15:45"})
    validator("ScenarioObjective").validate({"id": "room", "type": "min_floor_room", "limit": 25})
    for bad in ({"id": "room", "type": "min_floor_room", "limit": 101},
                {"id": "flat", "type": "flat_by", "at": "15:45", "limit": 1},
                {"id": "vega", "type": "max_net_vega", "limit": 10}):
        with pytest.raises(jsonschema.ValidationError):
            validator("ScenarioObjective").validate(bad)
