import { renderToStaticMarkup } from "react-dom/server"
import { expect, it } from "vitest"
import { StressResultPanel } from "./StressPanel"
it("labels derived stress, missing factors and sparse tail estimates", () => {
  const tail = { complete: false, observations: 12, var95: null, es95: null, var99: null, es99: null, tail99_reason: "Needs at least 100 observations; 12 available", first: null, last: null }
  const html = renderToStaticMarkup(<StressResultPanel data={{ account_version: "1", time: "2026-09-22T14:00:00Z", model: "derived",
    sticky_strike: tail, atm_vol_change: tail, stress: [{ id: "covid", name: "Mar 2020 COVID", from: "2020-03-13", to: "2020-03-16",
      label: "derived from public closes", source_note: "Tenor proxy", sources: ["https://www.cboe.com/"], complete: false,
      missing_underlyings: ["XYZ"], pnl: null, floor_breach: null, margin: null, clamped: false }] }} />)
  expect(html).toContain("20 aligned daily moves")
  expect(html).toContain("derived from public closes")
  expect(html).toContain("No shock for XYZ")
  expect(html).toContain("—")
})
it("shows 95% figures with a reason when the 99% tail is too thin", () => {
  const tail = { complete: true, observations: 40, var95: 120, es95: 150, var99: null, es99: null,
    tail99_reason: "Needs at least 100 observations; 40 available", first: "2026-07-01", last: "2026-08-26" }
  const html = renderToStaticMarkup(<StressResultPanel data={{ account_version: "1", time: "2026-09-22T14:00:00Z", model: "derived",
    sticky_strike: tail, atm_vol_change: tail, stress: [] }} />)
  expect(html).toContain("99% unavailable: Needs at least 100 observations; 40 available.")
  expect(html).toContain("VaR 99%")
})
it("leaves out floor and margin for an analysis portfolio, which is not an account", () => {
  const tail = { complete: true, observations: 29, var95: 80, es95: 95, var99: null, es99: null,
    tail99_reason: "Needs at least 100 observations; 29 available", first: "2026-07-01", last: "2026-08-11" }
  const html = renderToStaticMarkup(<StressResultPanel account={false} data={{ sticky_strike: tail, atm_vol_change: tail,
    stress: [{ id: "covid", name: "Mar 2020 COVID", from: "2020-03-13", to: "2020-03-16", label: "derived from public closes",
      source_note: "Tenor proxy", sources: [], complete: true, missing_underlyings: [], pnl: -412.5, clamped: false }] }} />)
  expect(html).toContain("Mar 2020 COVID")
  expect(html).not.toContain("Margin after")
  expect(html).not.toContain("Floor breach")
  expect(html).not.toContain("working orders")
})
