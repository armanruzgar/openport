#!/usr/bin/env python3
"""Random orders and exact accounting checks on an isolated, paused replay.

    tools/order_fuzz.py URL --scenario hold-overnight --ops 200 --repeat \
        --playbook --restart-check --openportd build/apps/openportd --replays-dir DIR

Replaces any existing replay. Never trades the live account. --seed selects the
market (default 81723); --fuzz-seed selects commands (default 1). JSON includes the
literal command tape and each command's market time, so failures can be reproduced.
All dependencies are Python's standard library.

Docs: Money is decimal dollars; position market_value already includes quantity,
multiplier and sign. /fills contains option executions; /trades contains signed
stock_fills (including deliveries). /settlements removes signed option quantities.
Early assignments also remove options; expiry assignments must not be counted twice.
Successful abandon commands remove the entire pre-command long at zero. Dividends
change cash, never quantities. No resets are generated, so ledgers span one attempt.

Buying power may be negative after market moves, with enforcement disabled, or
when a command frees power (docs/paper-trading.md, Margin and buying power). Check
accepted orders against previews and reject spending free power, or reserving power
on an empty book, below zero. A negative snapshot alone is not a violation.

Scenario journals contain no run-specific fields to normalize (docs/scenarios.md
and docs/runtime.md). Compare raw head/count/bytes, and raw files when accessible.
History's `file` is the recording, NOT the account journal. Current servers expose
no journal path: supply --replays-dir, the replays/ beside --paper-journal, for CLI
verification or byte comparison. Verification never repairs or compacts a journal.

--playbook creates one live catalogue definition (left off on live accounts), then
enables the same version in auto on each replay. Tagged orders and their fills are
counted by actor; no automatic fills is a coverage failure unless every automatic
attempt was refused by plan rules, which is reported as a warning. --restart-check needs
--openportd and --replays-dir and checks an inclusive, mid-run journal prefix: the
restarted journal is the source's prefix byte for byte, then the one run_input record
the server journals for the restart, {"kind": "restart", "from": SOURCE, "at": T}.
"""
import argparse
from collections import Counter
import copy
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
import http.client
import hashlib
import json
from pathlib import Path
import random
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


PREFIX = "/api/replay"
WORKING = {"working", "partially_filled", "armed"}
# Documented plan order gates, not malformed orders, feed failures or matching errors.
PLAN_REFUSALS = {"BUY_ONLY", "DEFINED_RISK", "BUYING_POWER", "LIMIT_ONLY", "STOP_REQUIRED",
                 "MAX_TRADE_RISK", "MAX_CONTRACTS_HELD", "INSTRUMENT_NOT_ALLOWED", "OUTSIDE_PLAN_HOURS",
                 "EXPIRY_CUTOFF", "SCALING_LIMIT", "MAX_VOLUME_SHARE", "NEWS_BLACKOUT", "HOLD_RESTRICTED",
                 "MIN_HOLD", "MICROSCALPING", "HEDGING", "COUNTER_POSITION", "FLAT_TIME", "OVERNIGHT_HOLD"}


class FuzzError(Exception):
    """Transport, protocol, lifecycle or verification failure."""


def money(value):
    result = Decimal(str(value))
    if not result.is_finite():
        raise ValueError(f"non-finite amount: {value}")
    return result


def dumps(value):
    return json.dumps(value, sort_keys=True, indent=2, allow_nan=False, default=str) + "\n"


def error_code(body):
    error = body.get("error")
    return error.get("code") if isinstance(error, dict) else None


class Client:
    def __init__(self, base, token=None, timeout=3600):
        self.base, self.token, self.timeout = base.rstrip("/"), token, timeout

    def call(self, method, path, body=None):
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        request = urllib.request.Request(
            self.base + path, method=method, headers=headers,
            data=None if body is None else json.dumps(body, allow_nan=False).encode())
        try:
            try:
                response = urllib.request.urlopen(request, timeout=self.timeout)
            except urllib.error.HTTPError as exc:
                response = exc
            with response:
                status, raw = response.code, response.read()
            result = json.loads(raw, parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x)))
            if not isinstance(result, dict):
                raise ValueError("expected a JSON object")
            return status, result
        except (OSError, ValueError, http.client.HTTPException) as exc:
            # Do not echo headers or token; never retry an uncertain stock trade.
            raise FuzzError(f"{method} {path}: {type(exc).__name__}: {exc}") from exc


def request(client, method, path, body=None, refusals=None):
    status, result = client.call(method, path, body)
    if 200 <= status < 300:
        return status, result
    code = error_code(result)
    if 400 <= status < 500 and isinstance(code, str) and code and refusals is not None:
        refusals[code] += 1
        return status, result
    raise FuzzError(f"{method} {path}: HTTP {status}: {result}")


def get(client, path):
    return request(client, "GET", path)[1]


def snapshot(client):
    return {name: get(client, PREFIX + "/" + name) for name in
            ("account", "portfolio", "fills", "orders", "trades", "settlements")}


def signed(fill):
    return int(fill["quantity"]) * (1 if fill["side"] == "buy" else -1)


