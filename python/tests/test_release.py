# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Guard release identity: no code checkout or stored secret in the publishing job."""
import re
import tomllib

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
    action = next(step["uses"] for step in publish["steps"] if step.get("uses", "").startswith("pypa/gh-action-pypi-publish@"))
    assert re.fullmatch(r"pypa/gh-action-pypi-publish@[0-9a-f]{40}", action), "Pin the publish action to a commit SHA"
    assert publish["environment"]["url"] == "https://pypi.org/p/openport-client"
    checkout = build["steps"][0]
    assert checkout["uses"].startswith("actions/checkout@") and checkout["with"]["fetch-depth"] == "0"
    assert any('git merge-base --is-ancestor "$GITHUB_SHA" origin/main' in step.get("run", "") for step in build["steps"])
    assert all("checkout" not in step.get("uses", "") for step in publish["steps"])
    assert "secrets." not in str(publish)


def test_distribution_name_is_distinct_from_the_import_package():
    project = tomllib.loads((ROOT / "python/pyproject.toml").read_text())["project"]
    # "openport" on PyPI is an unrelated project; the import package stays openport.
    assert project["name"] == "openport-client"
    assert (ROOT / "python/src/openport/__init__.py").exists()
    assert "pip install openport-client" in (ROOT / "python/README.md").read_text()
