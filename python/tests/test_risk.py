from typing import get_type_hints

from openport.types import HistoricalRisk, Risk, RiskProfile, ScenarioAnalysis
from conftest import SPEC


def test_risk_analysis_wire_fields_and_optional_extensions():
    schemas = SPEC["components"]["schemas"]
    for wire in (HistoricalRisk, ScenarioAnalysis):
        assert set(get_type_hints(wire)) == set(schemas[wire.__name__]["properties"])
    assert "scopes" in Risk.__optional_keys__
    assert "scopes" not in schemas["Risk"]["required"]
    assert get_type_hints(RiskProfile)["term_scaled"] is bool
    assert "term_scaled" in RiskProfile.__optional_keys__
    assert "term_scaled" not in schemas["RiskProfile"]["required"]
    for wire in (RiskProfile, ScenarioAnalysis):
        assert {"dynamics", "surface_fallback"} <= wire.__optional_keys__