def check_invariants(state, before=None, accepted_order=False, abandoned=None):
    """Return structured failures and counters, with no tolerance or float arithmetic."""
    failures, counts = [], Counter(samples=1)

    def fail(check, **values):
        failures.append(dict(check=check, **values))

    portfolio, account = state["portfolio"], state["account"]
    positions, stocks = portfolio["positions"], portfolio["stocks"]
    marked = positions + stocks
    for holdings, quantity_field, multiplier in ((positions, "quantity", 100), (stocks, "shares", 1)):
        for position in holdings:
            if position["mark"] is None or position["market_value"] is None:
                continue
            expected = money(position["mark"]) * money(position[quantity_field]) * multiplier
            actual = money(position["market_value"])
            if actual != expected:
                fail("position_market_value", symbol=position["symbol"], actual=str(actual),
                     expected=str(expected), difference=str(actual - expected))
    if any(p["mark"] is None or p["market_value"] is None for p in marked):
        counts["equity_skipped_missing_marks"] += 1
    else:
        expected = money(portfolio["cash"]) + sum((money(p["market_value"]) for p in marked), Decimal(0))
        actual = money(portfolio["equity"])
        if actual != expected:
            fail("equity", actual=str(actual), expected=str(expected), difference=str(actual - expected),
                 cash=portfolio["cash"], market_values=[p["market_value"] for p in marked])
    if money(account["evaluation"]["equity"]) != money(portfolio["equity"]):
        fail("account_equity", account=account["evaluation"]["equity"], portfolio=portfolio["equity"])

    quantities, stock_quantities, fill_totals = Counter(), Counter(), Counter()
    option_fills = state["fills"]["fills"]
    for fill in option_fills:
        quantities[fill["symbol"]] += signed(fill)
        fill_totals[fill["order_id"], fill["symbol"]] += int(fill["quantity"])
    settlements = state["settlements"]["settlements"]
    settled = {(s["symbol"], s["time"]) for s in settlements}
    for settlement in settlements:
        quantities[settlement["symbol"]] -= int(settlement["quantity"])
    for fill in state["trades"]["stock_fills"]:
        stock_quantities[fill["symbol"]] += int(fill["shares"])
        option = fill.get("option")
        if option and fill["source"] in ("early_exercise", "assignment") and (option, fill["time"]) not in settled:
            # OSI suffix: YYMMDD C/P strike; every supported deliverable is 100 shares.
            contracts = Decimal(fill["shares"]) / 100 * (1 if option[-9] == "C" else -1)
            quantities[option] -= contracts
    for symbol, quantity in (abandoned or {}).items():
        quantities[symbol] -= quantity
    for name, ledger, held, field in (("option_quantity", quantities, positions, "quantity"),
                                      ("stock_quantity", stock_quantities, stocks, "shares")):
        actual = {p["symbol"]: int(p[field]) for p in held}
        for symbol in sorted(set(actual) | set(ledger)):
            if actual.get(symbol, 0) != ledger[symbol]:
                fail(name, symbol=symbol, actual=actual.get(symbol, 0), expected=ledger[symbol])
    filled_symbols = {f["symbol"] for f in option_fills}
    for p in positions:
        if p["symbol"] not in filled_symbols:
            fail("position_without_fills", symbol=p["symbol"], quantity=p["quantity"])

    seen = set()
    for order in state["orders"]["orders"]:
        oid, qty, filled, remaining = order["id"], int(order["quantity"]), int(order["filled_quantity"]), int(order["remaining_quantity"])
        if oid in seen:
            fail("duplicate_order", order_id=oid)
        seen.add(oid)
        if filled < 0 or filled > qty:
            fail("order_overfill", order_id=oid, quantity=qty, filled=filled)
        if order["status"] in WORKING and (remaining <= 0 or filled >= qty or order.get("ended_at") is not None):
            fail("filled_and_working", order_id=oid, status=order["status"], quantity=qty, filled=filled, remaining=remaining)
        if order["status"] == "filled" and (filled != qty or remaining != 0):
            fail("filled_order_quantity", order_id=oid, quantity=qty, filled=filled, remaining=remaining)
        legs = order.get("legs") or [{"symbol": order["symbol"], "ratio": 1}]
        for leg in legs:
            actual = fill_totals[oid, leg["symbol"]]
            expected = filled * int(leg.get("ratio", 1))
            if actual != expected:
                fail("order_fill_quantity", order_id=oid, symbol=leg["symbol"], actual=actual, expected=expected)

    available = money(portfolio["buying_power"]["available"])
    if available < 0:
        counts["negative_buying_power_samples"] += 1
        if accepted_order and before and account["rules"]["buying_power"]:
            previous = before["portfolio"]
            # Portfolio margin measures free power from equity; strategy from cash.
            basis = "equity" if account["rules"].get("margin") == "portfolio" else "cash"
            free = money(portfolio[basis]) - money(portfolio["buying_power"]["requirement"])
            previous_free = money(previous[basis]) - money(previous["buying_power"]["requirement"])
            empty_book = (not previous["positions"] and not previous["stocks"] and
                          not any(o["status"] in WORKING for o in before["orders"]["orders"]))
            # A resting order on an empty book cannot be buying protection or
            # restoring cover for another working order. Negative reservation is
            # therefore a violation even before any fill changes free power.
            spending = free < previous_free or (free == previous_free and empty_book)
            if spending and available <= money(previous["buying_power"]["available"]):
                fail("buying_power", available=str(available), before_available=previous["buying_power"]["available"],
                     free=str(free), before_free=str(previous_free), empty_book=empty_book)
    return failures, counts


