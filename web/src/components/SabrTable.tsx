import type { Surface, SurfaceExpiry } from "../api/types"
import { expiryLabel, fixed } from "../lib/format"
import { smileColor } from "../lib/svi"

/** The optional SABR overlay's per-expiry parameters and diagnostics. */
export function SabrTable({ expiries, violations, beta }: { expiries: SurfaceExpiry[]; violations: NonNullable<Surface["sabr_calendar_violations"]>; beta: Surface["sabr_beta"] }) {
  if (!expiries.length || beta == null) return null
  return (
    <div className="mt-4 border-t border-border pt-3">
      <h3 className="mb-2 text-xs font-medium text-muted">SABR overlay · Hagan lognormal, {beta === "fit" ? "β calibrated" : `β fixed at ${fixed(beta, 2)}`} · RMSE in vol points</h3>
      <p className="mb-2 text-[11px] text-muted">An asymptotic expansion fitted per expiry for comparison; it is drawn only over each fit's quoted range and plays no part in the chain's analytics.</p>
      <table className="w-full table-fixed text-[11px]" aria-label="SABR parameters and arbitrage diagnostics">
        <thead className="text-muted">
          <tr>
            <th className="w-[25%] pb-1 text-left font-normal">Expiry / checks</th>
            <th className="w-[45%] pb-1 text-left font-normal">α · β · ρ · ν</th>
            <th className="pb-1 text-right font-normal">RMSE / points</th>
          </tr>
        </thead>
        <tbody>
          {expiries.map((e, i) => {
            const fit = e.sabr
            const pairs = violations.filter((p) => p.earlier === e.id || p.later === e.id)
            return (
              <tr key={e.id} className="border-t border-border/50 align-top">
                <td className="py-2 pr-2">
                  <span className="break-words" style={{ color: smileColor(i) }}>{expiryLabel(e.id, true)}</span>
                  <div className="mt-1 flex flex-wrap gap-1">
                    {fit && !fit.butterfly_ok && <span className="rounded border border-danger px-1 text-danger" title={`min g ${fixed(fit.butterfly_min_g, 5)} at k ${fixed(fit.butterfly_k, 4)}`}>Butterfly violation</span>}
                    {pairs.map((p) => <span key={`${p.earlier}-${p.later}`} className="rounded border border-danger px-1 text-danger">
                      calendar: {p.vol_points == null ? "violation" : `${fixed(p.vol_points, 1)} vp`} at k={fixed(p.k, 2)} vs {expiryLabel(p.earlier === e.id ? p.later : p.earlier)}
                    </span>)}
                    {fit?.butterfly_ok && !pairs.length && <span className="text-muted">Grid checks pass</span>}
                    {!fit && <span className="text-warn">{e.sabr_status === "too_few_points" ? "Too few points" : "Fit failed"}</span>}
                  </div>
                </td>
                <td className="py-2 pr-2">
                  {fit ? <div className="flex flex-wrap gap-x-2 gap-y-0.5 tabular">
                    <span>α {fixed(fit.alpha, 5)}</span><span>β {fixed(fit.beta, 3)}</span>
                    <span>ρ {fixed(fit.rho, 4)}</span><span>ν {fixed(fit.nu, 4)}</span>
                  </div> : <span className="break-words text-muted">{e.sabr_reason ?? "No calibration available."}</span>}
                </td>
                <td className="py-2 text-right tabular">
                  <div>{fixed(fit?.rmse_vol_points, 4)}</div>
                  <div className="text-muted">{fit?.points ?? "—"} pts</div>
                </td>
              </tr>
            )
          })}
        </tbody>
      </table>
    </div>
  )
}
