#!/usr/bin/env python3
"""Soak test: hold an SPX iron condor with far wings through a demo-market day and
check, every few seconds, that the account can still trade.

    tools/demo_soak.py URL [SPEED] [PLAN] [--seed SEED] [--token TOKEN] [--timeout SECONDS] [--json [PATH]]

URL is an openportd to test, such as one started for it with
`openportd --port 8094 --no-history --paper-journal /tmp/soak/paper.jsonl`. The soak
replaces any replay running there and trades only the replay's own in-memory account.
SPEED is the replay's speed (default 120), PLAN its plan (default practice) and SEED
the demo day's seed (default: one the server picks). The report prints the replay
run id and seed, so a saved run can be found and the market can be repeated.
TOKEN is sent as a Bearer credential, for a server with a write token or
--require-token; without one, only a loopback server with open writes accepts the soak.
--json writes a sorted JSON summary to PATH; omit PATH or use - for stdout, with
human progress on stderr. Without --json, text output is unchanged. The JSON
includes `run_id`, market times, leg symbols, all counters and exit_status. Probe
timing still depends on playback and HTTP scheduling.
--timeout sets the HTTP timeout in seconds (default 180), including scenario
generation, replay stepping and pausing while the current batch finishes. It also
bounds the wait for SPX to accept orders once the replay starts.

Each probe pauses the replay, places a limit buy of the short put inside the price
band and under the ask, cancels it and resumes, so it asks whether the account can
trade at all without trading. A probe that fills all the same is reported and sold
back, keeping the condor whole. The report counts refused probes by code, probe fills
and portfolio snapshots with incomplete marks; a healthy build refuses only by the
account's own risk rules and fills no probe. The exit status is 1 when a probe filled.
A soak that cannot start, cannot get SPX accepting orders or cannot fill every condor
leg stops before probing, saying why, with exit status 2 and no summary.
"""
import argparse
import json
import math
import sys
import time
import urllib.error
import urllib.request


class SoakError(RuntimeError):
    """The soak could not set up what it measures."""


class Client:
    def __init__(self, base, timeout=180, token=None):
        self.base = base.rstrip("/")
        self.timeout = timeout
        self.token = token

    def call(self, method, path, body=None):
        data = None if body is None else json.dumps(body).encode()
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        request = urllib.request.Request(self.base + path, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                return response.status, json.loads(response.read() or b"null")
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read() or b"null")


def code(body):
    return (body or {}).get("error", {}).get("code") if isinstance(body, dict) else None


def spx_tick(cents):
    """SPX options trade in nickels under $3 and dimes from $3."""
    return 5 if cents < 300 else 10


def probe_price(bid, ask, band_absolute=0.50, band_relative=0.20):
    """A buy limit that cannot trade against this quote and passes the price band:
    the highest tick at or under the bid, and under the ask, no further from the mid
    than the band allows. None when the band leaves no such price."""
    bid_cents, ask_cents = round(bid * 100), round(ask * 100)
    if bid_cents <= 0 or ask_cents <= bid_cents:
        return None
    mid = (bid_cents + ask_cents) / 2
    lowest = math.ceil(mid - max(band_absolute * 100, band_relative * mid) - 1e-9)
    price = bid_cents - bid_cents % spx_tick(bid_cents)
    while price < lowest:
        # A wide quote's bid can sit outside the band: bid at the band's edge instead.
        price += spx_tick(price) - price % spx_tick(price)
    if price <= 0 or price >= ask_cents:
        return None
    return price / 100


def probe(client, number, symbol, bid, ask, limits):
    """One probe while the replay is paused: "ok", "skipped", "finished", ("refused",
    code) or ("filled", contracts, restored)."""
    price = probe_price(bid, ask, float(limits.get("price_band_absolute", 0.5)),
                        float(limits.get("price_band_relative", 0.2)))
    if price is None:
        return "skipped"
    status, body = client.call("POST", "/api/replay/orders", {
        "client_order_id": f"probe-{number}", "symbol": symbol, "side": "buy", "type": "limit",
        "quantity": 1, "limit_price": f"{price:.2f}", "time_in_force": "day"})
    if status != 201:
        # The day can end between reading the replay and sending the probe.
        return "finished" if code(body) == "REPLAY_READ_ONLY" else ("refused", code(body))
    order = body["order"]
    if order["status"] in ("working", "partially_filled"):
        _, cancelled = client.call("DELETE", f"/api/replay/orders/{order['id']}")
        order = (cancelled or {}).get("order", order) if isinstance(cancelled, dict) else order
    filled = int(order.get("filled_quantity") or 0)
    if not filled:
        return "ok"
    # Sell back what the probe bought, so the condor's short put stays short.
    status, body = client.call("POST", "/api/replay/orders", {
        "client_order_id": f"restore-{number}", "symbol": symbol, "side": "sell", "type": "market",
        "quantity": filled, "time_in_force": "ioc"})
    restored = int(body.get("order", {}).get("filled_quantity") or 0) if status == 201 else 0
    return ("filled", filled, restored)


