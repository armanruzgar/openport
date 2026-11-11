# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Step a simulated scenario to 15:00 ET and paper trade its last hour."""
import argparse
import os
import time
from openport import Client
from put_spread import place_spread

def wait_until_ready(client, timeout=60):
    deadline = time.monotonic() + timeout
    while client.list_replays()["replay"]["fast_forwarding"]:
        if time.monotonic() >= deadline:
            raise TimeoutError("Replay did not finish preparing")
        time.sleep(0.1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", help="A scenario id from client.list_replays()['demos']")
    parser.add_argument("--seed", default="42")
    args = parser.parse_args()
    # A loopback server with open writes takes no token; any token it is sent must be valid.
    client = Client(os.environ.get("OPENPORT_URL", "http://127.0.0.1:8080"),
                    os.environ.get("OPENPORT_WRITE_TOKEN") or None, timeout=180)
    if client.list_replays()["replay"] is not None:
        raise ValueError("Stop the existing replay before running this example")
    client.start_replay(scenario=args.scenario, seed=args.seed, paused=True, plan="practice")
    try:
        wait_until_ready(client)
        print("SIMULATED SCENARIO:", client.step_replay("15:00"))
        replay = client.for_replay("main")
        place_spread(replay)
        print("SIMULATED SCENARIO:", client.step_replay("15:59"))
        print("PAPER FLATTEN:", replay.flatten())
        print("PAPER TRADES:", replay.trades())
    finally:
        client.stop_replay()


if __name__ == "__main__":
    main()
