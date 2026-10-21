"""Inspect native release packages without installing or starting a service."""
import argparse
from pathlib import Path
import subprocess
import struct
import tempfile
import unittest


def run(*args):
    return subprocess.check_output(args, text=True).strip()


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
                         "/usr/lib/systemd/system/openport.service", "/usr/share/doc/openport/LICENSE"]:
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
