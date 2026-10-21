import { expect, it } from "vitest"
import { riskHistory } from "./equity"
import type { EquitySample } from "../api/trading-types"
it("breaks Greek history at missing valuations, gaps and attempts", () => {
  const row: EquitySample = { time: "2026-09-22T14:00:00Z", day: "2026-09-22", attempt: 1, equity: "10000", floor: null, peak: "10000", target: null, tomorrow_floor: null, fill: null,
    risk: { dollar_delta: 200, dollar_gamma_1pct: 20, vega: 10, theta: -5, delta_utilisation: 0.2, vega_utilisation: 0.1 } }
  const series = riskHistory([row, { ...row, time: "2026-09-22T14:01:00Z", risk: null },
    { ...row, time: "2026-09-23T14:00:00Z", day: "2026-09-23", attempt: 2 }], "dollar_delta")
  expect(series[0]!.points.map((p) => p.y)).toEqual([200, null, null, 200])
  expect(riskHistory([row], "vega", "2026-09-23")).toEqual([])
})
