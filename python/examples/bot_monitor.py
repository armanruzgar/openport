# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Read a paper account; use --history RUN to walk a saved run without trading."""
import argparse
import asyncio
import os
from openport import Client
from openport.bot import Strategy, run


class Monitor(Strategy):
    def on_tick(self, tick):
        print("Paper account observation:", tick)

    def on_bar(self, symbol, bar):
        print("Observed closed bar:", symbol, bar)

    def on_fill(self, fill):
        print("Paper fill:", fill)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--account", default="main")
    parser.add_argument("--history")
    args = parser.parse_args()
    client = Client(os.environ.get("OPENPORT_URL", "http://127.0.0.1:8080"), os.getenv("OPENPORT_WRITE_TOKEN"), args.account)
    if args.history:
        client = client.for_history(args.history)
    asyncio.run(run(Monitor(), client, symbols=("SPX",)))
