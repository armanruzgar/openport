# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Run the bracketed paper spread example on a paused synthetic replay."""
import argparse
import asyncio
import os
from openport import Client
from openport.bot import Strategy, run
from put_spread import place_spread


class ReplaySpread(Strategy):
    def on_start(self):
        self.client.step_replay("10:00")
        # Uses discovered contracts, previews and a stable client order ID.
        place_spread(self.client)

    def on_tick(self, tick):
        print("Recorded/synthetic source:", tick["provider"])
        self.client.step_replay("15:59")

    def on_fill(self, fill):
        print("PAPER FILL:", fill)

    def on_stop(self):
        print("Replay remains paused for inspection; use stop_replay to archive it.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", help="SPX scenario spanning 10:00–15:59 ET")
    args = parser.parse_args()
    client = Client(os.environ.get("OPENPORT_URL", "http://127.0.0.1:8080"), os.environ["OPENPORT_WRITE_TOKEN"])
    if client.list_replays().get("replay"):
        raise SystemExit("Stop the existing replay before starting this example")
    client.start_replay(scenario=args.scenario, paused=True)
    asyncio.run(run(ReplaySpread(), client.for_replay(), max_ticks=1))
