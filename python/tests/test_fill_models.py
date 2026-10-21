# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Queue fills, stop stress, the fill-quality report and real-fill calibration on the wire."""
import jsonschema
import pytest

from conftest import SPEC
from openport import Client
from openport.types import AccountRules, FillCalibration, FillQuality, FillQualityGroup
from tools.contract_test import Contract


def validator(name):
    contract = Contract("", None, SPEC)
    return jsonschema.Draft202012Validator({"$ref": f"urn:openport#/components/schemas/{name}"}, registry=contract.registry)


def test_queue_and_stop_stress_settings_are_optional_rules_and_reset_fields():
    for key in ("queue_fills", "queue_seed", "stop_stress_ticks", "stop_stress_percent"):
        assert key in AccountRules.__optional_keys__
    reset = validator("ResetRequest")
    reset.validate({"reason": "queue", "plan": "practice", "fill_model": "queue", "queue_seed": 7,
                    "stop_stress_ticks": 2, "stop_stress_percent": 300})
    for bad in ({"stop_stress_ticks": 11}, {"stop_stress_percent": 100}, {"queue_seed": -1}, {"fill_model": "queues"}):
        with pytest.raises(jsonschema.ValidationError):
            reset.validate({"reason": "bad", "plan": "practice", **bad})
    wait = SPEC["components"]["schemas"]["OrderWait"]["properties"]["code"]["enum"]
    assert "QUEUE" in wait


def test_report_types_match_the_contract():
    schemas = SPEC["components"]["schemas"]
    for wire in (FillQuality, FillQualityGroup, FillCalibration):
        assert set(wire.__annotations__) == set(schemas[wire.__name__]["properties"])
    request = validator("CalibrationRequest")
    request.validate({"csv": "time,symbol,side,quantity,price\n", "format": "tastytrade", "max_quote_age_seconds": 60})
    for bad in ({"format": "robinhood"}, {"max_quote_age_seconds": 0}, {"stored": True}):
        with pytest.raises(jsonschema.ValidationError):
            request.validate({"csv": "", **bad})


def test_client_reads_the_report_scoped_and_posts_calibration_to_the_live_route(monkeypatch):
    calls = []
    def capture(self, method, path, body=None, **kwargs):
        calls.append((self._prefix, method, path, body, kwargs))
        return {}
    monkeypatch.setattr(Client, "_request", capture)
    client = Client(account="practice")
    client.fill_quality("all")
    assert calls[-1] == ("/api", "GET", "/fills/quality", None, {"scoped": True, "params": {"attempt": "all"}})
    client.for_replay().calibrate_fills("csv text", "ibkr", 30)
    assert calls[-1][1:] == ("POST", "/fills/calibration", {"csv": "csv text", "format": "ibkr", "max_quote_age_seconds": 30},
                             {"control": True})