def soak(client, speed=120, plan="practice", seed=None, out=print, pause=1.0):
    """Run the soak and return its summary."""
    client.call("DELETE", "/api/replay")
    start = {"demo": "reversal", "speed": 30, "plan": plan}
    if seed is not None:
        start["seed"] = str(seed)
    status, body = client.call("POST", "/api/replay", start)
    if status not in (200, 201):
        raise SoakError(f"POST /api/replay: HTTP {status} {body}")
    identity = body["replay"]
    out("demo day", identity.get("scenario"), "run", identity.get("id"), "seed", identity.get("seed"), "plan", plan, "speed", speed)
    deadline = time.monotonic() + client.timeout
    while True:
        status, st = client.call("GET", "/api/replay/status")
        if status != 200:
            raise SoakError(f"GET /api/replay/status: HTTP {status} {st}")
        spx = next((u for u in st["underlyings"] if u["symbol"] == "SPX"), None)
        if spx and spx["options"] > 0 and spx["paper"]["accepting"]:
            break
        if time.monotonic() >= deadline:
            raise SoakError(f"SPX did not accept orders within {client.timeout:g} s: {spx}")
        time.sleep(0.5)

    _, initial_portfolio = client.call("GET", "/api/replay/portfolio")

    # The condor: the second expiry, shorts near 10 delta, wings near 3 delta.
    _, summary = client.call("GET", "/api/replay/underlyings/SPX/summary")
    expiries = [e["id"] for e in summary["expiries"]]
    expiry = expiries[1] if len(expiries) > 1 else expiries[0]
    _, chain = client.call("GET", f"/api/replay/underlyings/SPX/chain?expiry={expiry}")
    rows = chain["strikes"]

    def pick(kind, target):
        best = None
        for row in rows:
            leg = row.get(kind)
            if not leg or leg.get("delta") is None or not leg.get("ask"):
                continue
            distance = abs(abs(leg["delta"]) - target)
            if best is None or distance < best[0]:
                best = (distance, leg)
        return best[1]

    legs = {"short put": pick("put", 0.10), "long put": pick("put", 0.03),
            "short call": pick("call", 0.10), "long call": pick("call", 0.03)}
    out("expiry", expiry)
    for name, leg in legs.items():
        out(f"  {name:10s} {leg['symbol']}  bid {leg['bid']} ask {leg['ask']}  delta {leg['delta']:.3f}")
    missing = []
    for name, side in (("long put", "buy"), ("long call", "buy"), ("short put", "sell"), ("short call", "sell")):
        status, body = client.call("POST", "/api/replay/orders", {
            "client_order_id": f"condor-{name.replace(' ', '-')}", "symbol": legs[name]["symbol"], "side": side,
            "type": "market", "quantity": 1, "time_in_force": "ioc"})
        out(f"  {side} {name}: {status} {body.get('order', {}).get('status') if status < 300 else body}")
        if status >= 300 or int(body.get("order", {}).get("filled_quantity") or 0) != 1:
            missing.append(f"{name} ({code(body) or (body or {}).get('order', {}).get('status')})")
    # Without the whole condor, every probe would be an opening buy, not one against the held short.
    if missing:
        raise SoakError("the condor did not form: " + ", ".join(missing) + " did not fill")
    _, risk = client.call("GET", "/api/replay/risk")
    limits = (risk or {}).get("limits") or {}

    # Play the rest of the day, probing as it goes.
    status, body = client.call("PUT", "/api/replay", {"speed": speed})
    assert status == 200, (status, body)
    symbol = legs["short put"]["symbol"]
    result = {"probes": 0, "skipped": 0, "refused": {}, "probe_fills": 0, "restored": 0,
              "incomplete": 0, "samples": 0, "worst_age": 0.0, "ended": None}
    while True:
        # Paused, the book cannot move between reading the quote and the probe.
        client.call("PUT", "/api/replay", {"paused": True})
        _, replay = client.call("GET", "/api/replay")
        state = replay["replay"]
        _, portfolio = client.call("GET", "/api/replay/portfolio")
        result["samples"] += 1
        if not portfolio.get("valuation_complete", True):
            result["incomplete"] += 1
            if result["incomplete"] <= 3 or result["incomplete"] % 40 == 0:
                stale = [(pos["symbol"].split()[-1], pos.get("fresh"), pos.get("mark_age_seconds"), pos.get("awaiting_settlement"))
                         for pos in portfolio.get("positions", []) if not pos.get("fresh")]
                out("  incomplete at", portfolio.get("time"), "flags", portfolio.get("quality_flags"), "stale", stale,
                    "stocks", [(st.get("symbol"), st.get("fresh")) for st in portfolio.get("stocks", [])])
        for position in portfolio.get("positions", []):
            result["worst_age"] = max(result["worst_age"], position.get("mark_age_seconds") or 0)
        result["ended"] = portfolio.get("time")
        if state.get("finished"):
            break
        _, chain = client.call("GET", f"/api/replay/underlyings/SPX/chain?expiry={expiry}")
        quote = next((row["put"] for row in chain.get("strikes", [])
                      if (row.get("put") or {}).get("symbol") == symbol), None)
        if quote and quote.get("bid") and quote.get("ask"):
            outcome = probe(client, result["probes"] + 1, symbol, quote["bid"], quote["ask"], limits)
            if outcome == "finished":
                break
            if outcome == "skipped":
                result["skipped"] += 1
            else:
                result["probes"] += 1
            if isinstance(outcome, tuple) and outcome[0] == "refused":
                result["refused"][outcome[1]] = result["refused"].get(outcome[1], 0) + 1
            if isinstance(outcome, tuple) and outcome[0] == "filled":
                result["probe_fills"] += outcome[1]
                result["restored"] += outcome[2]
                out(f"  probe-{result['probes']} filled {outcome[1]} at", portfolio.get("time"), f"(sold back {outcome[2]})")
        client.call("PUT", "/api/replay", {"paused": False})
        time.sleep(pause)

    _, portfolio = client.call("GET", "/api/replay/portfolio")
    held = {position["symbol"]: position["quantity"] for position in portfolio.get("positions", [])}
    result["positions"] = len(held)
    result["condor_intact"] = all(held.get(legs[name]["symbol"]) == (-1 if name.startswith("short") else 1) for name in legs)
    out("ended at", result["ended"])
    out("positions held:", result["positions"], "| condor intact:", "yes" if result["condor_intact"] else "no",
        "| equity", portfolio.get("equity"))
    out(f"probes {result['probes']}, refused {sum(result['refused'].values())} {result['refused']}, "
        f"filled {result['probe_fills']} (sold back {result['restored']}), skipped {result['skipped']}")
    out(f"portfolio samples {result['samples']}, with incomplete marks {result['incomplete']}, "
        f"oldest mark {result['worst_age']:.0f} s")
    result.update(url=client.base, speed=speed, plan=plan, run_id=identity.get("id"), seed=identity.get("seed"),
                  scenario=identity.get("scenario"), expiry=expiry,
                  legs={name: leg["symbol"] for name, leg in legs.items()},
                  start_market_time=initial_portfolio.get("time"), end_market_time=portfolio.get("time"),
                  exit_status=1 if result["probe_fills"] else 0)
    return result


