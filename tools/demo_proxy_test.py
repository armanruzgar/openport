"""Exercise the demo Caddyfile locally with synthetic auth and backend servers.

Requires Docker and the caddy:2.10.2-alpine and python:3.12-slim-bookworm images.
No real credentials, DNS, certificates or public listener are used.
"""
import json
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request
import uuid


ROOT = Path(__file__).resolve().parents[1]
STUB = '''
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.server.server_port == 9091:
            ok = (self.headers.get("Cookie") == "fixture=ok" and not self.headers.get("Authorization")
                  and self.headers.get("Host") == "auth:9091"
                  and self.headers.get("X-Forwarded-Host") == "demo.localhost:8088")
            self.send_response(200 if ok else 401)
            self.end_headers()
        else:
            self.send_response(200)
            self.end_headers()
            self.wfile.write(json.dumps({"authorization": self.headers.get("Authorization"),
                                        "client": self.headers.get("X-Real-IP")}).encode())
ThreadingHTTPServer(("0.0.0.0", int(os.environ["PORT"])), Handler).serve_forever()
'''


def docker(*args):
    return subprocess.check_output(["docker", *args], text=True, stderr=subprocess.STDOUT).strip()


class DemoProxyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.name = "openport-proxy-test-" + uuid.uuid4().hex[:12]
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        stub = Path(cls.temp.name) / "stub.py"
        stub.write_text(STUB)
        docker("network", "create", cls.name)
        cls.addClassCleanup(docker, "network", "rm", cls.name)
        for alias, port in [("auth", "9091"), ("openport", "8080")]:
            docker("run", "-d", "--name", cls.name + "-" + alias, "--network", cls.name,
                   "--network-alias", alias, "--cpus", "0.25", "--memory", "128m", "-e", "PORT=" + port,
                   "-v", f"{stub}:/stub.py:ro", "python:3.12-slim-bookworm", "python", "/stub.py")
            cls.addClassCleanup(docker, "rm", "-f", cls.name + "-" + alias)
        docker("run", "-d", "--name", cls.name, "--network", cls.name, "--cpus", "0.25", "--memory", "128m",
               "-p", "127.0.0.1::8088", "-e", "DEMO_HOST=http://demo.localhost:8088", "-e", "AUTH_UPSTREAM=http://auth:9091",
               "-v", f"{ROOT / 'deploy/demo/Caddyfile'}:/etc/caddy/Caddyfile:ro", "caddy:2.10.2-alpine")
        cls.addClassCleanup(docker, "rm", "-f", cls.name)
        cls.url = "http://" + docker("port", cls.name, "8088/tcp")
        for _ in range(100):
            try:
                if cls.request("/")[0] == 401 and cls.request("/api/status", {"Cookie": "fixture=ok"})[0] == 200:
                    return
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(0.1)
        raise RuntimeError("Local proxy failed to start: " + docker("logs", cls.name))

    @classmethod
    def request(cls, path, headers=None):
        request = urllib.request.Request(cls.url + path, headers={"Host": "demo.localhost:8088", **(headers or {})})
        try:
            with urllib.request.urlopen(request, timeout=3) as response:
                return response.status, response.read().decode()
        except urllib.error.HTTPError as response:
            with response:
                return response.code, response.read().decode()

    def test_anonymous_pages_api_and_websocket_are_protected(self):
        for path in ["/", "/api/status", "/ws"]:
            with self.subTest(path=path):
                self.assertEqual(self.request(path)[0], 401)

    def test_cookie_auth_preserves_openport_bearer_and_overwrites_client_ip(self):
        status, body = self.request("/api/status", {"Cookie": "fixture=ok", "Authorization": "Bearer synthetic-token", "X-Real-IP": "203.0.113.99"})
        self.assertEqual(status, 200)
        result = json.loads(body)
        self.assertEqual(result["authorization"], "Bearer synthetic-token")
        self.assertNotEqual(result["client"], "203.0.113.99")
        self.assertTrue(result["client"])

    def test_bearer_alone_does_not_bypass_gateway(self):
        self.assertEqual(self.request("/api/status", {"Authorization": "Bearer synthetic-token"})[0], 401)

    def test_gateway_failure_is_closed(self):
        docker("pause", self.name + "-auth")
        try:
            # The backend remains healthy; a gateway outage must not bypass auth.
            try:
                status, _ = self.request("/api/status", {"Cookie": "fixture=ok"})
                self.assertGreaterEqual(status, 400)
            except (TimeoutError, OSError):
                pass
        finally:
            docker("unpause", self.name + "-auth")


if __name__ == "__main__":
    unittest.main()
