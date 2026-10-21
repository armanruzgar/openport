import { renderToStaticMarkup } from "react-dom/server"
import { expect, it } from "vitest"
import { ScenarioGrid } from "./ScenarioGrid"
it("shows unavailable cells and volatility clamps", () => {
  const html = renderToStaticMarkup(<ScenarioGrid scenarios={{ spot_percent: [0], vol_points: [0, 5], pnl: [[null, 12]], clamped: [[false, true]], complete: false }} />)
  expect(html).toContain("—")
  expect(html).toContain("$12.00*")
  expect(html).toContain("Scenario valuation incomplete")
})
