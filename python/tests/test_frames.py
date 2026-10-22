# Copyright (c) 2026 OpenPort contributors. MIT License.
import json
from decimal import Decimal

import pandas
import pytest
from openport.frames import arrow_table, backtest_frame, fills_frame, write_arrow, write_parquet


@pytest.mark.parametrize("kind,data", [
    ("fills", {"fills": [{"id": "9007199254740993", "price": "123456789.123456", "fee": None},
                         {"id": "2", "price": "-0.000001", "fee": "0.00", "context": {"pnl": "0.01"}}]}),
    ("trades", {"trades": [{"net": "-0.01", "closed": None}]}),
    ("equity", {"samples": [{"equity": "123456789.123456", "time": None}]}),
    ("backtest", {"simulated": True, "label": "Synthetic fixture", "status": "completed", "days": [{"pnl": "-0.01", "trades": [{"net": "-0.01"}]}, None]}),
    ("chain", {"expiry": {"id": "expiry"}, "as_of": None,
               "strikes": [{"strike": 5000, "call": {"bid": 1.25, "ask": None}, "put": None}]}),
])
def test_columnar_roundtrip_preserves_money_nulls_and_late_columns(tmp_path, kind, data):
    # pyarrow is the optional openport[arrow] extra, so CI without it still runs the pandas tests.
    pa = pytest.importorskip("pyarrow")
    pq = pytest.importorskip("pyarrow.parquet")
    table = arrow_table(data, kind)
    parquet = tmp_path / "data.parquet"
    ipc = tmp_path / "data.arrow"
    write_parquet(data, parquet, kind)
    write_arrow(data, ipc, kind)
    assert pq.read_table(parquet).equals(table)
    with pa.memory_map(str(ipc), "r") as source:
        assert pa.ipc.open_file(source).read_all().equals(table)
    assert table.schema.metadata[b"openport.money"] == b"decimal-string"
    if kind == "backtest":
        assert table["simulated"].to_pylist() == [True, True]
        assert table["label"][0].as_py() == "Synthetic fixture"
    if kind == "fills":
        assert table["id"][0].as_py() == "9007199254740993"
        assert Decimal(table["price"][0].as_py()) == Decimal("123456789.123456")
        assert table["fee"][0].as_py() is None
        assert json.loads(table["context"][1].as_py()) == {"pnl": "0.01"}


def test_frames():
    frame = fills_frame({"fills": [{"price": "1.230000"}, {"price": None}]})
    assert str(frame.dtypes["price"]) == "string"
    assert frame.price[0] == "1.230000" and pandas.isna(frame.price[1])
    assert backtest_frame({"days": [None]}).iloc[0]["missing"]
    with pytest.raises(ValueError, match="variant"):
        backtest_frame({"mode": "sweep"})


def test_empty_exports(tmp_path):
    pytest.importorskip("pyarrow")
    assert arrow_table({"fills": []}, "fills").num_rows == 0
    write_parquet({"fills": []}, tmp_path / "empty.parquet", "fills")
    with pytest.raises(ValueError, match="kind"):
        arrow_table({}, "unknown")


def test_offline_example_notebook_runs():
    pytest.importorskip("pyarrow")
    from conftest import ROOT
    notebook = json.loads((ROOT / "python/examples/columnar.ipynb").read_text())
    namespace = {}
    for cell in notebook["cells"]:
        if cell["cell_type"] == "code":
            exec(compile("".join(cell["source"]), "columnar.ipynb", "exec"), namespace)