def tick(underlying, price):
    if underlying in ("SPX", "NDX", "RUT"):
        return Decimal("0.05") if price < 3 else Decimal("0.10")
    if underlying == "XSP":
        return Decimal("0.01") if price < 3 else Decimal("0.05")
    return Decimal("0.01")


def grid_price(underlying, price):
    price = max(Decimal("0.01"), money(price))
    for _ in range(2):  # Rounding can cross the $3 tier boundary.
        unit = tick(underlying, price)
        price = max(unit, (price / unit).to_integral_value(rounding=ROUND_HALF_UP) * unit)
    return format(price, ".2f")


def epoch(value):
    return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()


def market_ns(value):
    """Journal timestamps are nanoseconds; do not round the inclusive cut via float."""
    match = re.fullmatch(r"(.*T\d{2}:\d{2}:\d{2})(?:\.(\d{1,9}))?(Z|[+-]\d{2}:\d{2})", value)
    if not match:
        raise FuzzError(f"expected a zoned market timestamp: {value}")
    whole, fraction, zone = match.groups()
    delta = datetime.fromisoformat(whole + zone.replace("Z", "+00:00")) - datetime(1970, 1, 1, tzinfo=timezone.utc)
    return (delta.days * 86400 + delta.seconds) * 10 ** 9 + int((fraction or "").ljust(9, "0"))


def create_playbook(client):
    definitions = get(client, "/api/playbooks")["definitions"]
    # Repeated campaigns may share a server. Do not edit or reuse someone else's ID.
    identifier = next((f"order-fuzz-{i}" for i in range(1, 101) if f"order-fuzz-{i}" not in definitions), None)
    if identifier is None:
        raise FuzzError("no free fuzz playbook ID")
    definition = dict(id=identifier, name="Order fuzz time stop", description="Determinism coverage",
                      underlyings=["SPX"], window=dict(start="09:30", end="09:31", weekdays=[1, 2, 3, 4, 5]),
                      conditions={}, structure=dict(template=dict(kind="vertical", type="put", direction="credit",
                      target=dict(mode="delta", value=30), width=5), expiry=dict(min=0, max=7)),
                      sizing=dict(units=1), management=dict(close_by="09:32"),
                      guardrails=dict(max_entries_per_day=1, cooldown_minutes=0))
    created = request(client, "POST", "/api/playbooks", definition)[1]
    return copy.deepcopy(created["definitions"][identifier]["versions"][-1])


def actor_counts(state, definition=None):
    orders, fills = state["orders"]["orders"], state["fills"]["fills"]
    result = dict(orders=Counter(o.get("actor", "unknown") for o in orders),
                  fills=Counter(f.get("actor", "unknown") for f in fills))
    if definition:
        tag = f"playbook:{definition['id']}@v{definition['version']}"
        tagged = [o for o in orders if tag in o.get("tags", [])]
        ids = {o["id"] for o in tagged}
        executions = [f for f in fills if f["order_id"] in ids]
        result["playbook"] = dict(tag=tag, orders=len(tagged), fills=len(executions),
                                  orders_by_actor=Counter(o.get("actor", "unknown") for o in tagged),
                                  fills_by_actor=Counter(f.get("actor", "unknown") for f in executions),
                                  automatic_refusals=Counter((o.get("reason") or {}).get("code", "unknown") for o in tagged
                                                            if o.get("actor") == "system" and o.get("status") == "rejected"),
                                  time_stop_orders=sum(o.get("note", "").startswith("Playbook automatic time stop;") for o in tagged))
    return result


def playbook_preview_refusals(publication, definition, rules):
    # Auto evaluates immediately when enabled. A buy-only spread fails preview
    # before submission, so it leaves no tagged order. The publication exposes
    # messages, not codes: require the exact reducer refusal and the active rule.
    message = "This plan is buy-only; multi-leg orders may only close held positions"
    identifier = definition["id"]
    if not rules.get("buy_only") or publication["modes"].get(identifier) != "auto":
        return {}
    return {symbol: dict(code="BUY_ONLY", message=message) for symbol in definition["underlyings"]
            if publication.get("reasons", {}).get(f"{identifier}:{symbol}") == message}


def operation(kind, method, suffix, body=None, preview=False):
    return dict(kind=kind, method=method, path=PREFIX + suffix, body=body, preview=preview)


