import { useEffect, useState } from "react"
import type { MarketSession, TradingSession, Num } from "../api/types"
import { marketBadge, snapshotFreshness } from "../lib/freshness"

export function AsOf({ asOf, delaySeconds = 0, market, session, showBadge = true, now: clock, frozenSince }: {
  asOf: string | null | undefined
  delaySeconds?: Num
  market?: MarketSession | null
  session?: TradingSession | null
  showBadge?: boolean
  /** The market's clock when it is not the wall's: a replay's. */
  now?: number
  /** When the connection to the server was lost (wall clock): the snapshot ages from there on the wall clock. */
  frozenSince?: number | null
}) {
  const [wall, setNow] = useState(Date.now)
  // A market clock stops with the feed; what is on screen keeps aging in real time.
  const now = frozenSince != null ? (clock ?? frozenSince) + Math.max(0, wall - frozenSince) : clock ?? wall
  useEffect(() => {
    // Closed feeds still age even when neither REST nor the socket emits changes.
    if (frozenSince != null) setNow(Date.now())
    const timer = window.setInterval(() => setNow(Date.now()), frozenSince != null ? 5_000 : 30_000)
    return () => window.clearInterval(timer)
  }, [frozenSince])
  const { label, stale, ageSeconds } = snapshotFreshness(asOf, delaySeconds, now)
  const badge = showBadge ? marketBadge(session ?? market, stale) : null
  return (
    <span className="inline-flex flex-wrap items-center gap-1.5 text-[11px] text-muted">
      <time dateTime={ageSeconds == null ? undefined : asOf ?? undefined}>{label}</time>
      {badge && (
        <span className={`rounded-full border px-1.5 py-0.5 font-medium ${badge.tone === "warn" ? "border-warn text-warn" : "border-border text-muted"}`} title={badge.title}>
          {badge.label}
        </span>
      )}
    </span>
  )
}
