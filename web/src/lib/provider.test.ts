import { describe, expect, it } from "vitest"
import type { FeedState, ProviderInfo } from "../api/types"
import { fallbackLabel, fallbackTitle, providerLabel } from "./provider"

const provider: ProviderInfo = {
  name: "test", realtime: true, delay_seconds: 900,
  trades: false, open_interest: true, vendor_greeks: false,
}

describe("provider label", () => {
  it("uses the feed state for plan-dependent providers regardless of the static capability", () => {
    for (const realtime of [true, false]) {
      const plan = { ...provider, realtime, realtime_plan_dependent: true }
      expect(providerLabel(plan, "live")).toBe("real-time")
      expect(providerLabel(plan, "delayed")).toBe("15-min delay")
      expect(providerLabel({ ...plan, delay_seconds: 0 }, "delayed")).toBe("delayed")
    }
  })

  it("does not claim real-time data while a plan-dependent feed's timing is unknown", () => {
    const states: (FeedState | null | undefined)[] = ["connecting", "stale", "error", "stopped", null, undefined]
    for (const state of states) {
      expect(providerLabel({ ...provider, realtime_plan_dependent: true }, state)).toBe("timing unconfirmed")
    }
  })

  it("calls the demo market's prices simulated, whatever the feed state", () => {
    for (const state of ["live", "delayed", null] as const) expect(providerLabel({ ...provider, name: "replay (demo)", simulated: true }, state)).toBe("simulated prices")
  })

  it("preserves capability-based labels for older and fixed-plan providers", () => {
    for (const realtime_plan_dependent of [false, null, undefined]) {
      expect(providerLabel({ ...provider, realtime_plan_dependent }, "delayed")).toBe("real-time")
      expect(providerLabel({ ...provider, realtime_plan_dependent, realtime: false }, "live")).toBe("15-min delay")
    }
  })
})

describe("fallback label", () => {
  const fallback = { primary: "tradier", backup: "cboe", after_seconds: 30 }
  const spx = { symbol: "SPX", spot: 7400, as_of: null, version: 1 }

  it("names the backup only while it supplies the underlying", () => {
    expect(fallbackLabel({ ...provider, fallback }, { ...spx, feed: "cboe", fallback: true })).toBe("fallback · cboe")
    expect(fallbackLabel({ ...provider, fallback }, { ...spx, feed: "tradier", fallback: false })).toBeNull()
    expect(fallbackLabel(provider, { ...spx, fallback: true })).toBeNull()
    expect(fallbackLabel({ ...provider, fallback: null }, spx)).toBeNull()
  })

  it("explains when the backup took over and that its data is delayed", () => {
    expect(fallbackTitle({ ...provider, fallback })).toContain("tradier has been stale, in error or silent for 30 s")
    expect(fallbackTitle({ ...provider, fallback })).toContain("reported as delayed")
    expect(fallbackTitle(provider)).toBe("")
  })
})
