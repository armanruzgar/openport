"""Offline tests for update_formula.sh; all checksums are test fixtures."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
ASSET = "openport-0.3.0-darwin-arm64.tar.gz"
DIGEST = "0123456789abcdef" * 4


class UpdateFormulaTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix=".formula-test-", dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "tools").mkdir()
        (self.root / "Formula").mkdir()
        self.script = self.root / "tools/update_formula.sh"
        self.formula = self.root / "Formula/openport.rb"
        shutil.copyfile(ROOT / "tools/update_formula.sh", self.script)
        shutil.copyfile(ROOT / "Formula/openport.rb", self.formula)
        self.original = self.formula.read_text()
        self.sums = self.root / "SHA256SUMS"
        shutil.copyfile(ROOT / "tests/data/release-SHA256SUMS", self.sums)
        self.env = {**os.environ, "TMPDIR": str(self.root)}

    def run_script(self, version="0.3.0", local=True):
        return subprocess.run(
            ["bash", str(self.script), version, *([str(self.sums)] if local else [])],
            cwd=self.root, env=self.env, capture_output=True, text=True, check=False,
        )

    def test_saved_checksums_select_exact_macos_asset(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        formula = self.formula.read_text()
        self.assertIn(f'  sha256 "{DIGEST}"\n', formula)
        self.assertIn(f'/v0.3.0/{ASSET}"', formula)
        self.assertEqual(formula.split("  def install", 1)[1], self.original.split("  def install", 1)[1])
        self.assertIn('openport.git", tag: "v0.3.0"', formula)

    def test_binary_marker_uppercase_digest_and_crlf(self):
        self.sums.write_bytes(f"{DIGEST.upper()} *{ASSET}\r\n".encode())
        self.assertEqual(self.run_script("v0.3.0").returncode, 0)
        self.assertIn(f'  sha256 "{DIGEST}"', self.formula.read_text())

    def test_changes_url_version_and_checksum_together(self):
        self.sums.write_text(self.sums.read_text().replace("0.3.0", "1.12.3"))
        self.assertEqual(self.run_script("1.12.3").returncode, 0)
        formula = self.formula.read_text()
        self.assertIn('/v1.12.3/openport-1.12.3-darwin-arm64.tar.gz"', formula)
        self.assertIn('  version "1.12.3"', formula)
        self.assertIn(f'  sha256 "{DIGEST}"', formula)

    def test_missing_duplicate_or_malformed_digest_leaves_formula_unchanged(self):
        for content in ["", f"{DIGEST}  {ASSET}.sig\n", f"short  {ASSET}\n",
                        f"{'g' * 64}  {ASSET}\n", f"{DIGEST}  {ASSET} extra\n",
                        f"{DIGEST}  {ASSET}\n" * 2]:
            with self.subTest(content=content):
                self.sums.write_text(content)
                self.assertNotEqual(self.run_script().returncode, 0)
                self.assertEqual(self.formula.read_text(), self.original)

    def test_rejects_invalid_versions(self):
        for version in ["", "../0.3.0", "0.3", "00.3.0", "0.3.0;echo bad", "0.3.0-rc.1"]:
            with self.subTest(version=version):
                self.assertNotEqual(self.run_script(version).returncode, 0)
                self.assertEqual(self.formula.read_text(), self.original)

    def test_unexpected_formula_layout_leaves_it_unchanged(self):
        original = self.original.replace("  sha256 ", "  # sha256 ")
        self.formula.write_text(original)
        self.assertNotEqual(self.run_script().returncode, 0)
        self.assertEqual(self.formula.read_text(), original)

    def test_download_uses_release_checksum_url_without_network(self):
        fake_bin = self.root / "fake-bin"
        fake_bin.mkdir()
        curl = fake_bin / "curl"
        curl.write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$@" > curl-args\n'
                        'while [ "$1" != -o ]; do shift; done\ncp SHA256SUMS "$2"\n')
        curl.chmod(0o755)
        self.env["PATH"] = f"{fake_bin}{os.pathsep}{self.env['PATH']}"
        self.assertEqual(self.run_script(local=False).returncode, 0)
        self.assertIn("https://github.com/armanruzgar/openport/releases/download/v0.3.0/SHA256SUMS",
                      (self.root / "curl-args").read_text().splitlines())

    def test_failed_download_leaves_formula_unchanged(self):
        fake_bin = self.root / "fake-bin"
        fake_bin.mkdir()
        curl = fake_bin / "curl"
        curl.write_text("#!/usr/bin/env bash\nexit 22\n")
        curl.chmod(0o755)
        self.env["PATH"] = f"{fake_bin}{os.pathsep}{self.env['PATH']}"
        self.assertNotEqual(self.run_script(local=False).returncode, 0)
        self.assertEqual(self.formula.read_text(), self.original)


if __name__ == "__main__":
    unittest.main()
