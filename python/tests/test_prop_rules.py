# Copyright (c) 2026 OpenPort contributors. MIT License.
import json
import os
import subprocess
from typing import get_type_hints

import jsonschema
import pytest
from conftest import ROOT, SPEC
from openport import Client
from openport.types import AccountRules, OpenLossCap, PayoutReview, PayoutReviewOutcome, ReviewTrade
from tools.contract_test import Contract


def validator(name):
    return jsonschema.Draft202012Validator({"$ref": "urn:openport#/components/schemas/" + name},
                                           registry=Contract("", None, SPEC).registry)


def test_prop_rule_types_match_the_schemas():
    schemas = SPEC["components"]["schemas"]
    for wire in (OpenLossCap, PayoutReview, PayoutReviewOutcome, ReviewTrade):
        assert set(get_type_hints(wire)) == set(schemas[wire.__name__]["properties"])
    hints = get_type_hints(AccountRules)
    assert hints["open_loss_cap"] == OpenLossCap | None
    for field in ("drawdown_basis", "open_loss_cap", "minute_volume_percent", "best_position_percent"):
        assert field in schemas["AccountRulesInput"]["properties"]
        assert field in AccountRules.__optional_keys__


def test_rule_packs_and_comparisons_validate_against_the_contract():
    pack = {"format": "openport.rule-pack", "version": 1, "id": "desk-2026", "name": "Desk terms",
            "effective_from": "2026-09-01", "source": "https://example.com/terms", "verify": "Checked 2026-09-01",
            "plans": [{"id": "eval", "name": "Desk Eval", "initial_cash": "50000.00",
                       "rules": {"profit_target": "3000", "max_drawdown": "2000", "drawdown_mode": "intraday",
                                 "buy_only": False, "buying_power": True, "expiry_cutoff_seconds": 0}}]}
    validator("RulePack").validate(pack)
    for broken in ({**pack, "format": "other"}, {**pack, "id": "Has Spaces"}, {**pack, "plans": []},
                   {**pack, "extra": True}):
        with pytest.raises(jsonschema.ValidationError):
            validator("RulePack").validate(broken)
    validator("RulesCompareRequest").validate({"plans": ["intraday-100k"], "include_current": False})
    with pytest.raises(jsonschema.ValidationError):
        validator("RulesCompareRequest").validate({"plans": "intraday-100k"})
    cap = {"basis": "drawdown_room", "amount": None, "percent": 30, "action": "block_adds"}
    validator("OpenLossCap").validate(cap)
    with pytest.raises(jsonschema.ValidationError):
        validator("OpenLossCap").validate({**cap, "action": "warn"})


def test_client_sends_rule_comparisons_packs_and_economics(monkeypatch):
    calls = []
    monkeypatch.setattr(Client, "_request", lambda self, *args, **kwargs: calls.append((self._prefix, args, kwargs)) or {})
    client = Client(account="main")
    client.rules_compare(["intraday-100k"], [{"name": "Mine", "initial_cash": "100000", "rules": {}}], include_current=False)
    assert calls[-1][1] == ("POST", "/account/rules-compare", {
        "include_current": False, "plans": ["intraday-100k"], "rules": [{"name": "Mine", "initial_cash": "100000", "rules": {}}]})
    assert calls[-1][2] == {"scoped": True, "control": True}
    client.validate_rule_pack({"format": "openport.rule-pack"})
    assert calls[-1][1] == ("POST", "/rule-packs/validate", {"format": "openport.rule-pack"})
    assert calls[-1][2] == {"control": True}
    client.pass_odds(economics=True, payout_odds=0.4, payout="800")
    params = calls[-1][2]["params"]
    assert params["economics"] == "true" and params["payout_odds"] == 0.4 and params["payout"] == "800"
    client.pass_odds()
    assert calls[-1][2]["params"]["economics"] is None


def test_real_prop_rule_responses_match_the_contract(tmp_path):
    binary = ROOT / "build/tests/openport_tests"
    if not binary.exists():
        pytest.skip("Build C++ tests to check real prop-rule responses")
    result = subprocess.run(
        [str(binary), "--gtest_filter=PaperEngine.PropRulesRoundTripThroughResetAndAccountViews:"
         "PaperEngine.DisciplineGuardrailsAndTiltFlagsAppearOnTheRiskView:PaperEngine.RulesCompareReplaysTheAttemptAgainstPresetsAndCustomRules:"
         "PaperEngine.RulePacksValidateNormalizeAndStaySeparateFromPresets:PlaybookApi.PassOddsAddChallengeEconomicsForAProgram"],
        env={**os.environ, "OPENPORT_CONTRACT_OUTPUT": str(tmp_path)}, capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    contract = Contract("", None, SPEC)
    rows = [json.loads(line) for line in (tmp_path / "prop-rules.jsonl").read_text().splitlines()]
    for row in rows:
        contract.validate(**row)
    paths = {row["path"] for row in rows}
    assert {"/api/account/rules-compare", "/api/rule-packs/validate", "/api/account/pass-odds", "/api/account"} <= paths