class Generator:
    def __init__(self, client, rng, replay, ops):
        self.client, self.rng, self.replay, self.ops = client, rng, replay, ops
        self.chains = {}

    def advance(self, step, state):
        now = state["portfolio"]["time"]
        remaining = max(1, int(epoch(self.replay["end"]) - epoch(now)))
        # Budget advances over the whole scenario, including overnight gaps. Small
        # next/seconds steps still exercise boundaries. The final step reaches EOF.
        if step == self.ops:
            until = self.replay["end"]
        elif self.rng.random() < .25:
            until = self.rng.choice(["next", "+1s", "+15s"])
        else:
            budget = max(1, (self.ops - step) * .20)
            minutes = max(1, min(remaining // 60, int(remaining / budget / 60 * self.rng.uniform(.6, 1.2))))
            until = f"+{minutes}m" if remaining >= 60 else f"+{remaining}s"
        return operation("advance", "PUT", "", {"until": until})

    def quotes(self, state):
        now = state["portfolio"]["time"]
        if self.chains.get("time") == now:
            return self.chains["quotes"]
        quotes = []
        for underlying in self.replay["symbols"]:
            summary = get(self.client, PREFIX + f"/underlyings/{underlying}/summary")
            expiries = summary["expiries"]
            if not expiries:
                continue
            # Both the front and a later expiry; allows expiry and carry positions.
            expiry = self.rng.choice(expiries[:3])["id"]
            chain = get(self.client, PREFIX + f"/underlyings/{underlying}/chain?" + urllib.parse.urlencode({"expiry": expiry}))
            for row in chain["strikes"]:
                for kind in ("call", "put"):
                    q = row.get(kind)
                    if q and q.get("bid") is not None and q.get("ask") is not None and money(q["ask"]) > 0:
                        quotes.append(dict(q, underlying=underlying, expiry=expiry, option_type=kind))
        self.chains = dict(time=now, quotes=quotes)
        return quotes

    def draw(self, step, state):
        r, portfolio = self.rng, state["portfolio"]
        kind = r.choices(["advance", "single", "spread", "stop", "bracket", "exit", "modify", "cancel",
                          "stock", "stock_close", "flatten", "abandon", "instruction", "limits", "guardrails"],
                         [25, 22, 9, 6, 8, 5, 6, 5, 5, 3, 3, 1, 2, 1, 1])[0]
        if kind == "advance" or step == self.ops:
            return self.advance(step, state)
        held = portfolio["positions"]
        working = [o for o in state["orders"]["orders"] if o["status"] in WORKING]
        if kind in ("modify", "cancel") and working:
            order = r.choice(working)
            if kind == "cancel":
                return operation(kind, "DELETE", "/orders/" + order["id"])
            body = {"quantity": int(order["filled_quantity"]) + r.randint(1, 4)}
            if order["type"] == "limit" and order["symbol"]:
                body["limit_price"] = grid_price(order["underlying"], money(order["limit_price"]) + r.choice([-1, 1]) * tick(order["underlying"], money(order["limit_price"])))
            return operation(kind, "PUT", "/orders/" + order["id"], body, r.random() < .5)
        if kind == "flatten":
            return operation(kind, "POST", "/positions/close", {})
        if kind in ("abandon", "instruction") and held:
            position = r.choice([p for p in held if p["quantity"] > 0] or held)
            body = {"symbol": position["symbol"]}
            if kind == "instruction":
                body["do_not_exercise"] = r.choice([True, False])
            return operation(kind, "POST", "/positions/" + kind, body)
        if kind in ("limits", "guardrails"):
            risk = get(self.client, PREFIX + "/risk")
            settings = copy.deepcopy(risk[kind])
            if kind == "limits":
                settings["max_order_contracts"] = r.choice([2, 5, 10, 25])
            else:
                settings["cooldown_minutes"] = r.choice([0, 1, 3])
                settings["cooldown_loss"] = r.choice(["0.00", "100.00", "500.00"])
            return operation(kind, "PUT", "/risk/" + kind,
                             {"expected_revision": risk["limits_revision"], kind: settings})
        if kind in ("stock", "stock_close"):
            # Session metadata covers holidays and early closes; shares stop at 16:00
            # (13:00 early), fifteen minutes before the regular options session end.
            now = epoch(portfolio["time"])
            stock_hours = any(s["session"] == "regular" and epoch(s["open"]) <= now < epoch(s["end"]) - 15 * 60
                              for s in self.replay["sessions"])
            symbols = [s for s in self.replay["symbols"] if s in ("SPY", "QQQ")]
            if stock_hours and kind == "stock_close" and portfolio["stocks"]:
                return operation(kind, "POST", "/stocks/close", {"symbol": r.choice(portfolio["stocks"])["symbol"]})
            if stock_hours and symbols:
                return operation("stock", "POST", "/stocks/trade",
                                 {"symbol": r.choice(symbols), "side": r.choice(["buy", "sell"]), "shares": r.choice([1, 10, 100])}, r.random() < .5)
        quotes = self.quotes(state)
        if not quotes:
            return self.advance(step, state)
        # Bias toward liquid, affordable OTM contracts, but retain the full chain.
        liquid = [q for q in quotes if q.get("delta") is not None and .05 <= abs(q["delta"]) <= .55]
        q = r.choice(liquid or quotes)
        side, quantity = r.choice(["buy", "sell"]), r.choice([1, 1, 2, 3, 12])
        positions = {p["symbol"]: p for p in held}
        closing = [q for q in quotes if q["symbol"] in positions]
        if closing and (kind == "exit" or r.random() < .35):
            q = r.choice(closing)
            pos = positions[q["symbol"]]
            side, quantity = ("sell" if pos["quantity"] > 0 else "buy"), r.randint(1, int(abs(pos["quantity"])))
        typ = r.choice(["market", "limit", "limit"])
        body = dict(client_order_id=f"fuzz-{step}", symbol=q["symbol"], side=side, quantity=quantity,
                    type=typ, time_in_force="ioc" if typ == "market" else r.choice(["ioc", "day", "gtc"]))
        price = money(r.choice([q["bid"], q["ask"]])) + r.choice([-1, 0, 1]) * tick(q["underlying"], money(q["ask"]))
        if typ == "limit":
            body["limit_price"] = grid_price(q["underlying"], price)
        actual_kind = "single_" + typ
        if kind == "spread":
            others = [p for p in quotes if p["underlying"] == q["underlying"] and p["expiry"] == q["expiry"]
                      and p["option_type"] == q["option_type"] and p["symbol"] != q["symbol"]]
            if others:
                other = r.choice(others)
                body.pop("symbol")
                body.pop("side")
                body["legs"] = [{"symbol": q["symbol"], "side": side, "ratio": 1},
                                {"symbol": other["symbol"], "side": "sell" if side == "buy" else "buy", "ratio": 1}]
                if typ == "limit":
                    net = (money(q["ask"]) - money(other["bid"])) if side == "buy" else (money(other["ask"]) - money(q["bid"]))
                    unit = tick(q["underlying"], Decimal(0))
                    body["limit_price"] = format((net / unit).to_integral_value(rounding=ROUND_HALF_UP) * unit, ".2f")
                actual_kind = "spread_" + typ
        elif kind in ("stop", "bracket", "exit"):
            exit_only = kind == "exit" and q["symbol"] in positions
            long = (side == "sell") if exit_only else (side == "buy")
            level = grid_price(q["underlying"], money(q["bid"] if long else q["ask"]) * (Decimal("0.8") if long else Decimal("1.2")))
            trigger = {"source": "option", "direction": "at_or_below" if long else "at_or_above", "level": level}
            if kind == "stop":
                body["trigger"] = trigger
            else:
                body["bracket"] = {"stop_loss": {"trigger": trigger}}
                if exit_only:
                    body.update(exits_only=True, type="market", time_in_force="ioc", trigger=trigger)
                    body.pop("limit_price", None)
                elif r.random() < .5:
                    body["bracket"]["take_profit"] = {"limit_price": grid_price(q["underlying"], money(q["ask"]) * (Decimal("1.2") if long else Decimal("0.8")))}
            actual_kind = "exit" if exit_only else ("stop" if kind == "stop" else "bracket")
        return operation(actual_kind, "POST", "/orders", body, r.random() < .5)


def journal_path(entry, directory=None):
    # `file` is a recording identifier. Never interpret it as a journal path.
    exposed = entry.get("journal_path")
    if exposed:
        return Path(exposed)
    if directory:
        run_id = entry["id"]
        if Path(run_id).name != run_id or run_id in (".", ".."):
            raise FuzzError("unsafe run id in history")
        return Path(directory) / (run_id + ".jsonl")
    raise FuzzError("history exposes no journal path; use --replays-dir DIR beside the server's --paper-journal")


def check_history(entry):
    if entry.get("error") or any(entry.get(k) for k in ("torn", "bytes_cut", "truncated", "mismatch")):
        raise FuzzError(f"journal integrity: {entry}")
    for key in ("journal", "journal_found"):
        if not all(k in entry.get(key, {}) for k in ("head", "transactions", "bytes")):
            raise FuzzError(f"missing history {key} checkpoint")
    if entry["journal"] != entry["journal_found"]:
        raise FuzzError(f"journal checkpoint mismatch: {entry['journal']} != {entry['journal_found']}")


def verify(client, run_id, timeout=3600, pause=.2):
    path = PREFIX + "/history/" + urllib.parse.quote(run_id, safe="") + "/verify"
    request(client, "POST", path, {})
    deadline = time.monotonic() + timeout
    while True:
        result = get(client, path)
        if result["status"] in ("passed", "failed"):
            return result
        if result["status"] not in ("idle", "running"):
            raise FuzzError(f"unexpected verification state: {result}")
        if time.monotonic() >= deadline:
            raise FuzzError(f"verification timed out for {run_id}")
        time.sleep(pause)


def wait_paused(client, replay, at=None, timeout=3600):
    deadline = time.monotonic() + timeout
    while replay and (replay.get("fast_forwarding") or replay.get("stepping") or not replay.get("time")):
        if time.monotonic() >= deadline:
            raise FuzzError("replay preparation timed out")
        time.sleep(.1)
        replay = get(client, PREFIX)["replay"]
    if not replay or not replay["paused"]:
        raise FuzzError("replay did not settle paused")
    if at and market_ns(replay.get("settled_through") or replay["time"]) != market_ns(at):
        raise FuzzError(f"restart did not settle through {at}: {replay}")
    return replay


def verify_saved(client, result, binary, directory, failure, out):
    run_id = result["run_id"]
    try:
        history = get(client, PREFIX + "/history")["history"]
        entry = next((r for r in history if r["id"] == run_id), None)
        if entry is None:
            raise FuzzError(f"run {run_id} missing from history")
        result["history"] = entry
        try:
            check_history(entry)
        except FuzzError as exc:
            failure("journal_integrity", message=str(exc))
        out("verifying", run_id, "via HTTP")
        result["verification"] = verify(client, run_id)
        if result["verification"]["status"] != "passed":
            failure("verification", result=result["verification"])
        if binary:
            path = journal_path(entry, directory)
            out("verifying", run_id, "via --verify-run", path)
            process = subprocess.run([str(binary), "--verify-run", str(path.resolve())],
                                     capture_output=True, text=True, timeout=3600, check=False)
            result["cli_verification"] = dict(status="passed" if process.returncode == 0 else "failed",
                                              returncode=process.returncode, journal=str(path),
                                              stdout=process.stdout, stderr=process.stderr)
            if process.returncode:
                failure("cli_verification", result=result["cli_verification"])
    except (FuzzError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as exc:
        failure("verification", message=str(exc))


def restart_time(source):
    start, end = market_ns(source["start_market_time"]), market_ns(source["end_market_time"])
    candidates = {op["market_time"] for op in source["commands"] if start < market_ns(op["market_time"]) < end}
    if not candidates:
        raise FuzzError("restart needs a recorded command time strictly inside the run")
    return min(candidates, key=lambda t: (abs(2 * market_ns(t) - start - end), market_ns(t)))


def journal_prefix(path, at):
    """Keep transactions through T, excluding inputs for commands after T."""
    target, offset, previous, count = market_ns(at), 0, -1, 0
    digest, prefix, past_target = hashlib.sha256(), None, False
    with open(path, "rb") as stream:
        for line in stream:
            digest.update(line)
            record = json.loads(line)
            count += 1
            timestamp = record["time"]
            if (not line.endswith(b"\n") or type(timestamp) is not int or timestamp < previous
                    or record["seq"] != count):
                raise FuzzError("invalid source journal sequence, time or incomplete record")
            previous = timestamp
            offset += len(line)
            # Input-first journals stamp a command record with the preceding
            # account time. Its explicit market time determines whether it runs.
            later_command = record.get("type") == "run_input" and any(
                event.get("type") == "run_input" and event["payload"].get("kind") == "command"
                and event["payload"]["time"] > target for event in record["payload"]["events"])
            past_target = past_target or timestamp > target or later_command
            if not past_target:
                prefix = dict(bytes=offset, transactions=count, head=record["hash"], last_time_ns=timestamp)
    if prefix is None:
        raise FuzzError("source journal has no transaction at or before restart time")
    return dict(prefix, source_sha256=digest.hexdigest(), source_bytes=offset)


def identical_prefix(source, restarted, length):
    """Both files start with the same `length` bytes."""
    with open(source, "rb") as a, open(restarted, "rb") as b:
        while length:
            chunk = a.read(min(length, 1024 * 1024))
            if not chunk or chunk != b.read(len(chunk)):
                return False
            length -= len(chunk)
        return True


def restart_record(restarted, prefix, source_id, at):
    """The checkpoint after the restart input that follows the copied prefix.

    A restarted run re-executes the source's prefix, then journals one run_input
    record, {"kind": "restart", "from": SOURCE, "at": T}. None unless that record,
    complete and alone, is all the restarted journal holds after the prefix: a second
    record, a torn one or another input is a mismatch."""
    with open(restarted, "rb") as stream:
        stream.seek(prefix["bytes"])
        line = stream.readline()
        if not line.endswith(b"\n") or stream.read(1):
            return None
    try:
        record = json.loads(line)
        inputs = [event["payload"] for event in record["payload"]["events"] if event["type"] == "run_input"]
        checkpoint = dict(head=record["hash"], bytes=prefix["bytes"] + len(line), transactions=record["seq"])
        marker = {"kind": "restart", "from": source_id, "at": market_ns(at)}
        if (record["seq"] != prefix["transactions"] + 1 or record["type"] != "run_input"
                or type(record["time"]) is not int or record["time"] < prefix["last_time_ns"] or inputs != [marker]):
            return None
    except (ValueError, KeyError, TypeError, AttributeError, FuzzError):
        return None
    return checkpoint


def restart_check(client, source, binary, directory, out=print):
    result = dict(status="failed", failures=[], verification={"status": "not_run"},
                  cli_verification={"status": "not_run"})
    attempted = False

    def failure(check, **values):
        item = dict(check=check, **values)
        result["failures"].append(item)
        out("FAIL restart", dumps(item).strip())

    try:
        if not binary or not directory:
            raise FuzzError("--restart-check requires --openportd and --replays-dir")
        at = restart_time(source)
        source_path = journal_path(source["history"], directory)
        prefix = journal_prefix(source_path, at)
        result.update(source_run_id=source["run_id"], at=at, prefix=prefix)
        out("restarting", source["run_id"], "at", at, "prefix transactions", prefix["transactions"])
        attempted = True
        replay = request(client, "POST", PREFIX, dict(restart=source["run_id"], at=at))[1]["replay"]
        result["run_id"] = replay["id"]
        if replay["id"] == source["run_id"]:
            raise FuzzError("restart reused source run ID")
        replay = wait_paused(client, replay, at)
        result["settled_through"] = replay.get("settled_through") or replay["time"]
    except (FuzzError, KeyError, TypeError, ValueError, OSError) as exc:
        failure("restart", message=str(exc))
    finally:
        if attempted:
            try:
                request(client, "DELETE", PREFIX)
            except (FuzzError, OSError) as exc:
                failure("stop", message=str(exc))
    if "run_id" in result:
        verify_saved(client, result, binary, directory, failure, out)
        try:
            current = journal_prefix(source_path, at)
            if current != prefix:
                failure("restart_source_changed", before=prefix, after=current)
            entry = result["history"]
            restarted = journal_path(entry, directory)
            # The source's inclusive prefix byte for byte, then only the restart input.
            marker = (restart_record(restarted, prefix, source["run_id"], at)
                      if identical_prefix(source_path, restarted, prefix["bytes"]) else None)
            result["files"] = "identical_prefix" if marker else "different"
            result["restart_record"] = marker
            if not marker or entry["journal"] != marker:
                failure("restart_prefix", expected=prefix, restart_record=marker, actual=entry["journal"])
        except (FuzzError, KeyError, TypeError, ValueError, OSError) as exc:
            failure("restart_prefix", message=str(exc))
    result["status"] = "failed" if result["failures"] else "passed"
    return result


def identical_journals(first, second):
    """Read every byte; stat-based caches must not hide a changed journal."""
    with open(first, "rb") as a, open(second, "rb") as b:
        while True:
            chunk = a.read(1024 * 1024)
            if chunk != b.read(1024 * 1024):
                return False
            if not chunk:
                return True


def compare_runs(runs, directory=None):
    result = {"status": "passed", "normalized_fields": [], "files": "not_requested"}
    a, b = [r["history"] for r in runs]
    result["journals"] = [a["journal"], b["journal"]]
    if a["journal"] != b["journal"]:
        result["status"] = "failed"
    if directory:
        equal = identical_journals(journal_path(a, directory), journal_path(b, directory))
        result["files"] = "identical" if equal else "different"
        if not equal:
            result["status"] = "failed"
    # Different refusals/times must also be visible even if neither reached the journal.
    if runs[0]["outcomes"] != runs[1]["outcomes"]:
        result["status"] = "failed"
        result["outcomes_match"] = False
    for key in ("playbook", "actors"):
        if runs[0].get(key) != runs[1].get(key):
            result["status"] = "failed"
            result[key + "_match"] = False
    return result


def run_once(client, start, ops, fuzz_seed, tape=None, binary=None, directory=None, out=print, playbook=None):
    result = dict(operations=Counter(), refusals=Counter(), preview_refusals=Counter(), checks=Counter(),
                  failures=[], warnings=[], commands=[], outcomes=[], verification={"status": "not_run"},
                  cli_verification={"status": "not_requested"})
    run_id, step, op, start_attempted = None, 0, None, False

    def failure(check, **values):
        item = dict(step=step, operation=copy.deepcopy(op), check=check, **values)
        result["failures"].append(item)
        out("FAIL", dumps(item).strip())

    try:
        request(client, "DELETE", PREFIX)
        # A lost/malformed start response may still have created a replay.
        start_attempted = True
        replay = request(client, "POST", PREFIX, start)[1]["replay"]
        run_id = replay["id"]
        result.update(run_id=run_id, scenario=replay["scenario"], seed=replay["seed"], plan=replay["plan"])
        replay = wait_paused(client, replay)
        if playbook:
            identifier = playbook["id"]
            enabled = request(client, "PUT", PREFIX + f"/playbooks/{identifier}/mode", {"mode": "auto"})[1]
            actual = enabled["definitions"][identifier]["versions"][-1]
            result["playbook"] = dict(definition=actual, mode=enabled["modes"][identifier])
            if actual != playbook or result["playbook"]["mode"] != "auto":
                raise FuzzError("replay playbook definition/version or auto mode differs from setup")
        state = snapshot(client)
        if playbook:
            result["playbook"]["preview_refusals"] = playbook_preview_refusals(enabled, playbook, state["account"]["rules"])
        result["start_market_time"] = state["portfolio"]["time"]
        generator = Generator(client, random.Random(fuzz_seed), replay, ops)
        abandoned = Counter()
        issues, counters = check_invariants(state)
        result["checks"].update(counters)
        for item in issues:
            failure(**item)
        for step in range(1, ops + 1):
            op = copy.deepcopy(tape[step - 1]) if tape is not None else generator.draw(step, state)
            market_time = state["portfolio"]["time"]
            if tape is None:
                op["market_time"] = market_time
            elif op["market_time"] != market_time:
                failure("command_market_time", expected=op["market_time"], actual=market_time)
            result["commands"].append(copy.deepcopy(op))
            result["operations"][op["kind"]] += 1
            preview = None
            if op["preview"]:
                preview = request(client, "POST", op["path"] + "/preview", op["body"], result["preview_refusals"])[1]
                if preview.get("decision") not in (None, "ok"):
                    result["preview_refusals"][preview["decision"]] += 1
            status, response = request(client, op["method"], op["path"], op["body"], result["refusals"])
            accepted = status < 300 and (response.get("order") or op["kind"] == "stock")
            if accepted and preview and (preview.get("decision") == "BUYING_POWER" or
                                         (preview.get("reason") or {}).get("code") == "BUYING_POWER" or error_code(preview) == "BUYING_POWER"):
                failure("preview_buying_power", preview=preview, accepted=response)
            if status < 300 and op["kind"] == "abandon":
                abandoned[op["body"]["symbol"]] += next(p["quantity"] for p in state["portfolio"]["positions"] if p["symbol"] == op["body"]["symbol"])
            before, state = state, snapshot(client)
            result["outcomes"].append(dict(status=status, code=error_code(response), market_time=state["portfolio"]["time"],
                                           order_status=(response.get("order") or {}).get("status")))
            issues, counters = check_invariants(state, before, bool(accepted), abandoned)
            result["checks"].update(counters)
            for item in issues:
                failure(**item)
            if step % 25 == 0:
                out("run", run_id, "step", step, "time", state["portfolio"]["time"], "failures", len(result["failures"]))
        result["end_market_time"] = state["portfolio"]["time"]
        result["actors"] = actor_counts(state, playbook)
        if playbook and not result["actors"]["playbook"]["fills_by_actor"].get("system"):
            coverage = result["actors"]["playbook"]
            attempts = coverage["orders_by_actor"].get("system", 0)
            refused = sum(n for code, n in coverage["automatic_refusals"].items() if code in PLAN_REFUSALS)
            preview_refusals = result["playbook"]["preview_refusals"]
            if (attempts or preview_refusals) and refused == attempts:
                warning = dict(check="playbook_coverage", message="automatic playbook attempts refused by plan rules",
                               refusals=coverage["automatic_refusals"], preview_refusals=preview_refusals)
                result["warnings"].append(warning)
                out("WARN", dumps(warning).strip())
            else:
                failure("playbook_coverage", message="no automatic playbook fills", actors=result["actors"])
    except (FuzzError, KeyError, TypeError, ValueError, InvalidOperation, OSError) as exc:
        failure("execution", message=str(exc))
    finally:
        if start_attempted:
            try:
                request(client, "DELETE", PREFIX)
            except (FuzzError, OSError) as exc:
                failure("stop", message=str(exc))
    if run_id:
        op = None
        verify_saved(client, result, binary, directory, failure, out)
    return result


def fuzz(client, scenario="reversal", seed=81723, ops=300, fuzz_seed=1, plan="practice", repeat=False,
         binary=None, directory=None, out=print, playbook=False, restart=False):
    start = dict(scenario=scenario, seed=str(seed), plan=plan, paused=True)
    result = dict(url=client.base, scenario=scenario, seed=str(seed), fuzz_seed=fuzz_seed, ops=ops, plan=plan,
                  runs=[], determinism={"status": "not_requested"}, restart={"status": "not_requested"})
    try:
        if restart and (not binary or not directory):
            raise FuzzError("--restart-check requires --openportd and --replays-dir")
        definition = create_playbook(client) if playbook else None
        if definition:
            result["playbook"] = dict(definition=definition, mode="auto")
    except (FuzzError, KeyError, TypeError, ValueError, OSError) as exc:
        result.update(exit_status=1, setup_error=str(exc))
        out("FAIL setup", str(exc))
        return result
    first = run_once(client, start, ops, fuzz_seed, binary=binary, directory=directory, out=out, playbook=definition)
    result["runs"].append(first)
    if restart:
        result["restart"] = restart_check(client, first, binary, directory, out)
    if repeat:
        result["determinism"] = {"status": "failed", "message": "first run did not complete the command tape"}
        if len(first["commands"]) == ops and "history" in first:
            second = run_once(client, start, ops, fuzz_seed, tape=first["commands"], binary=binary,
                              directory=directory, out=out, playbook=definition)
            result["runs"].append(second)
            try:
                result["determinism"] = compare_runs(result["runs"], directory)
            except (FuzzError, OSError, KeyError) as exc:
                result["determinism"] = dict(status="failed", message=str(exc))
    result["exit_status"] = int(any(r["failures"] for r in result["runs"]) or result["determinism"]["status"] == "failed"
                                or result["restart"]["status"] == "failed")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("url")
    parser.add_argument("--scenario", default="reversal")
    parser.add_argument("--seed", type=int, default=81723)
    parser.add_argument("--ops", type=int, default=300)
    parser.add_argument("--fuzz-seed", type=int, default=1)
    parser.add_argument("--plan", default="practice")
    parser.add_argument("--repeat", action="store_true")
    parser.add_argument("--playbook", action="store_true", help="create a one-unit SPX time-window spread and enable replay auto")
    parser.add_argument("--restart-check", action="store_true",
                        help="restart at a recorded mid-run time; require the exact prefix, then only the restart input, and both verifiers")
    parser.add_argument("--token")
    parser.add_argument("--json", metavar="PATH", help="sorted JSON summary; - writes stdout")
    parser.add_argument("--openportd", metavar="BIN")
    parser.add_argument("--replays-dir", metavar="DIR")
    args = parser.parse_args(argv)
    if args.ops < 1 or not 0 <= args.seed < 2 ** 64:
        parser.error("--ops must be positive and --seed must fit uint64")
    if args.restart_check and (not args.openportd or not args.replays_dir):
        parser.error("--restart-check requires --openportd and --replays-dir")
    out = lambda *parts: print(*parts, file=sys.stderr if args.json == "-" else sys.stdout, flush=True)
    result = fuzz(Client(args.url, args.token), args.scenario, args.seed, args.ops, args.fuzz_seed, args.plan,
                  args.repeat, args.openportd, args.replays_dir, out, args.playbook, args.restart_check)
    for run in result["runs"]:
        out("run", run.get("run_id"), "operations", dict(run["operations"]), "refusals", dict(run["refusals"]),
            "preview refusals", dict(run["preview_refusals"]),
            "checks", dict(run["checks"]), "failures", len(run["failures"]), "warnings", len(run["warnings"]),
            "actors", run.get("actors", {}),
            "verify", run["verification"]["status"], "CLI", run["cli_verification"]["status"])
    out("determinism", result["determinism"], "exit", result["exit_status"])
    if args.restart_check:
        out("restart", result["restart"])
    if args.json:
        try:
            if args.json == "-":
                sys.stdout.write(dumps(result))
            else:
                Path(args.json).write_text(dumps(result), encoding="utf-8")
        except OSError as exc:
            out("FAIL writing summary:", exc)
            return 1
    return result["exit_status"]


if __name__ == "__main__":
    sys.exit(main())
