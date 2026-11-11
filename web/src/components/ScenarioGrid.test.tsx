import { renderToStaticMarkup } from "react-dom/server"
import { expect, it } from "vitest"
import { ScenarioGrid } from "./ScenarioGrid"

/** What the cell for `spot`% and `vol` points shows, found by its title. */
function cell(html: string, spot: number, vol: number): string | undefined {
  return [...html.matchAll(/<td[^>]*title="([^"]*)"[^>]*>([^<]*)<\/td>/g)]
    .find((match) => match[1]!.startsWith(`${spot}% spot, ${vol} vol points: `))?.[2]
}
it("shows unavailable cells and volatility clamps", () => {
  const html = renderToStaticMarkup(<ScenarioGrid scenarios={{ spot_percent: [0], vol_points: [0, 5], pnl: [[null, 12]], clamped: [[false, true]], complete: false }} />)
  // The incomplete-valuation note names the dash too, so read the unavailable cell itself.
  expect(cell(html, 0, 0)).toBe("—")
  expect(html).toContain('title="0% spot, 0 vol points: unavailable · unchanged spot and vol"')
  expect(cell(html, 0, 5)).toBe("$12.00*")
  expect(html).toContain("Scenario valuation incomplete")
})
it("shows a zero P&L as money, not as unavailable", () => {
  const html = renderToStaticMarkup(<ScenarioGrid scenarios={{ spot_percent: [0], vol_points: [0, 5], pnl: [[0, -12]], clamped: [[false, false]], complete: true }} />)
  expect(cell(html, 0, 0)).toBe("$0.00")
  expect(cell(html, 0, 5)).toBe("−$12.00")
  expect(html).not.toContain("—")
})
