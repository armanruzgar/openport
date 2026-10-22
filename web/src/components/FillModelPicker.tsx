import type { FillModel } from "../api/trading-types"

/** The program-rule part of a fill choice, merged into a plan's rules where the account's terms are compared. */
export function fillProgramRules(model: FillModel, stress: number) {
  return { ...(model === "queue" ? { queue_fills: true } : {}), ...(stress > 0 ? { stop_stress_ticks: stress, stop_stress_percent: 200 } : {}) }
}

export function FillModelPicker({ value, onChange, disabled, stress, onStress }: {
  value: FillModel; onChange: (value: FillModel) => void; disabled?: boolean
  /** Stop stress's extra ticks, when the caller offers it. */
  stress?: number; onStress?: (ticks: number) => void
}) {
  return <>
    <label className="block space-y-1 text-sm"><span>Simulated fills</span>
      <select className="trade-input w-full" value={value} disabled={disabled} onChange={(e) => onChange(e.target.value as FillModel)}>
        <option value="as_displayed">As displayed</option>
        <option value="conservative">Conservative</option>
        <option value="midpoint">Inside at midpoint (50%)</option>
        <option value="queue">Queue position (trade prints)</option>
      </select>
    </label>
    <p className="text-xs text-muted">{value === "conservative"
      ? "For delayed feeds: wait 1,000 ms on market time, add 1 tick of slippage, and 1 extra tick for each additional displayed-size block. The next available quote may arrive much later."
      : value === "midpoint" ? "Limits at or beyond halfway across the spread fill at their limit inside it, up to the displayed size left. Marketable orders use the bid or ask."
        : value === "queue" ? "A resting limit at the bid or ask waits behind the size shown there when it joins. Trade prints at its price use that queue up before it fills at its limit; without prints, volume increases or a five-minute turnover stand in. A seeded estimate, not your real place in an exchange's queue."
          : "Fill immediately at the displayed bid or ask, up to the available displayed size, with no slippage."}
      {" "}Applies to this account’s new attempt.{value === "queue" ? "" : " These models do not simulate queue position or hidden liquidity."}</p>
    {onStress && <label className="block space-y-1 text-sm"><span>Stop stress</span>
      <select className="trade-input w-full" aria-label="Stop stress" value={stress ?? 0} disabled={disabled} onChange={(e) => onStress(Number(e.target.value))}>
        <option value={0}>Off</option>
        {[1, 2, 3, 5].map((ticks) => <option key={ticks} value={ticks}>{ticks} extra {ticks === 1 ? "tick" : "ticks"} in fast markets</option>)}
      </select>
    </label>}
    {(value === "queue" || (stress ?? 0) > 0) && <p className="text-xs text-muted">
      {(stress ?? 0) > 0 && "Stops reached while the spread is twice its normal width, or quotes change twice as fast as normal, slip the extra ticks. "}
      Queue fills and stop stress are program rules: a preset with either no longer counts as that preset.</p>}
  </>
}
