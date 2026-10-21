# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Optional tabular exports. Decimal money and IDs stay strings; null stays null."""
import json


def _pandas():
    try:
        import pandas
    except ImportError as error:
        raise ImportError("Install openport[pandas] to use frame helpers") from error
    return pandas


def _arrow():
    try:
        import pyarrow
    except ImportError as error:
        raise ImportError("Install openport[arrow] to use Arrow/Parquet helpers") from error
    return pyarrow


def _chain_rows(chain):
    rows = []
    for strike in chain["strikes"]:
        for side in ("call", "put"):
            if strike.get(side) is not None:
                rows.append({**strike[side], "strike": strike["strike"], "side": side,
                             "expiry": chain["expiry"]["id"], "as_of": chain.get("as_of")})
    return rows


def _rows(data, kind, section="days"):
    if kind == "chain":
        return _chain_rows(data)
    if kind in {"fills", "trades"}:
        return data[kind]
    if kind == "equity":
        return data["samples"]
    if kind == "backtest":
        if section not in {"days", "attempts"}:
            raise ValueError("backtest section must be days or attempts")
        report = data.get("report", data)
        if report is None or report.get("mode") == "sweep":
            raise ValueError("Select a completed variant report before exporting a sweep")
        provenance = {"simulated": report.get("simulated"), "label": report.get("label"), "status": report.get("status")}
        return [{**provenance, "index": index, **row} if row is not None else {**provenance, "index": index, "missing": True}
                for index, row in enumerate(report[section])]
    raise ValueError("kind must be fills, trades, equity, backtest or chain")


def _frame(rows):
    pandas = _pandas()
    frame = pandas.DataFrame(rows)
    for name in frame.columns:
        values = [row.get(name) for row in rows]
        if any(isinstance(value, str) for value in values) and all(value is None or isinstance(value, str) for value in values):
            frame[name] = pandas.array(values, dtype="string")
    return frame


def chain_frame(chain):
    return _frame(_chain_rows(chain))


def smile_frame(surface):
    return _frame([{**point, "expiry": expiry["id"], "as_of": surface.get("as_of")}
                   for expiry in surface["expiries"] for point in expiry["points"]])


def trades_frame(trades):
    return _frame(_rows(trades, "trades"))


def fills_frame(fills):
    return _frame(_rows(fills, "fills"))


def equity_frame(equity):
    return _frame(_rows(equity, "equity"))


def backtest_frame(report, *, section="days"):
    return _frame(_rows(report, "backtest", section))


def arrow_table(data, kind, *, section="days"):
    """Return a pyarrow.Table without importing pandas.

    Money, IDs and ISO timestamps remain strings. Numeric analytics remain numeric.
    Nested objects/arrays become canonical JSON text, retaining exact money strings
    and avoiding unstable struct schemas across snapshots. Schema metadata records
    this policy. Empty inputs produce an empty table, without invented columns.
    """
    arrow = _arrow()
    rows = _rows(data, kind, section)
    columns = list(dict.fromkeys(key for row in rows for key in row))
    arrays = {}
    for key in columns:
        values = [row.get(key) for row in rows]
        if any(isinstance(value, (dict, list)) for value in values):
            values = [json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)
                      if value is not None else None for value in values]
        arrays[key] = arrow.array(values, type=arrow.string() if any(isinstance(v, str) for v in values) else None)
    table = arrow.table(arrays)
    return table.replace_schema_metadata({b"openport.kind": kind.encode(), b"openport.money": b"decimal-string",
                                          b"openport.nested": b"json", b"openport.timestamps": b"original"})


def write_parquet(data, path, kind, *, section="days", **options):
    """Write fills/trades/equity/backtest/chain using the Arrow dtype policy."""
    table = arrow_table(data, kind, section=section)
    import pyarrow.parquet
    pyarrow.parquet.write_table(table, path, **options)
    return table.schema


def write_arrow(data, path, kind, *, section="days"):
    """Write an Arrow IPC file, with the same schema as write_parquet."""
    arrow = _arrow()
    table = arrow_table(data, kind, section=section)
    with arrow.OSFile(str(path), "wb") as output:
        with arrow.ipc.new_file(output, table.schema) as writer:
            writer.write_table(table)
    return table.schema
