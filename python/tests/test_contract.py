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


def test_what_if_orders_match_submissions_without_chains():
    schemas = SPEC["components"]["schemas"]
    new = schemas["NewOrder"]
    what = schemas["WhatIfOrder"]
    assert what["properties"] == {key: value for key, value in new["properties"].items() if key not in ("then", "oco")}
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
    for key in ("then", "oco", "typo"):
        with pytest.raises(jsonschema.ValidationError):
            validator.validate({**order, key: None})


def test_entire_contract_against_threaded_synthetic_http_server(stub):
    contract = Contract(stub.url, "secret", SPEC)
    contract.run()
    expected = {(path, "get") for path, item in SPEC["paths"].items()
                if path.startswith("/api/") and "get" in contract.resolve(item)}
    assert expected <= contract.checked


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


def test_real_cpp_handler_responses_without_network(tmp_path):
    import json
    import os
    import subprocess
    from conftest import ROOT
    binary = ROOT / "build/tests/openport_tests"
    if not binary.exists():
        pytest.skip("Build the C++ test executable to check its real handler responses")
    result = subprocess.run([str(binary), "--gtest_filter=Api.ContractFixture:PaperEngine.ContractFixture:PaperEngine.SizeScalingAccountViewReportsTheCapitalCreditAndNewReview:PaperEngine.TwoStepProgressionAndProgramCosts:PaperEngine.ProgramCostOverridesAndCustomVerification:PaperEngine.DirectionRulesRoundTripAndHedgingPreviewEvidence:PaperEngine.VolumeRuleRoundTripsAndExplainsUnknownVolumeInPreview:PaperEngine.VolumeFillCancellationReportsRuleEvidenceAndPreservesEarlierFills:ReplayRun.ContractFixture:BacktestApi.ContractFixture:Backtest.AttemptsCarryBalancesDecidePassAndFailureAndLeaveOpenTail:PlaybookApi.LiveForwardPublicationIncludesWindowsAndJournalStats:BacktestApi.ListingSummarizesSavedReportIdentityWithoutCopyingTrades:BacktestApi.ComparisonAcceptsLegacyReportsAndRejectsInvalidSelection:Backtest.JointOpenTradeAttributionUsesOptionMarksAndFees:BacktestApi.JointPostRunsPinnedDefinitionsAndListsEveryMember"],
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
    validate("SignalRequest", {"action": "evaluate", "timestamp": 12345, "nonce": "test-1"})
    for body in [{"action": "send", "timestamp": 12345, "nonce": "test-1"},
                 {"action": "evaluate", "timestamp": True, "nonce": "test-1"},
                 {"action": "evaluate", "timestamp": 12345, "nonce": "../unsafe"}]:
        with pytest.raises(jsonschema.ValidationError): validate("SignalRequest", body)
    body = {"playbook": "morning", "plan": "practice", "scenarios": 2, "seed": "1",
            "parameter_grid": {"/structure/template/width": [5, 10]}}
    validate("BacktestSweepRequest", body)
    validate("BacktestSweepState", {"id": "000001", "kind": "sweep", "status": "running", "phase": "variants",
             "completed": 2, "total": 4, "report": None,
             "label": "Simulated trading on recorded or generated days. Not a prediction. Not investment advice."})
    for bad in ({**body, "parameter_grid": {}}, {**body, "playbooks": ["morning", "afternoon"]},
                {**body, "parameter_grid": {"/structure/template/width": [5, 5]}}):
        with pytest.raises(jsonschema.ValidationError): validate("BacktestSweepRequest", bad)
