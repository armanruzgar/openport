import type { RiskWarning } from "../api/trading-types"
import { Badge } from "./ui"

const titles: Record<RiskWarning["code"], string> = {
  DELTA_LIMIT: "Over the delta limit", VEGA_LIMIT: "Over the vega limit", DELTA_HEADROOM: "Delta limit within reach",
  SOFT_FLOOR: "At the soft floor", SOFT_FLOOR_ROLLOVER: "Soft floor at rollover", FLOOR_RATCHET: "Floor rises tonight",
  SOFT_FLOOR_UNUSED: "Percent floor unused",
  EXPIRY_DELIVERY: "Delivery at expiry", EARLY_ASSIGNMENT: "Early assignment", EX_DIVIDEND: "Ex-dividend",
  WARNINGS_UNAVAILABLE: "Warnings unavailable",
}
/** The held book's warnings, most urgent first, as the server words them; nothing when there are none. */
export function RiskWarnings({ warnings }: { warnings?: readonly RiskWarning[] }) {
  if (!warnings?.length) return null
  return <section aria-label="Risk warnings" className="space-y-1.5 text-xs">
    {warnings.map((w, index) => <div key={`${w.code}-${w.scope}-${w.symbol ?? ""}-${index}`} role={w.severity === "warning" ? "alert" : "status"}
      className={`rounded-md border p-2 ${w.severity === "warning" ? "border-warn/50 bg-warn/5" : "border-border"}`}>
      <div className="flex flex-wrap items-center gap-2">
        <Badge tone={w.severity === "warning" ? "warn" : "neutral"}>{titles[w.code] ?? w.code}</Badge>
        <span className="text-muted">{w.scope === "aggregate" ? "Account" : w.scope}</span>
      </div>
      <p className="mt-1">{w.message}.</p>
    </div>)}
  </section>
}
