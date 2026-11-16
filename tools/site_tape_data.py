#!/usr/bin/env python3
# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Write site/tape-data.js, the quote snapshot behind the landing page's tape.

Fetches Cboe's free delayed option chains for SPX, SPY and QQQ and keeps the
near-the-money quotes of the next two weekly expiries, or the ones passed with
--expiries. The page simulates prints at these quotes; it never fetches data.
The Pages workflow runs this after every close, so the tape's expiries stay ahead.
"""
from __future__ import annotations

import argparse
from datetime import date
import json
from pathlib import Path
import re
from urllib.request import Request, urlopen

URL = 'https://cdn-api.cboe.com/api/global/delayed_quotes/options/{}.json'
# Chain file, option root and strike band around the close.
SPECS = [('_SPX', 'SPXW', 60), ('SPY', 'SPY', 6), ('QQQ', 'QQQ', 6)]
OPTION = re.compile(r'([A-Z]+)(\d{6})([CP])(\d{8})$')


def fetch(name: str) -> dict:
    request = Request(URL.format(name), headers={'User-Agent': 'openport-site-tape'})
    with urlopen(request, timeout=60) as response:
        return json.load(response)


def weeklies(listed: list[str]) -> list[str]:
    """The last listed expiry of each week, kept when it falls on a Friday, or on a
    Thursday when that Friday is an exchange holiday: the expiries a tape trades most
    after 0DTE. Monday to Wednesday dailies are dropped."""
    weeks: dict[tuple[int, int], str] = {}
    for e in sorted(listed):
        d = date(2000 + int(e[:2]), int(e[2:4]), int(e[4:]))
        weeks[d.isocalendar()[:2]] = e
    return [e for e in weeks.values() if date(2000 + int(e[:2]), int(e[2:4]), int(e[4:])).weekday() in (3, 4)]


def snapshot(doc: dict, root: str, band: float, expiries: list[str] | None) -> dict:
    data = doc['data']
    close = data['close']
    listed = sorted({m.group(2) for o in data['options'] if (m := OPTION.match(o['option'])) and m.group(1) == root})
    chosen = expiries or weeklies(listed)[:2]
    quotes = []
    for o in data['options']:
        m = OPTION.match(o['option'])
        if not m or m.group(1) != root or m.group(2) not in chosen:
            continue
        strike = int(m.group(4)) / 1000
        if abs(strike - close) > band or o['bid'] <= 0 or o['ask'] <= o['bid'] or o['iv'] <= 0:
            continue
        quotes.append([m.group(2), strike, m.group(3), o['bid'], o['ask'], round(o['iv'], 4),
                       round(o['delta'], 4), round(o['gamma'], 5), int(o['volume'])])
    if not quotes:
        raise SystemExit(f'{root}: no usable quotes for expiries {chosen}')
    return {
        'spot': close,
        'asof': doc['timestamp'],
        'last': data['last_trade_time'],
        'quotes': quotes,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--expiries', nargs='+', metavar='YYMMDD', help='expiries to keep (default: next two weekly expiries)')
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'site' / 'tape-data.js')
    args = parser.parse_args()
    chains = {}
    for name, root, band in SPECS:
        chains[root] = snapshot(fetch(name), root, band, args.expiries)
        print(f"{root}: close {chains[root]['spot']}, {len(chains[root]['quotes'])} quotes")
    args.out.write_text(
        '/* Quote snapshot from Cboe delayed option chains; regenerate with tools/site_tape_data.py. */\n'
        'window.CHAINS = ' + json.dumps(chains, separators=(',', ':')) + ';\n')


if __name__ == '__main__':
    main()
