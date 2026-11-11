"""Offline checks that every release artifact carries the third-party notices."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
NOTICES = ROOT / "THIRD_PARTY_NOTICES.txt"
# Vite copies web/public into every build of the terminal, beside its index.html.
WEB_NOTICES = ROOT / "web/public/THIRD_PARTY_NOTICES.txt"
# Fetched only for tests and benchmarks, which never ship.
UNSHIPPED = {"googletest", "benchmark"}


class NoticesTest(unittest.TestCase):
    def test_every_fetched_library_that_ships_is_listed(self):
        cmake = (ROOT / "cmake/Dependencies.cmake").read_text()
        declared = re.findall(r"FetchContent_Declare\((\w+)\s+URL (https://github\.com/[^/\s]+/[^/\s]+)/", cmake)
        self.assertGreaterEqual(len(declared), 4, declared)
        notices = NOTICES.read_text()
        for name, repository in declared:
            if name not in UNSHIPPED:
                with self.subTest(name=name):
                    self.assertIn(f"  {repository}\n", notices)

    def test_every_web_runtime_package_is_listed(self):
        lock = json.loads((ROOT / "web/package-lock.json").read_text())
        packages = {key.rsplit("node_modules/", 1)[1] for key, entry in lock["packages"].items()
                    if key and not entry.get("dev") and not entry.get("devOptional")}
        self.assertIn("react", packages)
        notices = WEB_NOTICES.read_text()
        for package in sorted(packages):
            with self.subTest(package=package):
                # The whole name: react is not listed by react-dom or @tanstack/react-query.
                self.assertRegex(notices, rf"(?<![\w@/-]){re.escape(package)}(?![\w/-])")

    def test_licence_texts_are_in_full(self):
        notices = NOTICES.read_text()
        for text in ["Version 2.0, January 2004", "END OF TERMS AND CONDITIONS",
                     "Permission is hereby granted, free of charge",
                     "Redistribution and use in source and binary forms",
                     "Copyright (c) 1998-2026 The OpenSSL Project Authors"]:
            with self.subTest(text=text):
                self.assertIn(text, notices)
        web = WEB_NOTICES.read_text()
        for text in ["Permission is hereby granted, free of charge", "SIL OPEN FONT LICENSE Version 1.1",
                     "Copyright 2024 The Geist Project Authors", "OTHER DEALINGS IN THE FONT SOFTWARE."]:
            with self.subTest(text=text):
                self.assertIn(text, web)

    def test_every_artifact_carries_the_notices(self):
        # The release archives, from tools/release.sh and from the release workflow.
        self.assertIn('cp README.md LICENSE THIRD_PARTY_NOTICES.txt "$stage/"', (ROOT / "tools/release.sh").read_text())
        workflow = (ROOT / ".github/workflows/release-assets.yml").read_text()
        self.assertIn('cp README.md LICENSE "$stage/"\n', workflow)
        self.assertIn('[ ! -f THIRD_PARTY_NOTICES.txt ] || cp THIRD_PARTY_NOTICES.txt "$stage/"\n', workflow)
        # The deb and rpm packages, and the check run on them.
        packaging = (ROOT / "packaging/CMakeLists.txt").read_text()
        self.assertRegex(packaging, r"foreach\(file [^)]*THIRD_PARTY_NOTICES\.txt[^)]*share/openport/web/THIRD_PARTY_NOTICES\.txt\)")
        self.assertRegex(packaging, r'install\(FILES [^)]*"\$\{OPENPORT_STAGE\}/THIRD_PARTY_NOTICES\.txt"\s+DESTINATION share/doc/openport\)')
        package_test = (ROOT / "tools/package_test.py").read_text()
        for path in ["/usr/share/doc/openport/THIRD_PARTY_NOTICES.txt", "/usr/share/openport/web/THIRD_PARTY_NOTICES.txt"]:
            with self.subTest(path=path):
                self.assertIn(f'"{path}"', package_test)
        # The container image, beside its own licence.
        self.assertIn("COPY LICENSE THIRD_PARTY_NOTICES.txt /usr/share/doc/openport/\n", (ROOT / "Dockerfile").read_text())
        # Homebrew, from an archive that has them; the 0.5.1 archive does not.
        self.assertIn('doc.install "THIRD_PARTY_NOTICES.txt" if File.exist?("THIRD_PARTY_NOTICES.txt")\n',
                      (ROOT / "Formula/openport.rb").read_text())


if __name__ == "__main__":
    unittest.main()
