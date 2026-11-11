import { describe, expect, it } from "vitest"
import { ApiError } from "../api/client"
import { errorText, marketDataError } from "./errors"

describe("error text", () => {
  it("shows a failure's message without its class name", () => {
    const error = new ApiError(404, "no data for SPX yet", "NOT_FOUND")
    expect(String(error)).toBe("ApiError: no data for SPX yet")
    expect(errorText(error)).toBe("no data for SPX yet")
    expect(errorText(new TypeError("Failed to fetch"))).toBe("Failed to fetch")
    expect(errorText("plain")).toBe("plain")
    expect(errorText(undefined)).toBe("Request failed. Please retry.")
  })
  it("waits for an underlying's first snapshot instead of reporting its 404", () => {
    const missing = new ApiError(404, "no data for SPX yet", "NOT_FOUND")
    expect(marketDataError(missing, "SPX", { as_of: null }, "connecting")).toBe("Waiting for the first SPX snapshot… (feed: connecting)")
    expect(marketDataError(missing, "SPX", undefined)).toBe("Waiting for the first SPX snapshot…")
    // Once it has a snapshot, a 404 is a failure like any other, and so is another error before it.
    expect(marketDataError(missing, "SPX", { as_of: "2026-11-06T14:31:30Z" }, "live")).toBe("no data for SPX yet")
    expect(marketDataError(new ApiError(500, "boom"), "SPX", { as_of: null }, "connecting")).toBe("boom")
  })
  it("waits while the server says an underlying is not ready yet", () => {
    const waiting = new ApiError(503, "No data for SPX yet; it is ready once GET /api/status shows it live", "NOT_READY")
    expect(marketDataError(waiting, "SPX", { as_of: null }, "connecting")).toBe("Waiting for the first SPX snapshot… (feed: connecting)")
    expect(marketDataError(waiting, "SPX", undefined)).toBe("Waiting for the first SPX snapshot…")
    // An underlying the server does not carry is no wait.
    const unknown = new ApiError(404, "ZZZ is not an underlying of this server; GET /api/status lists them", "UNKNOWN_UNDERLYING")
    expect(marketDataError(unknown, "ZZZ", { as_of: "2026-11-06T14:31:30Z" }, "live")).toBe(unknown.message)
  })
})
