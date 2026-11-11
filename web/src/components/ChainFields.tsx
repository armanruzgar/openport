import { useState, type ReactNode } from "react"
import type { ChainedOrder, OrderChain, Side, Trigger } from "../api/trading-types"
import { crossDirection, describePending } from "../lib/ticket"
import { validMoney } from "../lib/trading"
import { Segmented } from "./ui"

export type ChainKind = "none" | "then" | "oco"

/** The order chained to a ticket's order, and the fields that set it. */
export interface OrderChainFields {
  terms: OrderChain
  valid: boolean
  fields: ReactNode
}

/**
 * A second limit order on the same contract, chained to the ticket's order:
 * placed once it fills completely (then), or working beside it until either
 * fills (oco). It may wait for the underlying to cross a level.
 */
export function useChainedOrder({ symbol, side, quantity, underlying, spot }: {
  symbol: string; side: Side; quantity: number; underlying: string; spot: number | null
}): OrderChainFields {
  const [kind, setKind] = useState<ChainKind>("none")
  const [chosenSide, setSide] = useState<Side | null>(null)
  const [size, setSize] = useState("")
  const [price, setPrice] = useState("")
  const [tif, setTif] = useState<"day" | "gtc">("gtc")
  const [level, setLevel] = useState("")
  const [chosenDirection, setDirection] = useState<Trigger["direction"] | null>(null)
  // A target after the entry sells what it bought; an alternative entry trades the same way.
  const chainedSide = chosenSide ?? (kind === "then" ? (side === "buy" ? "sell" : "buy") : side)
  const count = size === "" ? quantity : Number(size)
  const priceValid = validMoney(price) && Number(price) > 0
  // The level is crossed from where spot is now; with no spot yet, the trader says which way.
  const priced = spot != null && Number.isFinite(spot)
  const direction = priced ? crossDirection(Number(level), spot) : chosenDirection
  const levelValid = level === "" || (validMoney(level) && Number(level) > 0 && direction != null)
  const countValid = Number.isSafeInteger(count) && count > 0
  const trigger: Trigger | null = level && direction ? { source: "underlying", direction, level } : null
  const order: ChainedOrder | null = kind !== "none" && priceValid && levelValid && countValid ? {
    symbol, side: chainedSide, quantity: count, type: "limit", time_in_force: tif, limit_price: price, ...(trigger ? { trigger } : {}),
  } : null
  const terms: OrderChain = order ? (kind === "then" ? { then: order } : { oco: order }) : {}
  const valid = kind === "none" || order != null
  const described = order ? describePending({ ...order, legs: null, limit_price: price, good_till: null, walk: null, group: null, trigger, bracket: null, then: null, oco: null, symbol, side: chainedSide }, underlying) : ""
  const fields = <div className="space-y-2">
    <div className="trade-label">Chain
      <Segmented label="Chain" value={kind} onChange={setKind}
        options={[{ value: "none", label: "None" }, { value: "then", label: "Then, once filled" }, { value: "oco", label: "Or, one cancels other" }]} />
    </div>
    {kind !== "none" && <>
      <div className="grid grid-cols-2 gap-2">
        <label className="trade-label">Chained side<select className="trade-input" value={chainedSide} onChange={(e) => setSide(e.target.value as Side)}>
          <option value="buy">Buy</option><option value="sell">Sell</option>
        </select></label>
        <label className="trade-label">Chained quantity<input className="trade-input" inputMode="numeric" value={size} placeholder={String(quantity)} onChange={(e) => setSize(e.target.value)} /></label>
        <label className="trade-label">Chained limit ($)<input className="trade-input" inputMode="decimal" value={price} onChange={(e) => setPrice(e.target.value)} /></label>
        <label className="trade-label">Chained time in force<select className="trade-input" value={tif} onChange={(e) => setTif(e.target.value as "day" | "gtc")}>
          <option value="gtc">GTC</option><option value="day">Day</option>
        </select></label>
      </div>
      <label className="trade-label">Only when {underlying} crosses (optional)<input className="trade-input" inputMode="decimal" value={level}
        placeholder={spot != null ? spot.toFixed(2) : undefined} onChange={(e) => setLevel(e.target.value)} /></label>
      {level !== "" && !priced && <label className="trade-label">Arms when {underlying} is<select className="trade-input" value={chosenDirection ?? ""}
        onChange={(e) => setDirection(e.target.value === "" ? null : e.target.value as Trigger["direction"])}>
        <option value="">Choose…</option><option value="at_or_above">At or above the level</option><option value="at_or_below">At or below the level</option>
      </select></label>}
      <span className="text-[11px] text-muted">{!priceValid ? "Enter the chained order's limit price."
        : !countValid ? "Enter a whole number of contracts."
        : !levelValid ? (validMoney(level) && Number(level) > 0 ? `${underlying} has no price yet: choose whether the order arms at or above the level, or at or below it.` : `Enter a positive ${underlying} level, or leave it empty.`)
        : kind === "then" ? `Once this order fills completely, places ${described}, checked then like any new order. If this order ends any other way, nothing is placed.`
        : `${trigger ? "Arms" : "Works"} beside this order: ${described}. The first fill of either cancels the other; both reserve buying power until then.`}</span>
    </>}
  </div>
  return { terms, valid, fields }
}
