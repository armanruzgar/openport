import { renderToStaticMarkup } from "react-dom/server"
import { describe, expect, it } from "vitest"
import type { Attribution } from "../api/trading-types"
import { describeAttribution } from "../lib/attribution"
import { ClosedToday, RealizedCell } from "./PositionsView"
import { TripAttribution } from "./TradeReview"

const parts: Attribution = { delta: 120.123456, gamma: 4.5, vega: -30, theta: -12.25, other: 1.75, costs: -10.65, total: 73.473456 }

describe("P&L by Greek beyond held positions", () => {
  it("says when part of it fell back to other", () => {
    expect(describeAttribution(parts)).not.toContain("missing")
    expect(describeAttribution({ ...parts, fallback: true })).toContain("Greeks were missing at one end of a stretch")
  })
  it("lists contracts and shares closed today with their parts", () => {
    const html = renderToStaticMarkup(<ClosedToday closed={[
      { symbol: "SPXW  261016C07000000", kind: "option", underlying: "SPX", attribution: parts },
      { symbol: "SPY", kind: "shares", underlying: "SPY", attribution: { ...parts, delta: 5, gamma: 0, vega: 0, theta: 0, other: 0, costs: 0, total: 5 } },
    ]} />)
    for (const text of ["Closed today", "SPX Oct 16 7000C", "SPY shares", "+$73.47", "+$120.12", "−$30.00", "+$5.00"]) expect(html).toContain(text)
  })
  it("shows a round trip's own parts in the journal, and nothing for an older one", () => {
    const html = renderToStaticMarkup(<TripAttribution attribution={{ ...parts, fallback: true }} open />)
    for (const text of ["P&amp;L by Greek so far", "Delta", "+$120.12", "Total", "+$73.47", "Part of it is other"]) expect(html).toContain(text)
    expect(renderToStaticMarkup(<TripAttribution attribution={null} open={false} />)).toBe("")
  })
  it("puts the round trip's realized P&L beside the contract's lifetime", () => {
    const row = (lifetime?: { round_trips: number; realised: string; fees: string; net: string } | null) =>
      renderToStaticMarkup(<table><tbody><tr><RealizedCell realised="40.00" fees="0.65" lifetime={lifetime} /></tr></tbody></table>)
    const html = row({ round_trips: 3, realised: "120.00", fees: "3.25", net: "116.75" })
    expect(html).toContain("+$39.35")
    expect(html).toContain("lifetime +$116.75")
    expect(html).toContain("All 3 round trips this attempt")
    // One round trip, or an older server: only its own numbers.
    expect(row({ round_trips: 1, realised: "40.00", fees: "0.65", net: "39.35" })).not.toContain("lifetime")
    expect(row(undefined)).not.toContain("lifetime")
  })
  it("nets a sub-cent fee exactly, as every other net is formatted", () => {
    // 40.00 − 0.685 is 39.315, which rounds half up to +$39.32; in binary floating point it rounded to 39.31.
    const html = renderToStaticMarkup(<table><tbody><tr><RealizedCell realised="40.00" fees="0.685" /></tr></tbody></table>)
    expect(html).toContain("<div>+$39.32</div>")
    expect(html).toContain("realized +$40.00 before fees, fees $0.69, net +$39.32")
    expect(renderToStaticMarkup(<table><tbody><tr><RealizedCell realised="0.00" fees="0.65" /></tr></tbody></table>)).toContain("−$0.65")
  })
})
