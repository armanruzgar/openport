# Copyright (c) 2026 OpenPort contributors. MIT License.
import copy
from http import client as http_client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import socket
import threading
from urllib.parse import parse_qs, urlsplit

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = json.loads((ROOT / "docs/openapi.yaml").read_text())
TIME = "2026-09-22T14:00:00.000000000Z"


def example(schema):
    if "$ref" in schema:
        return example(SPEC["components"]["schemas"][schema["$ref"].rsplit("/", 1)[1]])
    if "const" in schema:
        return schema["const"]
    if "enum" in schema:
        return schema["enum"][0]
    if "anyOf" in schema:
        return example(schema["anyOf"][0])
    kind = schema.get("type")
    if kind == "object":
        result = {key: example(value) for key, value in schema.get("properties", {}).items() if key in schema.get("required", [])}
        if schema is SPEC["components"]["schemas"]["AccountRules"]:
            # The default fixture is evaluation-phase; capital growth is funded-only.
            result["size_scaling"] = None
        return result
    if kind == "array":
        return []
    if kind == "number" or kind == "integer":
        return 1
    if kind == "boolean":
        return False
    if kind == "null":
        return None
    return "1.00" if "pattern" in schema else "fixture"


def shaped(schema_name, **fields):
    return {**example(SPEC["components"]["schemas"][schema_name]), **fields}


PLAYBOOK = {"id": "morning", "version": 1, "name": "Morning spread", "description": "Synthetic", "underlyings": ["SPX"],
            "window": {"start": "09:45", "end": "11:00", "weekdays": [1, 2, 3, 4, 5]}, "conditions": {},
            "structure": {"template": {"kind": "vertical"}, "expiry": {"min": 0, "max": 1}}, "sizing": {"units": 1},
            "management": {"close_by": "15:45"}, "guardrails": {"max_entries_per_day": 1, "cooldown_minutes": 30}}


