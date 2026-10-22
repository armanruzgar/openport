import { useState } from "react"
import type { Bracket, Trigger } from "../api/trading-types"
import { roundNet } from "../lib/strategy"
import { useStopTrail } from "./TrailFields"
const netText = (net: number) => `$${Math.abs(net).toFixed(2)} ${net < 0 ? "credit" : "debit"}`

/** Closing net prices use the same signed debit convention as combo orders. */
export function spreadTarget(entry: number, percent: number, tick: number): string {
  return roundNet(-entry * percent / 100, tick).toFixed(2)
}

export function useSpreadExits(entry: number | null, tick: number, enabled = false, extended = false) {
  const [protect, setProtect] = useState(enabled)
  const [targetOn, setTargetOn] = useState(true)
  const [stopOn, setStopOn] = useState(true)
  // A typed percent holds for the side it was typed on: a debit entry takes profit above
  // 100% of its cost and a credit entry below, so a flip starts again from that side's default.
  const side = entry != null && entry > 0 ? "debit" : "credit"
  const [typed, setTyped] = useState<{ side: "debit" | "credit"; percent: string } | null>(null)
  const percent = typed?.side === side ? typed.percent : side === "debit" ? "150" : "50"
  const [source, setSource] = useState<"combo" | "underlying">("combo")
  const [direction, setDirection] = useState<Trigger["direction"]>("at_or_above")
  const [level, setLevel] = useState("")
  // A stop-limit closes at this signed net or better once its stop is reached.
  const [limitOn, setLimitOn] = useState(false)
  const [limit, setLimit] = useState("")
  const stopTrail = useStopTrail(source)
  const targetValid = entry != null && Number.isFinite(entry) && /^\d+(\.\d+)?$/.test(percent) && Number(percent) > 0
  const limitValid = !limitOn || (/^-?\d+(\.\d+)?$/.test(limit) && Number.isFinite(Number(limit)))
  const stopValid = /^-?\d+(\.\d+)?$/.test(level) && Number.isFinite(Number(level)) && (source === "combo" || Number(level) > 0) && limitValid && stopTrail.valid
  const valid = !protect || ((targetOn || stopOn) && (!targetOn || targetValid) && (!stopOn || stopValid))
  const target = targetValid ? spreadTarget(entry!, Number(percent), tick) : null
  const bracket: Bracket | undefined = protect && valid ? {
    ...(targetOn && target != null ? { take_profit: { limit_price: target } } : {}),
    ...(stopOn ? { stop_loss: { trigger: { source, direction, level, ...stopTrail.terms }, ...(limitOn ? { limit_price: limit } : {}) } } : {}),
  } : undefined
  const fields = <div className="space-y-3 text-xs">
    <label className="flex items-center gap-2"><input type="checkbox" checked={protect} onChange={(e) => setProtect(e.target.checked)} />Spread exits</label>
    {protect && <>
      <label className="flex items-center gap-2"><input type="checkbox" checked={targetOn} onChange={(e) => setTargetOn(e.target.checked)} />Take profit</label>
      {targetOn && <label className="trade-label">{side === "debit" ? "Receive (% of entry debit)" : "Buy back (% of entry credit)"}
        <input className="trade-input" inputMode="decimal" value={percent} onChange={(e) => setTyped({ side, percent: e.target.value })} />
        <span className="text-muted">Closing limit: {target == null ? "—" : netText(Number(target))}</span>
      </label>}
      <label className="flex items-center gap-2"><input type="checkbox" checked={stopOn} onChange={(e) => setStopOn(e.target.checked)} />Stop loss</label>
      {stopOn && <>
        <label className="trade-label">Stop source<select className="trade-input" value={source} onChange={(e) => setSource(e.target.value as "combo" | "underlying")}>
          <option value="combo">Closing combo net</option><option value="underlying">Underlying</option>
        </select></label>
        <label className="trade-label">Stop direction<select className="trade-input" value={direction} onChange={(e) => setDirection(e.target.value as Trigger["direction"])}>
          <option value="at_or_above">At or above</option><option value="at_or_below">At or below</option>
        </select></label>
        <label className="trade-label">Stop level<input className="trade-input" inputMode="decimal" value={level} onChange={(e) => setLevel(e.target.value)} /></label>
        <label className="flex items-center gap-2"><input type="checkbox" checked={limitOn} onChange={(e) => { setLimitOn(e.target.checked); if (e.target.checked && !limit && source === "combo") setLimit(level) }} />Stop-limit</label>
        {limitOn && <label className="trade-label">Closing net limit (negative to receive)<input className="trade-input" inputMode="decimal" value={limit} onChange={(e) => setLimit(e.target.value)} />
          <span className="text-muted">Limit: {limitValid && limit ? netText(Number(limit)) : "—"}</span></label>}
        {stopTrail.fields}
        <p className="text-muted">Combo net is the displayed cost to close: positive to pay, negative to receive. A leg nobody bids for is bought back at its ask or given away at $0.00. {limitOn
          ? "A reached stop-limit rests as a closing GTC combo limit at its net, on the combo tick, and waits if the market gaps through it."
          : extended ? "Outside regular hours a reached stop executes once at each leg’s touch, with displayed size only."
          : "A reached stop sends a market IOC combo with the plan’s slippage."}</p>
      </>}
      <p className="text-muted">Exits close all legs together, {extended ? "work in every product session" : "wait outside the regular session"}, and expire at the nearest leg’s last trade or auto-close. An exit filling completely cancels the other; a stop that fills only in part re-arms for the units left.</p>
    </>}
  </div>
  return { bracket, valid, fields, setLimitOn }
}
