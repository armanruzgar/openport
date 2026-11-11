# Copyright (c) 2026 OpenPort contributors. MIT License.
"""A paper put credit spread with closing-net stop and take-profit brackets."""
import argparse
from decimal import Decimal, ROUND_CEILING
import os
import uuid
from openport import Client


def place_spread(client, symbol="SPX"):
    provider = client.status()["provider"]
    chain = client.chain(symbol)
    print("SIMULATED PRICES" if provider.get("simulated") else "Provider market data", provider["name"],
          f"delay={provider['delay_seconds']}s", f"as_of={chain['as_of']}", "PAPER TRADING")
    puts = [row for row in chain["strikes"] if row.get("put") and row["put"].get("tradable")
            and row["put"].get("delta") is not None and (row["put"].get("bid") or 0) > 0
            and (row["put"].get("ask") or 0) > 0]
    pairs = [(short, long) for short in puts for long in puts if long["strike"] < short["strike"]]
    if not pairs:
        raise ValueError("No quoted put vertical available")
    short, long = min(pairs, key=lambda pair: (abs(pair[0]["put"]["delta"] + 0.15), pair[0]["strike"] - pair[1]["strike"]))
    credit = Decimal(str(short["put"]["bid"])) - Decimal(str(long["put"]["ask"]))
    def price(value):
        return str((value / Decimal("0.10")).to_integral_value(rounding=ROUND_CEILING) * Decimal("0.10"))
    if credit <= Decimal("0.20"):
        raise ValueError("Displayed spread credit is too small for this bracket example")
    order = {"client_order_id": str(uuid.uuid4()), "quantity": 1, "type": "limit", "time_in_force": "day",
             "limit_price": price(-credit), "legs": [{"symbol": short["put"]["symbol"], "side": "sell"},
                                                     {"symbol": long["put"]["symbol"], "side": "buy"}],
             "bracket": {"stop_loss": {"trigger": {"source": "combo", "direction": "at_or_above", "level": price(credit * 2)}},
                         "take_profit": {"limit_price": price(credit / 2)}}}
    preview = client.preview_order(order)
    print("SIMULATED ORDER PREVIEW:", preview)
    if preview["decision"] != "ok":
        raise ValueError(f"Preview did not accept the spread: {preview['reason']}")
    result = client.place_order(order)
    print("PAPER ORDER:", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--account", required=True)
    args = parser.parse_args()
    # A loopback server with open writes takes no token; any token it is sent must be valid.
    token = os.environ.get("OPENPORT_WRITE_TOKEN") or None
    place_spread(Client(os.environ.get("OPENPORT_URL", "http://127.0.0.1:8080"), token, args.account))


if __name__ == "__main__":
    main()
