import { useState } from "react"
import type { Walk } from "../api/trading-types"

/**
 * Server validation remains authoritative for product ticks and price bands. Until the
 * trader types them (or an existing walk sets them), the step and cap are the caller's
 * current ones: a ticket's side, quote and legs can change after it opens.
 */
export function useWalk(allowed: boolean, start: string, up: boolean, step: string, cap: string, initial?: Walk | null) {
  const [enabled, setEnabled] = useState(!!initial)
  const [typedStep, setIncrement] = useState<string | null>(initial?.step ?? null)
  const [seconds, setSeconds] = useState(String(initial?.seconds ?? 10))
  const [typedLimit, setLimit] = useState<string | null>(initial?.limit ?? null)
  const increment = typedStep ?? step
  const limit = typedLimit ?? cap
  const active = allowed && enabled
  const decimal = (s: string) => /^-?\d+(\.\d{1,6})?$/.test(s) && Number.isFinite(Number(s))
  const distance = (Number(limit) - Number(start)) * (up ? 1 : -1)
  const valid = !active || (decimal(start) && decimal(increment) && Number(increment) > 0 && decimal(limit) &&
    /^\d+$/.test(seconds) && Number(seconds) >= 1 && Number(seconds) <= 3600 && distance >= 0 && distance / Number(increment) <= 1000 + 1e-8)
  const walk: Walk | undefined = active && valid ? { step: increment, seconds: Number(seconds), limit } : undefined
  const fields = allowed ? <div className="col-span-2 space-y-2 rounded-md border border-border p-3">
    <label className="flex items-center justify-between gap-2 text-xs">
      Walk limit<input type="checkbox" role="switch" aria-label="Walk limit" checked={enabled} onChange={(e) => setEnabled(e.target.checked)} />
    </label>
    {enabled && <>
      <div className="grid grid-cols-3 gap-2">
        <label className="trade-label">Walk step ($)<input className="trade-input" inputMode="decimal" value={increment} onChange={(e) => setIncrement(e.target.value)} required /></label>
        <label className="trade-label">Every (seconds)<input className="trade-input" type="number" min="1" max="3600" step="1" value={seconds} onChange={(e) => setSeconds(e.target.value)} required /></label>
        <label className="trade-label">Walk cap ($)<input className="trade-input" inputMode="decimal" value={limit} onChange={(e) => setLimit(e.target.value)} required /></label>
      </div>
      <p className="text-[11px] text-muted">Moves {up ? "up" : "down"} on market time until filled or at the cap, then rests. Net credits use negative prices and move toward zero. Exits keep their own prices.</p>
      {!valid && <p role="status" className="text-xs text-warn">Use a positive step, 1–3600 seconds, and a cap toward the market within 1000 steps.</p>}
    </>}
  </div> : null
  return { walk, fields, valid }
}
