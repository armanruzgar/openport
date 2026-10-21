from typing import get_type_hints

import pytest

from conftest import SPEC
from openport.types import AccountRules, FeeSchedule, Fill, FillFees, OrderPreview
from tools.contract_test import Contract


def test_fee_wire_types_match_the_contract():
    schemas = SPEC["components"]["schemas"]
    for wire in (FeeSchedule, FillFees):
        assert set(get_type_hints(wire)) == set(schemas[wire.__name__]["properties"])
    assert get_type_hints(Fill)["fees"] == FillFees | None
    assert get_type_hints(Fill)["feed"] == str | None
    assert "feed" in Fill.__optional_keys__
    assert set(get_type_hints(Fill)) <= set(schemas["Fill"]["properties"])
    assert get_type_hints(OrderPreview)["fee"] == str | None
    assert get_type_hints(OrderPreview)["fees"] == FillFees | None
    assert "fees" in AccountRules.__optional_keys__


def test_create_accepts_partial_custom_schedule_and_rejects_malformed_types():
    import jsonschema

    contract = Contract("", None, SPEC)
    validator = jsonschema.Draft202012Validator({"$ref": "urn:openport#/components/schemas/CreateAccountRequest"}, registry=contract.registry)
    body = {"name": "Fees", "initial_cash": "100000.00", "rules": {
        "profit_target": None, "max_drawdown": None, "drawdown_mode": "intraday",
        "buy_only": False, "buying_power": True, "expiry_cutoff_seconds": 0,
        "fees": {"open": "1.00", "index": {"SPXW": "0.600001"}}}}
    validator.validate(body)
    body["rules"]["fees"]["open"] = 1
    with pytest.raises(jsonschema.ValidationError):
        validator.validate(body)
