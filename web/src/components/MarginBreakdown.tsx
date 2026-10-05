import type { MarginLeg, MarginPart, MarginUnderlying } from "../api/trading-types"
import { osiLabel } from "../lib/journal"
import { fixed } from "../lib/format"
import { compareMoney, formatMoney, sumMoney } from "../lib/trading"

export const partLabels: Record<MarginPart["kind"], string> = {
  naked: "Naked short",
  vertical: "Spread",
  covered: "Covered",
  straddle: "Straddle or strangle",
  short_shares: "Short shares",
  protected_shares: "Protected short shares",
  worst_loss: "Worst loss at expiry",
  long: "Paid in full",
  cash_secured: "Cash-secured put",
}
const partHints: Record<MarginPart["kind"], string> = {
  naked: "Its buy-back value plus 100 × max(20% of spot − out-of-the-money amount, 10% of spot or strike), plus any house margin",
  vertical: "The short with the long that covers it: the strikes' distance, never more than naked",
  covered: "A short call against 100 long shares holds nothing more; a short put against 100 short shares its buy-back value",
  straddle: "The greater naked requirement plus the other side's buy-back value",
  short_shares: "Their value and half again, with any house percentage added to the half",
  protected_shares: "100 short shares with a long call: at most its strike",
  worst_loss: "Shorts expiring together with longs expiring then or later, held at their worst loss at the shorts' expiry",
  long: "Premium and shares are paid in full",
  cash_secured: "A cash account's or IRA's short put holds its strike in cash",
}

/** "−1 SPX Oct 22 4900P" or "+100 SPY shares". */
export function marginLegLabel(leg: MarginLeg, underlying: string): string {
  const sign = leg.quantity > 0 ? "+" : "−"
  const size = Math.abs(leg.quantity)
  return leg.symbol === underlying ? `${sign}${size} ${underlying} shares` : `${sign}${size} ${osiLabel(leg.symbol, underlying)}`
}

/** What holds each underlying's margin requirement: strategy parts, or the portfolio scan's worst point. */
export function MarginBreakdown({ margin }: { margin: MarginUnderlying[] }) {
  if (!margin.length) return <p className="text-sm text-muted">No positions hold margin.</p>
  return <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Margin breakdown">
    <table className="w-full text-left text-xs whitespace-nowrap">
      <thead className="text-[11px] uppercase tracking-wide text-muted"><tr>
        {["Underlying", "Held as", "Positions", "Requirement"].map((h, i) =>
          <th key={h} scope="col" className={`px-2 py-2 font-normal ${i === 3 ? "text-right" : ""}`}>{h}</th>)}
      </tr></thead>
      <tbody className="[&_td]:px-2 [&_td]:py-1.5 [&_td]:align-top">
        {margin.map((item) => {
          const rows = item.scan
            ? [<tr key="scan" className="border-t border-border/40">
                <td className="font-medium">{item.underlying}</td>
                <td title="The largest loss over the portfolio-margin price and volatility shocks, or the contract minimum if larger, plus any house margin">Portfolio scan</td>
                <td className="text-muted">Worst at {fixed(item.scan.spot_percent, 1)}% spot{item.scan.vol_points ? `, ${item.scan.vol_points > 0 ? "+" : ""}${fixed(item.scan.vol_points, 0)} vol points` : ""}:
                  {" "}loss {formatMoney(item.scan.loss)}, minimum {formatMoney(item.scan.minimum)}</td>
                <td className="text-right tabular">{formatMoney(item.requirement)}</td>
              </tr>]
            : item.parts.map((part, i) => <tr key={i} className={i === 0 ? "border-t border-border/40" : ""}>
                <td className="font-medium">{i === 0 ? item.underlying : ""}</td>
                <td title={partHints[part.kind]}>{partLabels[part.kind]}</td>
                <td className="text-muted">{part.legs.map((leg) => marginLegLabel(leg, item.underlying)).join(" · ")}</td>
                <td className="text-right tabular">{formatMoney(part.requirement)}</td>
              </tr>)
          const summed = !item.scan && item.parts.length > 1
          const fallback = !item.scan && compareMoney(sumMoney(item.parts.map((part) => part.requirement)), item.requirement) !== 0
          return [...rows,
            ...(!item.scan && item.allocation ? [<tr key="allocation">
              <td /><td colSpan={3} className="text-muted">{item.allocation === "exact" ? "Exact minimum" : item.allocation === "bounded" ? "Conservative margin: allocation work limit reached" : "Recorded margin method"}</td>
            </tr>] : []),
            ...(summed || fallback || !rows.length ? [<tr key="total" className={rows.length ? "" : "border-t border-border/40"}>
              <td className="font-medium">{rows.length ? "" : item.underlying}</td>
              <td colSpan={2} className="text-muted">{fallback ? "Total, with the portfolio-margin minimum until the scan is complete" : "Total"}</td>
              <td className="text-right font-medium tabular">{formatMoney(item.requirement)}</td>
            </tr>] : [])]
        })}
      </tbody>
    </table>
  </div>
}
