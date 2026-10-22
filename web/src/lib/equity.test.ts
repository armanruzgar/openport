import { describe, expect, it } from "vitest"
import type { EquitySample } from "../api/trading-types"
import { intradayEquity } from "./equity"
const sample: EquitySample = { time: "2026-09-22T14:00:00Z", day: "2026-09-22", attempt: 1, equity: "10000", floor: "9000", peak: "10000", target: "11000", tomorrow_floor: "9100", fill: null }
describe("intraday equity mapping", () => {
  it("maps recorded values, target, tomorrow's floor and ratchet markers", () => {
    const data = intradayEquity([sample, { ...sample, time: "2026-09-22T14:01:00Z", equity: "10200", floor: "9200" }], sample.day)
    expect(data.series.map((s) => s.id)).toEqual(["equity", "floor", "target", "tomorrow_floor"])
    expect(data.series[0]!.points.map((p) => p.y)).toEqual([10000, 10200])
    expect(data.series[1]!.points.map((p) => p.y)).toEqual([9000, 9000, 9200])
    expect(data.markers).toEqual([{ x: Date.parse("2026-09-22T14:01:00Z"), label: "Floor raised", color: "var(--bearish)" }])
    // Equity's samples are dots; the floor, target and tomorrow's floor draw as dashed lines,
    // with the old floor ending just before the raise so a readout there shows the new one.
    expect(data.series.map((s) => [s.id, !!s.dots, !!s.dashed])).toEqual([["equity", true, false], ["floor", false, true], ["target", false, true], ["tomorrow_floor", false, true]])
    expect(data.series[1]!.points.map((p) => p.x)).toEqual([Date.parse(sample.time), Date.parse("2026-09-22T14:01:00Z") - 1, Date.parse("2026-09-22T14:01:00Z")])
  })
  it("leaves gaps and separates attempts instead of inventing samples", () => {
    const data = intradayEquity([sample, { ...sample, time: "2026-09-22T14:20:00Z" }, { ...sample, time: "2026-09-22T14:21:00Z", attempt: 2 }], sample.day)
    expect(data.series[0]!.points.map((p) => p.y)).toEqual([10000, null, 10000, null, 10000])
    expect(intradayEquity([sample], "2026-09-23").series).toEqual([])
    expect(intradayEquity([sample], sample.day, 2).series).toEqual([])
  })
  it("keeps absent values absent and retains same-time fills", () => {
    const data = intradayEquity([{ ...sample, floor: null, target: null, tomorrow_floor: null }, { ...sample, floor: null, target: null, tomorrow_floor: null, fill: "2", equity: "9990" }], sample.day)
    expect(data.series).toHaveLength(1)
    expect(data.series[0]!.points.map((p) => p.y)).toEqual([10000, 9990])
  })
})
