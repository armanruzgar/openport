import { useQuery } from "@tanstack/react-query"
import { api } from "../api/client"
import { marketNow, useLive } from "../api/live"
import { count, fixed, isNum, price } from "../lib/format"
import { matchingPayload } from "../lib/payload"
import { providerLabel } from "../lib/provider"
import { useTheme } from "../lib/theme"
import { AlertsButton } from "./Alerts"
import { AsOf } from "./AsOf"
import { Flash } from "./Flash"
import { FeedBadge } from "./ui"

export function Header({
  symbol,
  onSymbol,
  onMenu,
}: {
  symbol: string | null
  onSymbol: (symbol: string) => void
  /** Opens the navigation drawer on narrow screens. */
  onMenu?: () => void
}) {
  const live = useLive()
  const { status, tick, connection, version, market, underlyings } = live
  const { theme, toggleTheme } = useTheme()
  const current = underlyings.find((u) => u.symbol === symbol)
  const feed = tick?.feed ?? status?.feed
  const provider = status?.provider
  const engine = tick?.engine ?? status?.engine
  const summary = useQuery({
    queryKey: ["summary", symbol, symbol ? version(symbol) : 0],
    queryFn: ({ signal }) => api.summary(symbol as string, signal),
    enabled: symbol != null && current?.as_of != null,
    placeholderData: (previous) => symbol ? matchingPayload(previous, symbol) : undefined,
  })
  const summaryData = symbol ? matchingPayload(summary.data, symbol) : undefined
  const inferredSpot = summaryData?.version === current?.version && summaryData?.spot_source === "parity" && isNum(current?.spot)
  const dataLabel = provider ? providerLabel(provider, feed?.state) : ""

  return (
    <header className="relative z-20 border-b border-border bg-panel/95 backdrop-blur lg:sticky lg:top-0">
      <div className="flex flex-wrap items-center gap-x-5 gap-y-2 px-4 py-2">
        {onMenu && (
          <button type="button" onClick={onMenu} aria-label="Open navigation"
            className="flex h-8 w-8 items-center justify-center rounded-md border border-border text-muted hover:bg-raised hover:text-foreground lg:hidden">
            <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" aria-hidden="true"><path d="M4 7h16M4 12h16M4 17h16" /></svg>
          </button>
        )}

        <nav className="flex min-w-0 flex-wrap items-center gap-1" aria-label="Underlyings">
          {underlyings.map((u) => (
            <button
              key={u.symbol}
              onClick={() => onSymbol(u.symbol)}
              className={`min-w-0 rounded-md px-2 py-1 text-sm tabular [overflow-wrap:anywhere] ${u.symbol === symbol ? "bg-raised text-foreground" : "text-muted hover:text-foreground"}`}
            >
              {u.symbol}
            </button>
          ))}
        </nav>

        {current && (
          <div className="flex min-w-0 flex-wrap items-baseline gap-2">
            <span title={inferredSpot ? "No underlying quote from this provider; spot inferred from put-call parity" : undefined}>
              <Flash key={symbol} value={current.spot} className="px-1 text-lg tabular">
                {inferredSpot ? "≈" : ""}{price(current.spot)}
              </Flash>
            </span>
            <span className="min-w-0 text-[11px] text-muted">as of <AsOf asOf={current.as_of} delaySeconds={provider?.delay_seconds} market={market} session={current.session}
              now={live.source === "replay" || provider?.simulated ? marketNow(live) : undefined} /></span>
          </div>
        )}

        <div className="ml-auto flex min-w-0 flex-wrap items-center gap-3 text-[11px] text-muted">
          {provider && (
            <span className={`${provider.simulated ? "" : "hidden sm:inline "}min-w-0 [overflow-wrap:anywhere]`} title={`${dataLabel}${provider.realtime_plan_dependent ? " · based on the current feed state" : ""}`}>
              <span className="text-foreground">{provider.name}</span> {dataLabel}
            </span>
          )}
          {feed && <FeedBadge state={feed.state} message={feed.message} />}
          {engine && (
            <span className="hidden tabular md:inline" title="contracts tracked · events per second · last analytics pass">
              {count(engine.contracts)} contracts · {fixed(engine.analytics_ms, 1)} ms
            </span>
          )}
          {connection !== "open" && <span className="text-warn">reconnecting…</span>}
          <AlertsButton />
          <button
            type="button"
            aria-label={`Switch to ${theme === "dark" ? "light" : "dark"} theme`}
            title={`Switch to ${theme === "dark" ? "light" : "dark"} theme`}
            onClick={toggleTheme}
            className="flex h-8 w-8 items-center justify-center rounded-md border border-border text-muted hover:bg-raised hover:text-foreground"
          >
            <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
              {theme === "dark" ? (
                <>
                  <circle cx="12" cy="12" r="4" />
                  <path d="M12 2v2m0 16v2M2 12h2m16 0h2M5 5l1.5 1.5m11 11L19 19M5 19l1.5-1.5m11-11L19 5" />
                </>
              ) : <path d="M20.5 13A8.5 8.5 0 0 1 11 3.5 8.5 8.5 0 1 0 20.5 13Z" />}
            </svg>
          </button>
        </div>
      </div>
    </header>
  )
}
