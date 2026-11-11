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


def test_no_install_metadata_is_tracked():
    # An editable install writes *.egg-info beside the sources. A tracked copy goes stale
    # and shows a phantom distribution to anything that puts src on sys.path, as pytest does.
    import subprocess
    import pytest
    if not (ROOT / ".git").exists():
        pytest.skip("not a git checkout")
    tracked = subprocess.run(["git", "ls-files", "--", "*.egg-info/*"], cwd=ROOT,
                             capture_output=True, text=True, check=True).stdout
    assert tracked == ""
    ignored = subprocess.run(["git", "check-ignore", "--no-index", "-q", "python/src/openport_client.egg-info/PKG-INFO"],
                             cwd=ROOT)
    assert ignored.returncode == 0


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


def test_each_linux_release_image_is_smoke_tested_before_anything_is_attached():
    # CI tests only amd64 Linux and arm64 macOS; the arm64 Linux release must still start.
    workflow = (ROOT / ".github/workflows/release-assets.yml").read_text()
    linux = workflow.split("\n  linux:\n", 1)[1].split("\n  macos:\n", 1)[0]
    assert "runner: ubuntu-24.04-arm" in linux
    smoke = linux.split("- name: Smoke test the image on this architecture\n", 1)[1].split("\n      - name:", 1)[0]
    for check in ("curl -fsS http://127.0.0.1:8080/api/status", "python3 tools/smoke_trade.py http://127.0.0.1:8080",
                  "tools/contract_test.py http://127.0.0.1:8080"):
        assert check in smoke, check
    assert linux.index("Smoke test the image on this architecture") < linux.index("gh release upload")
    assert "if: always()" not in smoke

