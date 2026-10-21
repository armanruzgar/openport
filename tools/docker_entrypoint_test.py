"""Offline tests for the container image's entrypoint; Docker is not needed."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/docker-entrypoint.sh"
SERVER = ["--address", "0.0.0.0", "--paper-journal", "/var/lib/openport/paper-journal.jsonl",
          "--write-token-file", "/var/lib/openport/write-token", "--web-root", "/usr/share/openport/web"]


def instruction(name):
    """The JSON (exec) form of a Dockerfile instruction."""
    match = re.search(rf"^{name} (\[.*\])$", (ROOT / "Dockerfile").read_text(), re.MULTILINE)
    return json.loads(match.group(1)) if match else None


class DockerEntrypointTest(unittest.TestCase):
    def setUp(self):
        self.bin = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.bin)
        # A stand-in openportd that prints the arguments it was given.
        fake = self.bin / "openportd"
        fake.write_text(f"#!{sys.executable}\nimport json, sys\nprint(json.dumps(sys.argv[1:]))\n")
        fake.chmod(0o755)

    def image(self, *args):
        """openportd's arguments for `docker run IMAGE args`: the ENTRYPOINT, then args or CMD."""
        entrypoint = instruction("ENTRYPOINT")
        self.assertEqual(entrypoint, ["/bin/sh", "/usr/local/bin/openport-entrypoint"])
        command = list(args) or instruction("CMD")
        environment = {**os.environ, "PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}"}
        result = subprocess.run(["/bin/sh", str(SCRIPT), *command], capture_output=True, text=True,
                                env=environment, check=True)
        return json.loads(result.stdout)

    def test_dockerfile_installs_the_entrypoint(self):
        self.assertIn("COPY tools/docker-entrypoint.sh /usr/local/bin/openport-entrypoint\n",
                      (ROOT / "Dockerfile").read_text())

    def test_server_start_keeps_the_image_settings(self):
        self.assertEqual(self.image(), [*SERVER, "--provider", "cboe", "--symbols", "SPX,SPY,QQQ,IWM,DIA"])
        self.assertEqual(self.image("--provider", "databento", "--symbols", "SPX,QQQ"),
                         [*SERVER, "--provider", "databento", "--symbols", "SPX,QQQ"])
        self.assertEqual(self.image("--compact-journals"), [*SERVER, "--compact-journals"])

    def test_one_shot_modes_get_only_their_own_arguments(self):
        for args in (["--verify-run", "/var/lib/openport/paper-journal.jsonl"],
                     ["--backtest", "pb", "--plan", "practice", "--scenarios", "5", "--seed", "1",
                      "--out", "/var/lib/openport/r.json"],
                     ["--import-day", "cboe", "--date", "2026-10-09", "--out", "/var/lib/openport/d.oprec"],
                     ["with space", "--backtest", "a b"]):
            with self.subTest(args=args):
                self.assertEqual(self.image(*args), args)


if __name__ == "__main__":
    unittest.main()
