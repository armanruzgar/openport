# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Optional tabular views; money stays as its original decimal string."""


def _pandas():
    try:
        import pandas
    except ImportError as error:
        raise ImportError("Install openport[pandas] to use frame helpers") from error
    return pandas


def chain_frame(chain):
    rows = []
    for strike in chain["strikes"]:
        for side in ("call", "put"):
            if strike.get(side) is not None:
                rows.append({**strike[side], "strike": strike["strike"], "side": side,
                             "expiry": chain["expiry"]["id"], "as_of": chain.get("as_of")})
    return _pandas().DataFrame(rows)


def smile_frame(surface):
    return _pandas().DataFrame([
        {**point, "expiry": expiry["id"], "as_of": surface.get("as_of")}
        for expiry in surface["expiries"] for point in expiry["points"]
    ])


def trades_frame(trades):
    return _pandas().DataFrame(trades["trades"])


def exposure_frame(exposure):
    """One row per displayed strike/expiry, preserving missing cells and model context."""
    metrics = ("gex", "vex", "dex", "cex", "vomex")
    rows = []
    summary = exposure.get("exposure", {})
    for expiry in exposure["expiries"]:
        for index, strike in enumerate(exposure["strikes"]):
            row = {"strike": strike, "expiry": expiry["id"], "days": expiry.get("days"),
                   "as_of": exposure.get("as_of"), "convention": exposure.get("convention", "open_interest"),
                   "slice": exposure.get("slice", "all"), "available": summary.get("available"),
                   "unavailable_reason": summary.get("unavailable_reason")}
            for metric in metrics:
                values = expiry.get(metric, [])
                row[metric] = values[index] if index < len(values) else None
            rows.append(row)
    return _pandas().DataFrame(rows, columns=["strike", "expiry", "days", "as_of", "convention",
                                            "slice", "available", "unavailable_reason", *metrics])
