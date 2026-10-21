# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Guard release identity: no code checkout or stored secret in the publishing job."""
import yaml
from conftest import ROOT


def test_release_is_tag_gated_and_publishing_identity_is_isolated():
    workflow = yaml.load((ROOT / ".github/workflows/python-release.yml").read_text(), Loader=yaml.BaseLoader)
    assert workflow["on"] == {"push": {"tags": ["python-v*"]}}
    assert workflow["permissions"] == {"contents": "read"}
    build = workflow["jobs"]["build"]
    assert any("Tag must match python/pyproject.toml" in step.get("run", "") for step in build["steps"])
    publish = workflow["jobs"]["publish"]
    assert publish["needs"] == "build"
    assert publish["permissions"] == {"id-token": "write"}
    assert publish["environment"]["name"] == "pypi"
    assert any(step.get("uses", "").startswith("pypa/gh-action-pypi-publish@") for step in publish["steps"])
    assert all("checkout" not in step.get("uses", "") for step in publish["steps"])
    assert "secrets." not in str(publish)
