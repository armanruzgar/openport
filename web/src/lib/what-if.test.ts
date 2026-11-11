// @vitest-environment jsdom
import { afterEach, expect, it } from "vitest"
import type { NewOrder } from "../api/trading-types"
import { addWhatIfOrder, updateWhatIf, type WhatIfState } from "./what-if"

afterEach(() => localStorage.clear())

const order = (symbol: string) => ({ client_order_id: "", symbol, side: "buy", type: "market", time_in_force: "ioc", quantity: 1 }) as NewOrder
const saved = (key: string) => JSON.parse(localStorage.getItem(key) ?? "null") as WhatIfState
/** What a save in another tab delivers to this one. */
function savedElsewhere(key: string, state: WhatIfState | null) {
  const text = state ? JSON.stringify(state) : null
  if (text) localStorage.setItem(key, text)
  window.dispatchEvent(new StorageEvent("storage", { key: state ? key : null, newValue: text, storageArea: localStorage }))
}

it("reads another tab's candidates before adding to the same list", () => {
  // Each tab saved its whole cached list, so whichever added last dropped the other's.
  const key = "openport.what-if.live:tabs"
  updateWhatIf("live:tabs", (state) => state)
  savedElsewhere(key, { candidates: [{ id: "b1", name: "From the other tab", orders: [order("SPXW  261016C07000000")] }], target: null })
  expect(addWhatIfOrder("live:tabs", order("SPXW  261016P06900000"), "From this tab")).toBe(true)
  expect(saved(key).candidates.map((c) => c.name)).toEqual(["From the other tab", "From this tab"])
  // Storage cleared in another tab empties this tab's lists too.
  localStorage.clear()
  savedElsewhere(key, null)
  let loaded: WhatIfState | undefined
  updateWhatIf("live:tabs", (state) => { loaded = state; return state })
  expect(loaded).toEqual({ candidates: [], target: null })
  // A save of another kind leaves the cached lists alone.
  expect(addWhatIfOrder("live:tabs", order("SPXW  261016C07000000"), "Kept")).toBe(true)
  localStorage.setItem(key, JSON.stringify({ candidates: [], target: null }))
  savedElsewhere("openport.analyze.SPX", { candidates: [], target: null })
  updateWhatIf("live:tabs", (state) => { loaded = state; return state })
  expect(loaded?.candidates.map((c) => c.name)).toEqual(["Kept"])
})
