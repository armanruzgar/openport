# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Guard the client's identity: installs come from tagged GitHub sources, not PyPI."""
import re
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


def test_workflows_that_publish_pin_their_actions_by_commit():
    # These run with a token that can publish releases, the image or the site: a moved
    # or compromised tag of an action they use would run with it.
    for name in ("image.yml", "release-assets.yml", "pages.yml"):
        uses = re.findall(r"^\s*(?:-\s+)?uses:\s*(\S+)(.*)$", (ROOT / ".github/workflows" / name).read_text(), re.M)
        assert uses, name
        for action, rest in uses:
            assert re.fullmatch(r"[\w.-]+/[\w./-]+@[0-9a-f]{40}", action), f"{name}: {action}"
            assert re.fullmatch(r"\s+# v\d+(\.\d+)*", rest), f"{name}: {action} needs its version in a comment"
    assert "package-ecosystem: github-actions" in (ROOT / ".github/dependabot.yml").read_text()
