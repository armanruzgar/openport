#!/usr/bin/env python3
# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Validate openportd's HTTP contract; writes use a disposable simulated replay.

Requires jsonschema and pyyaml. Refuses to replace an active replay. Every API GET
is visited (including mirrors and archived routes); static files/WS are transports,
not JSON API calls. First mismatch reports method, path and JSON field.
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import math
from pathlib import Path
import re
import sys
import time
import uuid
from urllib.error import HTTPError
from urllib.parse import quote, urlencode
from urllib.request import Request, urlopen

import jsonschema
from referencing import Registry, Resource
import yaml


class ContractMismatch(RuntimeError):
    pass


def resting_bid(contract, band):
    """A buy limit at or under the bid, so it rests, and inside the plan's price band
    around mid. Whole dimes are valid at every tick tier."""
    bid, ask = contract.get("bid") or 0, contract.get("ask") or 0
    if bid < 1 or ask <= bid:
        return None
    mid = (bid + ask) / 2
    width = max(band[0], band[1] * mid)
    for dimes in (math.floor(bid * 10) - 1, math.floor(bid * 10)):
        price = dimes / 10
        if 0 < price < ask and mid - price < 0.9 * width:
            return f"{price:.2f}"
    return None


class Contract:
    def __init__(self, base_url, token, spec):
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.spec = spec
        self.checked = set()
        self.registry = Registry().with_resource("urn:openport", Resource.from_contents(
            {"$schema": "https://json-schema.org/draft/2020-12/schema", **spec}))

    def resolve(self, node):
        while "$ref" in node:
            ref = node["$ref"]
            if not ref.startswith("#/"):
                raise ContractMismatch("Only local contract references are supported")
            node = self.spec
            for part in ref[2:].split("/"):
                node = node[part.replace("~1", "/").replace("~0", "~")]
        return node

    def operation(self, path, method):
        plain = path.split("?", 1)[0]
        # Literal paths precede parameterized ones.
        for template, raw in sorted(self.spec["paths"].items(), key=lambda item: item[0].count("{")):
            pattern = re.sub(r"\\\{[^}]+\\\}", "[^/]+", re.escape(template))
            if re.fullmatch(pattern, plain):
                item = self.resolve(raw)
                if method.lower() in item:
                    return template, self.resolve(item[method.lower()])
        raise ContractMismatch(f"{method} {path}: undocumented route")

    def validate(self, method, path, status, body, content_type="application/json"):
        template, operation = self.operation(path, method)
        responses = operation["responses"]
        response = self.resolve(responses.get(str(status), responses.get("default", {})))
        content = response.get("content", {})
        media = content_type.split(";", 1)[0]
        if media not in content:
            raise ContractMismatch(f"{method} {path}: Content-Type {media} is not described for HTTP {status}")
        # References resolve against the entire OpenAPI document.
        schema = {"$id": "urn:openport:response", **content[media]["schema"]}
        def absolute(value):
            if isinstance(value, dict):
                return {key: "urn:openport" + item if key == "$ref" and item.startswith("#") else absolute(item) for key, item in value.items()}
            if isinstance(value, list):
                return [absolute(item) for item in value]
            return value
        validator = jsonschema.Draft202012Validator(absolute(schema), registry=self.registry)
        error = next(validator.iter_errors(body), None)
        if error:
            def leaves(problem):
                return [leaf for child in problem.context for leaf in leaves(child)] if problem.context else [problem]
            error = max(leaves(error), key=lambda problem: len(problem.absolute_path))
            field = ".".join(map(str, error.absolute_path)) or "$"
            raise ContractMismatch(f"{method} {path}: {field}: {error.message}")
        if media == "text/csv":
            columns = next(csv.reader(io.StringIO(body)), [])
            expected = ["account", "account_version", "provider", "prices", "new_york_date"]
            if columns[:5] != expected or ("fills.csv" in path and "actor" not in columns):
                raise ContractMismatch(f"{method} {path}: CSV header does not contain the documented columns")
        self.checked.add((template, method.lower()))
        return body

    def call(self, method, path, body=None, *, success=False):
        headers = {"Accept": "application/json"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        data = None if body is None else json.dumps(body).encode()
        if data is not None:
            headers["Content-Type"] = "application/json"
        request = Request(self.base_url + path, data=data, headers=headers, method=method)
        try:
            response = urlopen(request, timeout=120)
        except HTTPError as error:
            response = error
        with response:
            status, media, raw = response.status, response.headers.get_content_type(), response.read().decode()
        try:
            value = json.loads(raw) if media == "application/json" else raw
        except ValueError as error:
            raise ContractMismatch(f"{method} {path}: $: invalid JSON") from error
        self.validate(method, path, status, value, media)
        if success and not 200 <= status < 300:
            raise ContractMismatch(f"{method} {path}: expected success, HTTP {status}: {value}")
        return value

    def get_family(self, prefix, run_id=None, discovered_symbol=None):
        status = self.call("GET", prefix + "/status", success=True)
        symbols = status["underlyings"]
        if not symbols and discovered_symbol is None:
            raise ContractMismatch(f"GET {prefix}/status: underlyings: no symbol available for discovery")
        symbol = symbols[0]["symbol"] if symbols else discovered_symbol
        summary = self.call("GET", prefix + "/underlyings/" + quote(symbol, safe="") + "/summary")
        expiry = summary.get("expiries", [{}])[0].get("id") if summary.get("expiries") else None
        accounts = self.call("GET", prefix + "/accounts", success=True)["accounts"]
        playbooks = self.call("GET", prefix + "/playbooks", success=True)["definitions"]
        playbook = next(iter(playbooks), None)
        packs = self.call("GET", "/api/replay/packs", success=True)["packs"] if prefix == "/api/replay" else []
        pack = packs[0]["scenario"]["id"] if packs else None
        for template, raw in self.spec["paths"].items():
            if not template.startswith("/api/") or "get" not in self.resolve(raw):
                continue
            if prefix == "/api" and template.startswith("/api/replay"):
                continue
            if prefix == "/api/replay" and (not template.startswith(prefix + "/") or "/history/" in template):
                continue
            if run_id is not None and not template.startswith("/api/replay/history/{run_id}/"):
                continue
            path = template.replace("{symbol}", quote(symbol, safe="")).replace("{run_id}", quote(run_id or "", safe=""))
            if playbook is not None:
                path = path.replace("/playbooks/{id}", "/playbooks/" + quote(playbook, safe=""))
            if pack is not None:
                path = path.replace("/packs/{id}", "/packs/" + quote(pack, safe=""))
            if path in {"/api/backtests/{id}", "/api/backtests/sweeps/{id}"}:
                path = path.replace("{id}", "000000")  # A missing saved run exercises the documented error.
            path = path.replace("/orders/{id}", "/orders/1")
            if "{" in path:
                continue
            item = self.resolve(raw)
            names = {self.resolve(p)["name"] for p in item.get("parameters", [])}
            choices = accounts if "account" in names else [None]
            for account in choices:
                params = {}
                if account is not None:
                    params["account"] = account["id"]
                if "expiry" in names and expiry:
                    params["expiry"] = expiry
                self.call("GET", path + ("?" + urlencode(params) if params else ""))

    def run(self):
        listing = self.call("GET", "/api/replay", success=True)
        if listing.get("replay") is not None:
            raise ContractMismatch("/api/replay: stop the current replay before running the write contract test")
        self.get_family("/api")
        scenarios = listing.get("demos", [])
        if not scenarios:
            raise ContractMismatch("/api/replay: no simulated scenario available for the isolated write check")
        started = False
        try:
            scenario = next((item for item in scenarios if item.get("session", "regular") == "regular"), scenarios[0])
            state = self.call("POST", "/api/replay", {"scenario": scenario["id"], "paused": True, "plan": "practice"}, success=True)
            started = True
            deadline = time.monotonic() + 60
            while self.call("GET", "/api/replay", success=True)["replay"].get("fast_forwarding", False):
                if time.monotonic() >= deadline:
                    raise ContractMismatch("/api/replay: timed out preparing the simulated session")
                time.sleep(0.1)
            self.call("PUT", "/api/replay", {"until": "09:31"}, success=True)
            self.get_family("/api/replay")
            status = self.call("GET", "/api/replay/status", success=True)
            symbol = status["underlyings"][0]["symbol"]
            summary = self.call("GET", f"/api/replay/underlyings/{quote(symbol)}/summary", success=True)
            expiry = summary["expiries"][0]["id"]
            chain = self.call("GET", f"/api/replay/underlyings/{quote(symbol)}/chain?" + urlencode({"expiry": expiry}), success=True)
            limits = self.call("GET", "/api/replay/risk?account=main", success=True)["limits"]
            band = (float(limits["price_band_absolute"]), float(limits["price_band_relative"]))
            candidates = [(row[side], price) for row in chain["strikes"] for side in ("call", "put")
                          if row.get(side) and row[side].get("tradable")
                          for price in [resting_bid(row[side], band)] if price]
            if not candidates:
                raise ContractMismatch("chain.strikes: no tradable two-sided contract for a nonmarketable limit")
            contract, price = candidates[0]
            order = {"client_order_id": "contract-" + str(uuid.uuid4()), "symbol": contract["symbol"],
                     "side": "buy", "type": "limit", "time_in_force": "gtc", "quantity": 1, "limit_price": price}
            self.call("POST", "/api/replay/orders/preview?account=main", order, success=True)
            self.call("POST", "/api/replay/orders/what-if?account=main",
                      {"candidates": [{"name": "contract", "orders": [{k: v for k, v in order.items() if k != "client_order_id"}]}]}, success=True)
            placed = self.call("POST", "/api/replay/orders?account=main", order, success=True)
            if placed["order"]["status"] != "working":
                raise ContractMismatch("POST /api/replay/orders: order.status: expected a resting limit")
            self.call("POST", "/api/replay/orders/" + placed["order"]["id"] + "/preview?account=main", {"quantity": 2}, success=True)
            self.call("POST", "/api/replay/positions/close/preview?account=main", {}, success=True)
            self.call("DELETE", "/api/replay/orders/" + placed["order"]["id"] + "?account=main", success=True)
            self.call("GET", "/api/replay/orders/" + placed["order"]["id"] + "?account=main", success=True)
            self.call("GET", "/api/replay/orders?account=main", success=True)
            self.call("DELETE", "/api/replay", success=True)
            started = False
            history = self.call("GET", "/api/replay", success=True).get("history", [])
            if history:
                run_id = state["replay"].get("id") or history[0]["id"]
                self.call("GET", "/api/replay/history/" + quote(run_id), success=True)
                self.get_family("/api/replay/history/" + quote(run_id), run_id, symbol)
                self.call("GET", "/api/replay/history/" + quote(run_id) + "/orders/" + placed["order"]["id"], success=True)
                self.call("GET", "/api/replay/history", success=True)
                verify = "/api/replay/history/" + quote(run_id) + "/verify"
                self.call("POST", verify, {}, success=True)
                deadline = time.monotonic() + 120
                while True:
                    result = self.call("GET", verify, success=True)
                    if result["status"] != "running":
                        break
                    if time.monotonic() > deadline:
                        raise ContractMismatch(verify + ": verification timed out")
                    time.sleep(0.1)
                if result["status"] != "passed":
                    raise ContractMismatch(verify + ": " + result["message"])
                self.call("GET", verify + "?format=receipt", success=True)
            else:
                raise ContractMismatch("/api/replay: history is empty; enable durable journals to check archived routes")
        finally:
            if started:
                self.call("DELETE", "/api/replay", success=True)
        print(f"contract: {len(self.checked)} route/method responses validated")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_url")
    parser.add_argument("--token")
    parser.add_argument("--spec", type=Path, default=Path(__file__).resolve().parents[1] / "docs/openapi.yaml")
    args = parser.parse_args()
    try:
        spec = yaml.safe_load(args.spec.read_text())
        Contract(args.base_url, args.token, spec).run()
    except Exception as error:
        print(f"contract: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
