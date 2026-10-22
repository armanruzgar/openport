from typing import get_type_hints
from urllib.parse import urlsplit

import jsonschema
import pytest

from conftest import SPEC, shaped
from openport import Client
from openport.types import Account, AccountRules, PayoutBestDay, PayoutRules, PayoutStatus
from tools.contract_test import Contract


def test_payout_types_and_optional_rule_defaults():
    schemas = SPEC["components"]["schemas"]
    for wire in (PayoutRules, PayoutStatus, PayoutBestDay):
        assert set(get_type_hints(wire)) == set(schemas[wire.__name__]["properties"])
    assert get_type_hints(AccountRules)["payouts"] == PayoutRules | None
    assert get_type_hints(Account)["payout"] == PayoutStatus | None
    assert PayoutRules.__optional_keys__ == {"consistency_percents", "buffer", "buffer_payouts", "review", "after_payout", "reset_percent"}
    contract = Contract("", None, SPEC)
    validator = jsonschema.Draft202012Validator({"$ref": "urn:openport#/components/schemas/PayoutRules"}, registry=contract.registry)
    rules = dict(qualifying_profit="50.00", qualifying_days=1, withdrawal_percent=50,
                 split_percent=80, minimum="10.00", caps=[])
    validator.validate(rules)  # Requests from before these rules remain valid.
    validator.validate({**rules, "consistency_percents": [20, 25, 30], "buffer": "2100.00", "buffer_payouts": 3})
    for field, value in [("consistency_percents", [0]), ("consistency_percents", [101]),
                         ("consistency_percents", [40.5]), ("buffer_payouts", 101), ("buffer", 2100)]:
        with pytest.raises(jsonschema.ValidationError):
            validator.validate({**rules, field: value})


def test_account_keeps_exact_payout_standing(stub, monkeypatch):
    original = stub.respond
    standing = {**shaped("PayoutStatus"), "consistency_percent": 40, "cycle_profit": "300.000001",
                "best_day": {"day": "2026-09-22", "profit": "200.000001"},
                "consistency_needed": "200.01", "buffer_balance": "52100.00"}
    rules = {**shaped("PayoutRules"), "consistency_percents": [40], "buffer": "2100.00", "buffer_payouts": 3}

    def respond(method, target, headers, body):
        if method == "GET" and urlsplit(target).path == "/api/account":
            account = shaped("Account")
            account["rules"]["payouts"] = rules
            return 200, {**account, "payout": standing}
        return original(method, target, headers, body)

    monkeypatch.setattr(stub, "respond", respond)
    account = Client(stub.url).account()
    assert account["payout"] == standing
    assert account["rules"]["payouts"] == rules