class Stub:
    """Synthetic API fixture; all prices are explicitly simulated."""
    def __init__(self):
        self.requests = []
        self.failures = []
        self.retry_after = None
        self.order_bodies = {}
        self.expiry = shaped("Expiry", id="2026-10-22PM", expiry="2026-10-22")
        option = shaped("OptionQuote", symbol="SPXW  261022P05000000", tradable=True, bid=4, ask=4.2, mid=4.1)
        self.chain = shaped("Chain", symbol="SPX", spot=5000, as_of=TIME, expiry=self.expiry,
                            strikes=[shaped("ChainRow", strike=5000, call=None, put=option)])
        self.provider = shaped("ProviderInfo", name="demo (synthetic test)", simulated=True, delay_seconds=900)
        self.status = shaped("Status", provider=self.provider,
                             underlyings=[shaped("UnderlyingStatus", symbol="SPX", spot=5000, as_of=TIME)])
        self.replay = None
        self.history = []

    def respond(self, method, target, headers, body):
        self.requests.append((method, target, dict(headers), copy.deepcopy(body)))
        if self.failures:
            status, code, *retry = self.failures.pop(0)
            self.retry_after = retry[0] if retry else None
            return status, {"error": {"code": code, "message": "synthetic rejection", "actual": None, "limit": None, "scope": None}}
        path = urlsplit(target).path
        if path == "/api/backtests":
            if method == "GET":
                return 200, shaped("BacktestListing", runs=[], active=None)
            return 202, shaped("BacktestState", id="000001", report=None)
        if path == "/api/backtests/000001":
            return 200, shaped("BacktestState", id="000001", report=None)
        if path == "/api/replay/history":
            return 200, {"history": self.history}
        if path == "/api/replay/history/run-1/verify":
            return (202 if method == "POST" else 200), {"status": "running" if method == "POST" else "passed", "message": "Run run-1 verified"}
        if path == "/api/replay":
            if method == "GET":
                return 200, shaped("ReplayListing", replay=self.replay, history=self.history,
                                   demos=[shaped("ReplayDemo", id="fixture", simulated=True)])
            if method == "POST":
                self.replay = shaped("ReplayState", id="run-1", provider=self.provider["name"], demo=True, delay_seconds=900, time=TIME)
                return 201, {"replay": self.replay}
            if method == "DELETE":
                self.history = [{**self.replay, "read_only": True, "result": "open", "pnl": "0.00"}]
                self.replay = None
                return 200, {"replay": None}
            return 200, {"replay": self.replay, "settled_through": TIME}
        path = path.replace("/api/replay/history/run-1", "/api").replace("/api/replay", "/api")
        if path == "/api":
            path = "/api/account"
        if path == "/api/tokens/reload":
            return 200, {"names": ["operator"], "count": 1, "loaded_at": TIME}
        if path == "/api/status":
            return 200, self.status
        if "/underlyings/" in path:
            view = path.rsplit("/", 1)[-1]
            if view == "chain":
                return 200, self.chain
            name = {"surface": "Surface", "exposure": "ExposureMatrix", "volatility": "Volatility", "candles": "Candles",
                    "probability": "Probability"}.get(view, "Summary")
            value = shaped(name, symbol="SPX", as_of=TIME)
            if name == "Summary":
                value["expiries"] = [self.expiry]
            return 200, value
        if path == "/api/playbooks" and method == "GET":
            return 200, shaped("PlaybooksResponse", definitions={"morning": {"versions": [PLAYBOOK], "deleted": False}})
        if path == "/api/playbooks/morning" and method == "GET":
            return 200, PLAYBOOK
        if path == "/api/account/pass-odds":
            return 200, shaped("PassOdds", seed="81723")
        if path == "/api/calendar/events":
            return 200, {"events": []}
        if path == "/api/strategy-template":
            return 200, shaped("TemplateResult")
        if path == "/api/orders/what-if":
            return 200, shaped("WhatIfResponse", simulated=True)
        if path == "/api/positions/close/preview":
            return 200, shaped("FlattenPreview", simulated=True)
        if path == "/api/orders/preview" or (path.startswith("/api/orders/") and path.endswith("/preview")):
            return 200, shaped("OrderPreview", simulated=True, max_units_basis="limits")
        if path == "/api/orders" and method == "POST":
            key = body["client_order_id"]
            first = key not in self.order_bodies
            self.order_bodies[key] = body
            order = shaped("Order", id="1", client_order_id=key, actor="agent", status="working", quantity=body["quantity"])
            return 201 if first else 200, {"account_version": "1", "order": order, "fills": []}
        if path.startswith("/api/orders/") and method == "GET":
            return 200, {"account_version": "2", "order": shaped("Order", id=path.rsplit("/", 1)[-1], actor="agent", status="cancelled")}
        if path.startswith("/api/accounts/"):
            return 200, shaped("DeleteAccountResponse" if method == "DELETE" else "UpdateAccountResponse")
        if path.startswith("/api/orders/") and method in {"DELETE", "PUT"}:
            return 200, {"account_version": "2", "order": shaped("Order", actor="agent", status="cancelled"), "fills": []}
        if path.endswith(".csv"):
            return 200, "account,account_version,provider,prices,new_york_date,id,order_id,actor,symbol\n"
        name = {"account": "Account", "portfolio": "Portfolio", "orders": "OrdersResponse", "fills": "FillsResponse",
                "risk": "Risk", "limits": "Risk", "guardrails": "Risk", "profile": "RiskProfile", "history": "HistoricalRisk", "scenarios": "ScenarioAnalysis", "trades": "TradesResponse", "plans": "PlansResponse", "accounts": "AccountsResponse",
                "equity": "EquityHistory", "close": "ClosePositionsResponse", "cancel": "CancelAllResponse",
                "alerts": "AlertsResponse"}.get(path.rsplit("/", 1)[-1])
        if name:
            value = shaped(name)
            if name == "AccountsResponse":
                value["accounts"] = [shaped("AccountListItem", id="main")]
            if name in {"Portfolio", "Account"}:
                value["time"] = TIME
            return 200, value
        return 404, {"error": {"code": "NOT_FOUND", "message": "synthetic route", "actual": None, "limit": None, "scope": None}}


@pytest.fixture
def stub(monkeypatch):
    fixture = Stub()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def respond(self):
            size = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(size)) if size else None
            status, value = fixture.respond(self.command, self.path, self.headers, body)
            raw = value.encode() if isinstance(value, str) else json.dumps(value).encode()
            self.send_response(status)
            if fixture.retry_after is not None:
                self.send_header("Retry-After", str(fixture.retry_after))
                fixture.retry_after = None
            self.send_header("Content-Type", "text/csv" if isinstance(value, str) else "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = respond

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler, bind_and_activate=False)
    worker = None
    try:
        server.server_bind()
        server.server_activate()
        fixture.url = f"http://127.0.0.1:{server.server_port}"
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
    except PermissionError:
        # The sandbox denies bind(). Still exercise the actual HTTP protocol and
        # threaded http.server handler via connected local sockets, without a port.
        fixture.url = "http://127.0.0.1:8080"
        def connect(connection):
            client_socket, server_socket = socket.socketpair()
            client_socket.settimeout(connection.timeout)
            connection.sock = client_socket
            server.process_request(server_socket, ("127.0.0.1", 0))
        monkeypatch.setattr(http_client.HTTPConnection, "connect", connect)
    yield fixture
    if worker:
        server.shutdown()
        worker.join()
    server.server_close()
