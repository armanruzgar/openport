import { useSyncExternalStore } from "react"
import { useLive } from "../api/live"
import type { NewOrder, Position } from "../api/trading-types"

/** A candidate adjustment: POST /api/orders/what-if takes 1–4 orders and a trimmed name of 1–64 UTF-8 bytes. */
export interface WhatIfDraft { id: string; name: string; orders: NewOrder[] }
export interface WhatIfState { candidates: WhatIfDraft[]; /** The candidate new orders join, or null for a new one. */ target: string | null }

// One list per account, kept in this browser only: a convenience, not account state.
const empty: WhatIfState = { candidates: [], target: null }
const states = new Map<string, WhatIfState>()
const listeners = new Set<() => void>()
const storageKey = (scope: string) => `openport.what-if.${scope || "main"}`
export const maxCandidates = 6
export const maxOrders = 4

export function whatIfNameError(name: string): string | null {
  const trimmed = name.trim()
  if (!trimmed) return "Enter a candidate name."
  return new TextEncoder().encode(trimmed).length > 64 ? "Use a name of 64 UTF-8 bytes or fewer." : null
}

const record = (value: unknown): value is Record<string, unknown> => typeof value === "object" && value !== null && !Array.isArray(value)
const side = (value: unknown) => value === "buy" || value === "sell"
/** An order the panel can show and send: one contract with a side, or two to four legs. */
function savedOrder(value: unknown): value is NewOrder {
  if (!record(value) || !Number.isSafeInteger(value.quantity) || (value.quantity as number) < 1) return false
  if (!(value.type === "market" || (value.type === "limit" && typeof value.limit_price === "string"))) return false
  if (value.legs === undefined) return typeof value.symbol === "string" && side(value.side)
  return Array.isArray(value.legs) && value.legs.length >= 2 && value.legs.length <= 4 && value.legs.every((leg) =>
    record(leg) && typeof leg.symbol === "string" && side(leg.side) && Number.isSafeInteger(leg.ratio) && (leg.ratio as number) >= 1)
}
function savedDraft(value: unknown): value is WhatIfDraft {
  return record(value) && typeof value.id === "string" && typeof value.name === "string" && Array.isArray(value.orders) &&
    value.orders.length >= 1 && value.orders.length <= maxOrders && value.orders.every(savedOrder)
}

/**
 * Another build or a damaged browser profile can leave anything in storage, and the
 * panel reads it while rendering: keep only candidates whose every field checks out.
 */
function load(scope: string): WhatIfState {
  const saved = states.get(scope)
  if (saved) return saved
  let state = empty
  try {
    const text = window.localStorage.getItem(storageKey(scope))
    const parsed: unknown = text ? JSON.parse(text) : null
    if (record(parsed) && Array.isArray(parsed.candidates)) {
      const candidates = parsed.candidates.filter(savedDraft).slice(0, maxCandidates)
      state = { candidates, target: candidates.some((c) => c.id === parsed.target) ? parsed.target as string : null }
    }
  } catch { /* storage unavailable or unreadable: start empty */ }
  states.set(scope, state)
  return state
}
function save(scope: string, state: WhatIfState) {
  states.set(scope, state)
  try { window.localStorage.setItem(storageKey(scope), JSON.stringify(state)) } catch { /* kept in memory only */ }
  listeners.forEach((listener) => listener())
}
function subscribe(listener: () => void) {
  listeners.add(listener)
  return () => { listeners.delete(listener) }
}
/** The order as a what-if keeps it: a client ID means nothing there, so none is sent. */
function stripped(order: NewOrder): NewOrder {
  return { ...order, client_order_id: "" }
}
let counter = 0
function nextId() { return `${Date.now().toString(36)}-${(counter++).toString(36)}` }

/** Adds `order` to the target candidate, or as a new one named `name`; false when there is no room. */
export function addWhatIfOrder(scope: string, order: NewOrder, name: string): boolean {
  const state = load(scope)
  const target = state.candidates.find((c) => c.id === state.target)
  if (target) {
    if (target.orders.length >= maxOrders) return false
    save(scope, { ...state, candidates: state.candidates.map((c) => c === target ? { ...c, orders: [...c.orders, stripped(order)] } : c) })
    return true
  }
  if (state.candidates.length >= maxCandidates) return false
  save(scope, { ...state, candidates: [...state.candidates, { id: nextId(), name, orders: [stripped(order)] }] })
  return true
}
/** A market order closing each position, as one candidate. */
export function closingOrders(positions: readonly Position[]): NewOrder[] {
  return positions.filter((p) => p.quantity !== 0).map((p) => ({
    client_order_id: "", symbol: p.symbol, side: p.quantity > 0 ? "sell" : "buy", type: "market", time_in_force: "ioc", quantity: Math.abs(p.quantity),
  }) as NewOrder)
}
export function addWhatIfCandidate(scope: string, name: string, orders: NewOrder[]): boolean {
  const state = load(scope)
  if (!orders.length || orders.length > maxOrders || state.candidates.length >= maxCandidates) return false
  save(scope, { ...state, candidates: [...state.candidates, { id: nextId(), name, orders: orders.map(stripped) }] })
  return true
}
export function updateWhatIf(scope: string, change: (state: WhatIfState) => WhatIfState) {
  const next = change(load(scope))
  const candidates = next.candidates.filter((c) => c.orders.length > 0)
  save(scope, { candidates, target: candidates.some((c) => c.id === next.target) ? next.target : null })
}
/** The what-if list's key: the account, live or in a replay. */
export function useWhatIfScope() {
  const { account, source } = useLive()
  return `${source}:${account}`
}
export function useWhatIf(scope: string): WhatIfState {
  return useSyncExternalStore(subscribe, () => load(scope), () => empty)
}
