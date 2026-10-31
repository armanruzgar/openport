# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Guard the client's identity: installs come from tagged GitHub sources, not PyPI."""
import tomllib

from conftest import ROOT


def test_distribution_name_is_distinct_from_the_import_package():
    project = tomllib.loads((ROOT / "python/pyproject.toml").read_text())["project"]
    # "openport" on PyPI is an unrelated project; the import package stays openport.
    assert project["name"] == "openport-client"
    assert (ROOT / "python/src/openport/__init__.py").exists()


def test_readme_installs_the_current_version_from_its_tag():
    version = tomllib.loads((ROOT / "python/pyproject.toml").read_text())["project"]["version"]
    readme = (ROOT / "python/README.md").read_text()
    assert f"git+https://github.com/armanruzgar/openport@python-v{version}#subdirectory=python" in readme
    assert "pip install openport-client" not in readme, "The client is not on PyPI"


def test_no_workflow_publishes_to_pypi():
    for workflow in (ROOT / ".github/workflows").glob("*.yml"):
        assert "pypa/gh-action-pypi-publish" not in workflow.read_text(), workflow.name
