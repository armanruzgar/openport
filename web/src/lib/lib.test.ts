import { describe, expect, it } from "vitest"
import { extent, labelRows, linear, niceTicks } from "../charts/scale"
import { flashDirection } from "./flash"
import { count, days, expiryLabel, fixed, money, price, vol } from "./format"
import { formatRoute, parseRoute } from "./route"

describe("route", () => {
  it("round-trips symbol, view and expiry through the hash", () => {
    const route = { symbol: "SPX", view: "chain" as const, expiry: "2026-10-16AM" }
    expect(parseRoute(formatRoute(route))).toEqual(route)
  })

  it("lands on the dashboard for anything unknown", () => {
    expect(parseRoute("#/spy/nonsense")).toEqual({ symbol: "SPY", view: "dashboard", expiry: null })
    expect(parseRoute("")).toEqual({ symbol: null, view: "dashboard", expiry: null })
  })

  it.each(["#/%/chain", "#/%E0%A4/chain", "#/SPX/chain/%", "#/SPX/chain/%FF"])("safely resets a malformed encoded route: %s", (hash) => {
    expect(parseRoute(hash)).toEqual({ symbol: null, view: "dashboard", expiry: null })
  })

  it.each(["constructor", "toString", "hasOwnProperty", "__proto__", "valueOf"])("lands on the dashboard for an object property name: %s", (view) => {
    expect(parseRoute(`#/SPX/${view}`)).toEqual({ symbol: "SPX", view: "dashboard", expiry: null })
  })

  it("keeps the old portfolio link", () => {
    expect(parseRoute("#/SPX/portfolio").view).toBe("positions")
  })

  it("still decodes valid escaped route segments", () => {
    expect(parseRoute("#/brk%2Fb/smile")).toEqual({ symbol: "BRK/B", view: "smile", expiry: null })
  })
})

describe("format", () => {
  it("never renders a missing value as zero", () => {
    for (const value of [null, undefined, Number.NaN]) {
      expect(price(value)).toBe("—")
      expect(count(value)).toBe("—")
      expect(fixed(value)).toBe("—")
      expect(vol(value)).toBe("—")
    }
  })

  it("signs compact money with the typographic minus, as signed money does", () => {
    expect(money(-2.65e9)).toBe("−$2.65B")
    expect(money(1_500)).toBe("$1.5K")
    expect(money(null)).toBe("—")
  })

  it("preserves received zero quotes and open interest", () => {
    expect(price(0)).toBe("0.00")
    expect(count(0)).toBe("0")
  })

  it("shows vol points and sub-dollar prices sensibly", () => {
    expect(vol(0.1575)).toBe("15.75")
    expect(price(0.05)).toBe("0.050")
    expect(price(41.5)).toBe("41.50")
  })

  it("labels expiries and time left", () => {
    expect(expiryLabel("2026-10-16AM")).toBe("Oct 16")
    expect(expiryLabel("2026-10-16AM", true)).toBe("Oct 16 AM")
    expect(days(0.5)).toBe("12.0h")
    expect(days(3.2)).toBe("3.2d")
  })
})

describe("scale", () => {
  it("maps linearly and inverts", () => {
    const x = linear([0, 10], [100, 200])
    expect(x(5)).toBe(150)
    expect(x.invert(150)).toBe(5)
  })

  it("picks round ticks", () => {
    expect(niceTicks(0, 1, 5)).toEqual([0, 0.2, 0.4, 0.6, 0.8, 1])
    expect(niceTicks(7600, 7900, 4)).toEqual([7600, 7700, 7800, 7900])
  })

  it("ends on a span only a few ulps wide, with one tick for values that print alike", () => {
    // A step below the doubles' spacing used to leave the tick in place and loop until out of memory.
    expect(niceTicks(0.3, 0.1 + 0.2, 5)).toEqual([0.3])
    expect(niceTicks(100, 100.00000000000001, 5)).toEqual([100])
    expect(niceTicks(7000, 7000.000000000001, 5)).toEqual([7000])
    expect(niceTicks(1, 1.0000000000000004, 5)).toEqual([1])
    expect(niceTicks(1, 1.0000000000000007, 5)).toEqual([1])
    expect(niceTicks(2, 1, 5)).toEqual([])
  })

  it("ignores missing values in extents", () => {
    expect(extent([3, null, 1, Number.NaN, 2])).toEqual([1, 3])
    expect(extent([null])).toBeNull()
  })

  it("stacks marker labels that would run into each other", () => {
    // "spot 7667.43" at 500 runs to about 576, so "flip" at 520 drops a row; one far off does not.
    expect(labelRows([{ x: 520, label: "flip" }, { x: 500, label: "spot 7667.43" }, { x: 700, label: "wall" }])).toEqual([1, 0, 0])
    expect(labelRows([{ x: 100, label: "a" }, { x: 104, label: "b" }, { x: 108, label: "c" }])).toEqual([0, 1, 2])
    expect(labelRows([{ x: 100, label: "a" }, { x: 106, label: "b" }, { x: 112, label: "c" }])).toEqual([0, 1, 0])
  })
})

describe("flash", () => {
  it("only flashes when an existing value moves", () => {
    expect(flashDirection(undefined, 1)).toBeNull()
    expect(flashDirection(1, 1)).toBeNull()
    expect(flashDirection(1, 2)).toBe("up")
    expect(flashDirection(2, 1)).toBe("down")
  })
})
