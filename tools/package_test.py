"""Inspect native release packages without installing them or starting a service."""
import argparse
import os
from pathlib import Path
import shlex
import socket
import subprocess
import struct
import tempfile
import time
import unittest
import urllib.request


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class PackageTest(unittest.TestCase):
    def test_deb_architecture_and_dependencies(self):
        self.assertEqual(run("dpkg-deb", "-f", DEB, "Architecture"), ARCH)
        self.assertIn("libc6 (>= 2.39)", run("dpkg-deb", "-f", DEB, "Depends"))

    def test_rpm_architecture_and_dependencies(self):
        self.assertEqual(run("rpm", "-qp", "--qf", "%{ARCH}", RPM), {"amd64": "x86_64", "arm64": "aarch64"}[ARCH])
        self.assertIn("glibc >= 2.39", run("rpm", "-qp", "--requires", RPM))

    def test_both_packages_include_terminal_and_systemd_unit(self):
        for listing in [run("dpkg-deb", "-c", DEB), run("rpm", "-qpl", RPM)]:
            for path in ["/usr/bin/openportd", "/usr/bin/openport-probe", "/usr/share/openport/web/index.html",
                         "/usr/lib/systemd/system/openport.service", "/usr/share/doc/openport/LICENSE",
                         "/usr/share/doc/openport/THIRD_PARTY_NOTICES.txt",
                         "/usr/share/openport/web/THIRD_PARTY_NOTICES.txt"]:
                self.assertIn(path, listing)

    def test_extracted_binary_and_private_service_defaults(self):
        with tempfile.TemporaryDirectory() as directory:
            run("dpkg-deb", "-x", DEB, directory)
            root = Path(directory)
            header = (root / "usr/bin/openportd").read_bytes()[:20]
            self.assertEqual(header[:4], b"\x7fELF")
            self.assertEqual(header[4:6], b"\x02\x01")  # 64-bit, little endian
            self.assertEqual(struct.unpack("<H", header[18:20])[0], {"amd64": 62, "arm64": 183}[ARCH])
            version = run("dpkg-deb", "-f", DEB, "Version")
            self.assertEqual(run("rpm", "-qp", "--qf", "%{VERSION}", RPM), version)
            self.assertEqual(run(str(root / "usr/bin/openportd"), "--version"), "openportd " + version)
            unit = (root / "usr/lib/systemd/system/openport.service").read_text()
            for setting in ["DynamicUser=yes", "StateDirectoryMode=0700", "--address 127.0.0.1", "--no-token-link", "--write-token-file"]:
                self.assertIn(setting, unit)

    def test_the_units_command_line_starts_openportd(self):
        # Only the unit passes some of these flags: a renamed or removed one would make
        # the service exit on every start. They run as the unit writes them, with its
        # state directory and web root moved under a temporary directory, the offline
        # demo feed and a free port.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "root"
            run("dpkg-deb", "-x", DEB, str(root))
            state = Path(directory) / "state"
            state.mkdir(mode=0o700)
            unit = (root / "usr/lib/systemd/system/openport.service").read_text()
            start = next(line for line in unit.splitlines() if line.startswith("ExecStart="))
            command = shlex.split(start.removeprefix("ExecStart="))
            self.assertEqual(command[0], "/usr/bin/openportd")
            moved = {"/var/lib/openport": str(state), "/usr/share/openport/web": str(root / "usr/share/openport/web")}
            arguments = []
            for argument in command[1:]:
                for old, new in moved.items():
                    argument = argument.replace(old, new)
                arguments.append(argument)
            port = free_port()
            log = Path(directory) / "openportd.log"
            no_proxy = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            status = None
            with log.open("w") as output:
                daemon = subprocess.Popen(
                    [str(root / "usr/bin/openportd"), *arguments, "--provider", "demo", "--port", str(port)],
                    stdout=output, stderr=subprocess.STDOUT, env=dict(os.environ, HOME=str(state)))
                try:
                    deadline = time.monotonic() + 120
                    while status is None and daemon.poll() is None and time.monotonic() < deadline:
                        try:
                            with no_proxy.open(f"http://127.0.0.1:{port}/api/status", timeout=5) as response:
                                status = response.status
                        except OSError:
                            time.sleep(0.5)
                finally:
                    daemon.terminate()
                    daemon.wait(timeout=60)
            self.assertEqual(status, 200, log.read_text())
            self.assertEqual(daemon.returncode, 0, log.read_text())
            self.assertTrue((state / "write-token").is_file(), log.read_text())

    def test_install_does_not_start_service_or_remove_data(self):
        self.assertEqual(run("rpm", "-qp", "--scripts", RPM), "")
        with tempfile.TemporaryDirectory() as directory:
            run("dpkg-deb", "-e", DEB, directory)
            self.assertFalse(any((Path(directory) / name).exists() for name in ["postinst", "preinst", "prerm", "postrm"]))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--arch", required=True, choices=["amd64", "arm64"])
    args = parser.parse_args()
    ARCH = args.arch
    DEB, = map(str, args.directory.glob("*.deb"))
    RPM, = map(str, args.directory.glob("*.rpm"))
    unittest.main(argv=[__file__])
