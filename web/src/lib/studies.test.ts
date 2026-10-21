import { expect, it } from "vitest"
import { indicators, studyPrefs } from "./studies"
const time = Date.parse("2026-10-16T14:00:00Z") / 1000
const bars = (values: number[]) => values.map((c, i) => ({ t: time + i * 60, o: c, h: c, l: c, c, v: 10 }))
it("warms up SMA and seeds EMA from the first complete SMA", () => {
  const result = indicators(bars([1, 2, 3, 7]), 3)
  expect(result.map((p) => p.sma)).toEqual([null, null, 2, 4])
  expect(result.map((p) => p.ema)).toEqual([null, null, 2, 4.5])
  expect(indicators([], 20)).toEqual([])
})
it("uses population standard deviation for two-sigma Bollinger bands", () => {
  const last = indicators(bars([1, 2, 3]), 3).at(-1)!
  expect(last.upper).toBeCloseTo(2 + 2 * Math.sqrt(2 / 3))
  expect(last.lower).toBeCloseTo(2 - 2 * Math.sqrt(2 / 3))
  expect(indicators(bars([5, 5, 5]), 3).at(-1)).toMatchObject({ sma: 5, upper: 5, lower: 5 })
})
it("weights VWAP by volume, resets daily, and never substitutes unknown volume", () => {
  const source = bars([10, 20, 30]); source[1]!.v = 30; source[2]!.t += 86400
  expect(indicators(source, 2).map((p) => p.vwap)).toEqual([10, 17.5, 30])
  expect(indicators([{ ...source[0]!, v: 0 }], 2)[0]?.vwap).toBeNull()
  const missing = [{ ...source[0]!, v: undefined }, source[1]!, source[2]!]
  expect(indicators(missing).map((p) => p.vwap)).toEqual([null, null, 30])
})
it("validates study preferences and breaks the warmup over invalid closes", () => {
  expect(studyPrefs({ period: 1, sma: true })).toMatchObject({ period: 20, sma: true, ema: false })
  expect(indicators(bars([1, NaN, 3, 4]), 2).map((p) => p.sma)).toEqual([null, null, null, 3.5])
})
