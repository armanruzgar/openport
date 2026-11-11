"""Offline checks that the oldest macOS the release archive supports is the same
wherever it is named: CMakeLists.txt sets it, and the formula, the release workflow,
the release notes and the install docs follow it."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
# Homebrew's names for the macOS releases a floor may name.
HOMEBREW = {"14": "sonoma", "15": "sequoia", "26": "tahoe"}


def floor():
    match = re.search(r"^set\(OPENPORT_MACOS_FLOOR (\d+)\.(\d+)\)$", (ROOT / "CMakeLists.txt").read_text(), re.M)
    assert match, "OPENPORT_MACOS_FLOOR is not set in CMakeLists.txt"
    return match.group(1), match.group(2)


class MacosFloorTest(unittest.TestCase):
    def test_static_builds_on_a_mac_target_it_before_project(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        default = cmake.index("set(CMAKE_OSX_DEPLOYMENT_TARGET ${OPENPORT_MACOS_FLOOR} CACHE STRING")
        self.assertLess(default, cmake.index("\nproject(openport"))
        self.assertIn("if(CMAKE_HOST_APPLE AND OPENPORT_STATIC_DEPS AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)", cmake)

    def test_formula_requires_it_for_the_archive_only(self):
        major, _ = floor()
        formula = (ROOT / "Formula/openport.rb").read_text()
        arm = re.search(r"^  on_arm do\n(.*?)^  end\n", formula, re.M | re.S)
        self.assertIsNotNone(arm)
        self.assertIn(f"    depends_on macos: :{HOMEBREW[major]}\n", arm.group(1))
        # Intel Macs build from source for themselves.
        intel = re.search(r"^  on_intel do\n(.*?)^  end\n", formula, re.M | re.S)
        self.assertNotIn("depends_on macos:", intel.group(1))

    def test_the_release_workflow_builds_the_archive_on_it(self):
        major, _ = floor()
        workflow = (ROOT / ".github/workflows/release-assets.yml").read_text()
        job = workflow.split("\n  macos:\n", 1)[1].split("\n  checksums:\n", 1)[0]
        self.assertIn(f"    runs-on: macos-{major}\n", job)
        self.assertIn("-DOPENPORT_STATIC_DEPS=ON", job)
        # Never replaces an archive the formula may pin.
        self.assertNotIn("--clobber", job)
        self.assertIn("needs: [linux, macos]", workflow)

    def test_release_notes_and_docs_name_it(self):
        major, _ = floor()
        self.assertIn("macOS ${macos_floor%.0} or later", (ROOT / "tools/release.sh").read_text())
        for path in ["README.md", "docs/install.md"]:
            with self.subTest(path=path):
                self.assertIn(f"macOS {major} or later", (ROOT / path).read_text())


if __name__ == "__main__":
    unittest.main()