def write_json(result, destination):
    """Write one stable, newline-terminated summary; - selects stdout."""
    text = json.dumps(result, sort_keys=True, indent=2, allow_nan=False) + "\n"
    if destination == "-":
        sys.stdout.write(text)
    else:
        with open(destination, "w", encoding="utf-8") as output:
            output.write(text)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Soak an openportd demo replay with a held condor.")
    parser.add_argument("url")
    parser.add_argument("speed", nargs="?", type=int, default=120)
    parser.add_argument("plan", nargs="?", default="practice")
    parser.add_argument("--seed", type=int, help="demo day seed; the server picks one when omitted")
    parser.add_argument("--token", help="Bearer token for a server with a write token or --require-token")
    parser.add_argument("--timeout", type=float, default=180, metavar="SECONDS",
                        help="HTTP timeout, including replay generation and stepping (default: 180 seconds)")
    parser.add_argument("--json", nargs="?", const="-", metavar="PATH",
                        help="write JSON summary to PATH (default: stdout, progress on stderr)")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be a finite positive number of seconds")
    out = (lambda *parts: print(*parts, file=sys.stderr)) if args.json == "-" else print
    try:
        result = soak(Client(args.url, timeout=args.timeout, token=args.token), args.speed, args.plan, args.seed, out=out)
    except SoakError as error:
        print(f"demo_soak: {error}", file=sys.stderr)
        return 2
    if args.json is not None:
        write_json(result, args.json)
    return result["exit_status"]


if __name__ == "__main__":
    sys.exit(main())
