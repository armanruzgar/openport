// @vitest-environment jsdom
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import type { MarginUnderlying } from "../api/trading-types"
import { MarginBreakdown, marginLegLabel } from "./MarginBreakdown"

let root: Root
let host: HTMLDivElement
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  host = document.createElement("div")
  document.body.append(host)
  root = createRoot(host)
})
afterEach(async () => {
  await act(async () => root.unmount())
  host.remove()
  vi.unstubAllGlobals()
})
const rows = () => [...host.querySelectorAll("tbody tr")].map((tr) => [...tr.querySelectorAll("td")].map((td) => td.textContent))

describe("margin breakdown", () => {
  it("lists each part with its positions and adds a total for several", async () => {
    const margin: MarginUnderlying[] = [{
      underlying: "SPY", requirement: "1250.00", scan: null, parts: [
        { kind: "covered", legs: [{ symbol: "SPY   261022C00520000", quantity: -1 }, { symbol: "SPY", quantity: 100 }], requirement: "0.00" },
        { kind: "vertical", legs: [{ symbol: "SPY   261022P00490000", quantity: -1 }, { symbol: "SPY   261022P00480000", quantity: 1 }], requirement: "1000.00" },
        { kind: "naked", legs: [{ symbol: "SPY   261022P00450000", quantity: -1 }], requirement: "250.00" },
      ],
    }]
    await act(async () => root.render(<MarginBreakdown margin={margin} />))
    expect(rows()).toEqual([
      ["SPY", "Covered", "−1 SPY Oct 22 520C · +100 SPY shares", "$0.00"],
      ["", "Spread", "−1 SPY Oct 22 490P · +1 SPY Oct 22 480P", "$1,000.00"],
      ["", "Naked short", "−1 SPY Oct 22 450P", "$250.00"],
      ["", "Total", "$1,250.00"],
    ])
  })

  it("shows a portfolio scan's worst point, and a fallback that adds the minimum", async () => {
    await act(async () => root.render(<MarginBreakdown margin={[
      { underlying: "SPX", requirement: "9000.00", parts: [], scan: { loss: "9000.00", spot_percent: -8, vol_points: 5, minimum: "75.00" } },
      { underlying: "QQQ", requirement: "137.50", scan: null, parts: [{ kind: "vertical", legs: [], requirement: "100.00" }] },
    ]} />))
    const [scan, part, total] = rows()
    expect(scan).toEqual(["SPX", "Portfolio scan", "Worst at -8.0% spot, +5 vol points: loss $9,000.00, minimum $75.00", "$9,000.00"])
    expect(part![1]).toBe("Spread")
    expect(total).toEqual(["", "Total, with the portfolio-margin minimum until the scan is complete", "$137.50"])
  })

  it("checks totals before rounding individual parts to cents", async () => {
    await act(async () => root.render(<MarginBreakdown margin={[
      { underlying: "SPY", requirement: "300.008", scan: null, parts: [
        { kind: "naked", legs: [], requirement: "100.004" },
        { kind: "naked", legs: [], requirement: "200.004" },
      ] },
    ]} />))
    expect(rows().at(-1)).toEqual(["", "Total", "$300.01"])
    expect(host.textContent).not.toContain("until the scan is complete")
  })

  it("names a cash-secured put", async () => {
    await act(async () => root.render(<MarginBreakdown margin={[
      { underlying: "SPY", requirement: "49000.00", scan: null, parts: [
        { kind: "cash_secured", legs: [{ symbol: "SPY   261022P00490000", quantity: -1 }], requirement: "49000.00" },
      ] },
    ]} />))
    expect(rows()).toEqual([["SPY", "Cash-secured put", "−1 SPY Oct 22 490P", "$49,000.00"]])
  })

  it("shows a joint expiry pool with a later long", async () => {
    await act(async () => root.render(<MarginBreakdown margin={[
      { underlying: "SPY", requirement: "43000.00", scan: null, parts: [
        { kind: "worst_loss", legs: [
          { symbol: "SPY   261022P00490000", quantity: -2 },
          { symbol: "SPY   261022P00480000", quantity: -2 },
          { symbol: "SPY   261022P00510000", quantity: 2 },
          { symbol: "SPY   261029P00490000", quantity: 1 },
        ], requirement: "43000.00" },
      ] },
    ]} />))
    expect(rows()).toEqual([["SPY", "Worst loss at expiry",
      "−2 SPY Oct 22 490P · −2 SPY Oct 22 480P · +2 SPY Oct 22 510P · +1 SPY Oct 29 490P", "$43,000.00"]])
    expect(host.querySelector("td[title]")?.getAttribute("title")).toContain("longs expiring then or later")
  })

  it("distinguishes a proved minimum from the work-limit fallback and older runs", async () => {
    for (const [allocation, label] of [
      ["exact", "Exact minimum"],
      ["bounded", "Conservative margin: allocation work limit reached"],
      ["legacy", "Recorded margin method"],
    ] as const) {
      await act(async () => root.render(<MarginBreakdown margin={[
        { underlying: "SPY", requirement: "100.00", scan: null, allocation,
          parts: [{ kind: "naked", legs: [], requirement: "100.00" }] },
      ]} />))
      expect(host.textContent).toContain(label)
    }
  })

  it("labels shares and contracts", () => {
    expect(marginLegLabel({ symbol: "SPY", quantity: -200 }, "SPY")).toBe("−200 SPY shares")
    expect(marginLegLabel({ symbol: "SPXW  261022P04900000", quantity: 2 }, "SPX")).toBe("+2 SPX Oct 22 4900P")
  })
})
