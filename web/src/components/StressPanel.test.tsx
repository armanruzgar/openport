import { renderToStaticMarkup } from "react-dom/server"
import { expect, it } from "vitest"
import { StressResultPanel } from "./StressPanel"
it("labels derived stress, missing factors and sparse tail estimates", () => {
  const tail = { complete: false, observations: 12, var95: null, es95: null, var99: null, es99: null, first: null, last: null }
  const html = renderToStaticMarkup(<StressResultPanel data={{ account_version: "1", time: "2026-09-22T14:00:00Z", model: "derived",
    sticky_strike: tail, atm_vol_change: tail, stress: [{ id: "covid", name: "Mar 2020 COVID", from: "2020-03-13", to: "2020-03-16",
      label: "derived from public closes", source_note: "Tenor proxy", sources: ["https://www.cboe.com/"], complete: false,
      missing_underlyings: ["XYZ"], pnl: null, floor_breach: null, margin: null, clamped: false }] }} />)
  expect(html).toContain("20 aligned daily moves")
  expect(html).toContain("derived from public closes")
  expect(html).toContain("No shock for XYZ")
  expect(html).toContain("—")
})
