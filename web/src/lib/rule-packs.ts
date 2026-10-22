import { useCallback, useEffect, useState } from "react"
import type { AccountRules, Plan, RulePack } from "../api/trading-types"

/**
 * Imported rule packs, kept in this browser only: the server validates a pack and
 * starting an attempt from one of its plans sends its rules as custom rules, which
 * the server checks again. Built-in presets stay separate and illustrative.
 */
const key = "openport.rule-packs"
export const MAX_PACKS = 20

function shaped(value: unknown): value is RulePack {
  const pack = value as RulePack
  return !!pack && pack.format === "openport.rule-pack" && pack.version === 1 && typeof pack.id === "string" &&
    typeof pack.name === "string" && Array.isArray(pack.plans) &&
    pack.plans.every((plan) => plan && typeof plan.id === "string" && typeof plan.name === "string" && typeof plan.rules === "object")
}
export function loadRulePacks(): RulePack[] {
  try {
    const saved: unknown = JSON.parse(localStorage.getItem(key) ?? "[]")
    return Array.isArray(saved) ? saved.filter(shaped).slice(0, MAX_PACKS) : []
  } catch { return [] }
}
export function saveRulePacks(packs: RulePack[]) {
  try { localStorage.setItem(key, JSON.stringify(packs.slice(0, MAX_PACKS))) } catch { /* The current session keeps them. */ }
  if (typeof window !== "undefined") window.dispatchEvent(new Event("openport:rule-packs"))
}
/** Adds or replaces (by ID) a pack the server returned normalized. */
export function withPack(packs: RulePack[], pack: RulePack): RulePack[] {
  return [...packs.filter((p) => p.id !== pack.id), pack].slice(-MAX_PACKS)
}
export function useRulePacks() {
  const [packs, setPacks] = useState(loadRulePacks)
  useEffect(() => {
    const reload = () => setPacks(loadRulePacks())
    window.addEventListener("openport:rule-packs", reload)
    window.addEventListener("storage", reload)
    return () => { window.removeEventListener("openport:rule-packs", reload); window.removeEventListener("storage", reload) }
  }, [])
  const update = useCallback((next: RulePack[]) => { setPacks(next); saveRulePacks(next) }, [])
  return [packs, update] as const
}

/** The JSON text an export writes; it validates back to the same pack. */
export function packFile(pack: RulePack): string { return JSON.stringify(pack, null, 2) + "\n" }

/** A pack's plans as plan choices, for starting a custom attempt from one. */
export const PACK_PREFIX = "pack:"
export function packPlans(packs: RulePack[]): Plan[] {
  return packs.flatMap((pack) => pack.plans.map((plan): Plan => ({
    id: `${PACK_PREFIX}${pack.id}:${plan.id}`, name: plan.name,
    summary: `${pack.name}, effective ${pack.effective_from}. ${plan.summary ?? ""}`.trim(),
    initial_cash: plan.initial_cash, rules: { ...plan.rules, plan: plan.name, plan_id: null } as AccountRules,
    phase: plan.rules.phase, unlocked_by: null,
  })))
}
export const isPackPlan = (id: string) => id.startsWith(PACK_PREFIX)
