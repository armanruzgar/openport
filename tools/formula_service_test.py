"""Offline tests for the Homebrew service's command; Homebrew is not needed.

The formula's service runs /bin/sh with a script from a Ruby squiggly heredoc. The
test renders it as Ruby does, with temporary directories for the formula's paths,
and runs it with a stand-in openportd that prints its arguments and environment."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]
FORMULA = ROOT / "Formula/openport.rb"
SERVICE = re.compile(r'^    run \["/bin/sh", "-c", <<~SH, "openport-service"\]\n(.*?)^    SH\n', re.M | re.S)


class FormulaServiceTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        self.paths = {"etc": self.root / "etc", "var": self.root / "var", "opt_bin": self.root / "opt/bin",
                      "opt_pkgshare": self.root / "opt/share/openport"}
        self.settings = self.paths["etc"] / "openport"
        self.settings.mkdir(parents=True)
        self.paths["opt_bin"].mkdir(parents=True)
        fake = self.paths["opt_bin"] / "openportd"
        fake.write_text(f"#!{sys.executable}\nimport json, os, sys\n"
                        "print(json.dumps(dict(args=sys.argv[1:], env=dict(os.environ))))\n")
        fake.chmod(0o755)
        match = SERVICE.search(FORMULA.read_text())
        self.assertIsNotNone(match, "the service no longer runs /bin/sh -c <<~SH")
        script = textwrap.dedent(match.group(1))
        # Ruby interpolates the formula's paths; nothing else may be interpolated.
        self.assertEqual(set(re.findall(r"#\{(\w+)\}", script)), set(self.paths))
        self.assertNotRegex(script, r"#[$@]")
        script = re.sub(r"#\{(\w+)\}", lambda m: str(self.paths[m.group(1)]), script)
        # The only escape is \\ before a line break, a shell line continuation.
        self.assertNotIn("\\", re.sub(r"\\\\\n", "", script))
        self.script = script.replace("\\\\\n", "\\\n")

    def start(self, args=None, env=None):
        if args is not None:
            (self.settings / "openportd.args").write_text(args)
        if env is not None:
            (self.settings / "openportd.env").write_text(env)
        # launchd starts the service with a bare environment, not a shell's.
        return subprocess.run(["/bin/sh", "-c", self.script, "openport-service"], capture_output=True,
                              text=True, env={"PATH": "/usr/bin:/bin"}, cwd=self.root, check=False)

    def started(self, **kwargs):
        result = self.start(**kwargs)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def data_paths(self):
        var, web = self.paths["var"] / "openport", self.paths["opt_pkgshare"] / "web"
        return ["--address", "127.0.0.1", "--no-token-link", "--paper-journal", f"{var}/paper-journal.jsonl",
                "--candle-dir", f"{var}/candles", "--series-dir", f"{var}/series",
                "--write-token-file", f"{var}/write-token", "--web-root", str(web)]

    def test_the_log_never_carries_the_token_link(self):
        # launchd creates var/log/openport.log readable by every local account, and
        # openportd prints the write token's link at each start unless told not to.
        self.assertIn("--no-token-link", self.started()["args"])
        self.assertIn("openportd --print-url --open --write-token-file", FORMULA.read_text())

    def test_without_settings_it_runs_the_data_paths_as_before(self):
        started = self.started()
        self.assertEqual(started["args"], self.data_paths())
        self.assertNotIn("settings", started["env"])
        self.assertEqual(started["env"]["PATH"], "/usr/bin:/bin")

    def test_each_args_line_is_a_flag_and_its_value(self):
        started = self.started(args="# Tradier, with NVDA\n--provider tradier\n\n  --symbols   SPX,SPY,NVDA \t\n"
                                    "--record-dir /Users/me/My Recordings\n--no-history\n--option sandbox=true")
        self.assertEqual(started["args"], self.data_paths() + [
            "--provider", "tradier", "--symbols", "SPX,SPY,NVDA", "--record-dir", "/Users/me/My Recordings",
            "--no-history", "--option", "sandbox=true"])

    def test_env_lines_reach_openportd_as_written(self):
        started = self.started(env="# Keys stay out of the formula\nTRADIER_ACCESS_TOKEN=abc=def $HOME `id` \"q\"\n\n"
                                   "  OPENPORT_NOTIFY_NTFY_URL=https://ntfy.sh/topic  \nEMPTY=")
        env = started["env"]
        self.assertEqual(env["TRADIER_ACCESS_TOKEN"], 'abc=def $HOME `id` "q"')
        self.assertEqual(env["OPENPORT_NOTIFY_NTFY_URL"], "https://ntfy.sh/topic")
        self.assertEqual(env["EMPTY"], "")
        self.assertEqual(started["args"], self.data_paths())

    def test_a_malformed_env_line_stops_the_service_without_echoing_it(self):
        for line in ["not an assignment", "1ST=x", "BAD-NAME=x", "=value", "TOKEN secret-value"]:
            with self.subTest(line=line):
                result = self.start(env=f"GOOD=1\n# a comment\n{line}\n")
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, "")
                self.assertIn("openportd.env line 3: expected NAME=VALUE", result.stderr)
                self.assertNotIn("secret-value", result.stderr)

    def test_the_formula_keeps_the_service_and_makes_the_settings_directory(self):
        formula = FORMULA.read_text()
        for line in ['    (etc/"openport").mkpath\n', "    keep_alive true\n", '    working_dir var/"openport"\n',
                     '    error_log_path var/"log/openport.error.log"\n']:
            with self.subTest(line=line):
                self.assertIn(line, formula)
        self.assertIn("#{etc}/openport/openportd.args", formula.split("  def caveats", 1)[1])


if __name__ == "__main__":
    unittest.main()
